// b3core.cpp -- ERD-241 exact branch and bound core, C++17, standalone binary.
//
// This is a 1:1 port of search_free() from b3search.py (the source of truth).
// It computes: the valid B_3 set of size k with min = 1, max <= cap and the
// SMALLEST possible maximum, or reports that none exists.
//
// Representation: the three big-integer bitmasks of the Python version become
// uint64 word arrays.  b (the set itself) is kept as a plain sorted int array S,
// because every place the Python code needs `b` it needs it shifted by y or 2y
// and OR-ed / tested -- which over a k-element set is k bit probes instead of a
// full word loop.
//
//   Python                          here
//   b   = bitmask of S              S[0..ns)
//   b2  = bitmask of pair sums      b2[0..W)
//   b3  = bitmask of triple sums    b3[0..W)
//
// _ok(b, b2, b3, y) is true iff, with t1 = b2<<y, t2 = b<<2y, t3 = 1<<3y:
//   multiset:  t1&t2 == 0 and t1&t3 == 0 and (t1|t2|t3)&b3 == 0
//   distinct:  t1&b3 == 0
// which is exactly what ok_add() below tests, term by term:
//   t1&t3 != 0  <=>  bit 2y set in b2
//   t3&b3 != 0  <=>  bit 3y set in b3
//   t1&t2 != 0  <=>  exists a in S with bit y+a set in b2
//   t2&b3 != 0  <=>  exists a in S with bit 2y+a set in b3
//   t1&b3 != 0  <=>  (b2<<y) & b3 != 0            (word loop)
//
// MODES
//   --mode compat     single thread, strict improvement (accept y < B), first
//                     solution found wins.  Reproduces b3search.py exactly,
//                     including the node count.  Used by the equivalence harness.
//   --mode canonical  bounds relaxed by one unit (accept y <= B) so that EVERY
//                     optimal set is enumerated, and the answer is the
//                     lexicographically smallest one.  This makes the result
//                     independent of thread count / exploration order.
//                     Determinism argument: the lex-smallest optimal set L is
//                     never cut by the reflection bound (if a set has first gap
//                     > last gap its reflection has a smaller second element,
//                     hence is lex-smaller, so L always has first gap <= last
//                     gap), and no other bound can cut a set with max <= B
//                     because B >= M = the optimum at all times.  So L is always
//                     recorded; min over any recorded superset of {L} is L.
//
// Output: one JSON object on stdout.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
static int W = 0;          // uint64 words per mask
static int CAP = 0;
static int KK = 0;
static bool MULTI = true;
static int SLACK = 0;      // 0 = strict (compat), 1 = canonical
static bool CANON = false;
static bool PARANOID = false;
static std::vector<int> GG; // GG[j] = proven g(j), j = 1..k-1

static double T_BUDGET = 0.0;   // absolute epoch deadlines, 0 = none
static double T_BLOCK = 0.0;

static std::atomic<int> STATUS{0};   // 0 ok, 1 budget timeout, 2 block timeout
static std::atomic<long long> NODES{0};

struct BestT {
    std::mutex m;
    std::vector<int> set;
    bool have = false;
    std::atomic<int> B{INT_MAX};
};
static BestT BEST;

static inline double now_s() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static inline bool getbit(const uint64_t *m, int p) {
    return (m[p >> 6] >> (p & 63)) & 1ULL;
}
static inline void setbit(uint64_t *m, int p) {
    m[p >> 6] |= 1ULL << (p & 63);
}

// (a << sh) & b  != 0 ?
static inline bool shl_and_any(const uint64_t *a, const uint64_t *b, int sh) {
    const int q = sh >> 6, r = sh & 63;
    if (r == 0) {
        for (int i = q; i < W; i++)
            if (a[i - q] & b[i]) return true;
    } else {
        uint64_t prev = 0;
        for (int i = q; i < W; i++) {
            const uint64_t cur = a[i - q];
            if (((cur << r) | (prev >> (64 - r))) & b[i]) return true;
            prev = cur;
        }
    }
    return false;
}

// dst |= (a << sh)
static inline void shl_or(uint64_t *dst, const uint64_t *a, int sh) {
    const int q = sh >> 6, r = sh & 63;
    if (r == 0) {
        for (int i = W - 1; i >= q; i--) dst[i] |= a[i - q];
    } else {
        for (int i = W - 1; i >= q; i--) {
            uint64_t v = a[i - q] << r;
            if (i > q) v |= a[i - q - 1] >> (64 - r);
            dst[i] |= v;
        }
    }
}

// _ok(): is S + [y] still valid?
static inline bool ok_add(const int *S, int ns, const uint64_t *b2,
                          const uint64_t *b3, int y) {
    if (MULTI) {
        if (getbit(b3, 3 * y)) return false;   // t3 & b3
        if (getbit(b2, 2 * y)) return false;   // t1 & t3
        const int y2 = 2 * y;
        for (int i = 0; i < ns; i++) {
            const int a = S[i];
            if (getbit(b2, y + a)) return false;    // t1 & t2
            if (getbit(b3, y2 + a)) return false;   // t2 & b3
        }
    }
    return !shl_and_any(b2, b3, y);            // t1 & b3
}

// _advance(): new (b2, b3, dup) after adding y to S.
static inline void advance(const int *S, int ns, const uint64_t *b2,
                           const uint64_t *b3, int y, uint64_t *nb2,
                           uint64_t *nb3, bool *dup) {
    memcpy(nb3, b3, sizeof(uint64_t) * (size_t)W);
    shl_or(nb3, b2, y);                                // | (b2 << y)
    memcpy(nb2, b2, sizeof(uint64_t) * (size_t)W);
    bool d = false;
    if (MULTI) {
        const int y2 = 2 * y;
        for (int i = 0; i < ns; i++) setbit(nb3, y2 + S[i]);   // | (b << 2y)
        setbit(nb3, 3 * y);                                    // | (1 << 3y)
        for (int i = 0; i < ns; i++) {                         // newp = (b<<y)|(1<<2y)
            const int p = y + S[i];
            if (getbit(b2, p)) d = true;
            setbit(nb2, p);
        }
        if (getbit(b2, y2)) d = true;
        setbit(nb2, y2);
    } else {
        for (int i = 0; i < ns; i++) {                         // newp = b << y
            const int p = y + S[i];
            if (getbit(b2, p)) d = true;
            setbit(nb2, p);
        }
    }
    *dup = d;
}

// ---------------------------------------------------------------------------
struct Arena {
    std::vector<uint64_t> mb2, mb3;
    std::vector<int> mcand;
    int cc;
    int S[160];
    int ns = 0;
    long long nodes = 0;
    Arena(int levels, int cc_) : cc(cc_) {
        mb2.assign((size_t)levels * W, 0);
        mb3.assign((size_t)levels * W, 0);
        mcand.assign((size_t)levels * cc_, 0);
    }
    uint64_t *B2(int d) { return &mb2[(size_t)d * W]; }
    uint64_t *B3(int d) { return &mb3[(size_t)d * W]; }
    int *CANDB(int d) { return &mcand[(size_t)d * cc]; }
};

static void record(Arena &A, int y) {
    if (!CANON) {                       // compat: strict, single threaded
        BEST.set.assign(A.S, A.S + A.ns);
        BEST.set.push_back(y);
        BEST.have = true;
        BEST.B.store(y);
        return;
    }
    std::vector<int> s(A.S, A.S + A.ns);
    s.push_back(y);
    std::lock_guard<std::mutex> lk(BEST.m);
    if (!BEST.have || y < BEST.set.back() ||
        (y == BEST.set.back() && s < BEST.set)) {
        BEST.set = s;
        BEST.have = true;
    }
    if (y < BEST.B.load()) BEST.B.store(y);
}

static void dfs(Arena &A, int depth, const int *cand, int m, int need,
                const uint64_t *b2, const uint64_t *b3, int a2) {
    A.nodes++;
    if (!(A.nodes & 255)) {
        const double t = now_s();
        if (T_BUDGET > 0.0 && t > T_BUDGET) STATUS.store(1);
        else if (T_BLOCK > 0.0 && t > T_BLOCK) STATUS.store(2);
    }
    if (STATUS.load(std::memory_order_relaxed)) return;
    if (m < need) return;
    const int Bv = BEST.B.load(std::memory_order_relaxed);
    if (need == 1) {
        const int y = cand[0];          // cand is sorted: the first one is the best
        if (y < Bv + SLACK) record(A, y);
        return;
    }
    for (int i = 1; i <= need; i++) {   // Hall-type span bound over open positions
        const int thr = Bv - GG[need - i + 1] + SLACK;
        if ((int)(std::upper_bound(cand, cand + m, thr) - cand) < i) return;
    }
    int limit = Bv - GG[need] + SLACK;
    if (a2 && Bv - a2 + SLACK < limit) limit = Bv - a2 + SLACK;   // reflection cut

    uint64_t *nb2 = A.B2(depth + 1);
    uint64_t *nb3 = A.B3(depth + 1);
    int *sub = A.CANDB(depth + 1);

    for (int i = 0; i < m; i++) {
        if (m - i < need) return;
        const int y = cand[i];
        if (y > limit) return;
        if (PARANOID && !ok_add(A.S, A.ns, b2, b3, y)) {
            fprintf(stderr, "PARANOID: filtered candidate infeasible\n");
            exit(9);
        }
        bool dup = false;
        advance(A.S, A.ns, b2, b3, y, nb2, nb3, &dup);
        A.S[A.ns++] = y;
        int sm = 0;
        if (!dup)
            for (int j = i + 1; j < m; j++)
                if (ok_add(A.S, A.ns, nb2, nb3, cand[j])) sub[sm++] = cand[j];
        dfs(A, depth + 1, sub, sm, need - 1, nb2, nb3, a2 ? a2 : y);
        A.ns--;
        if (STATUS.load(std::memory_order_relaxed)) return;
        const int Bn = BEST.B.load(std::memory_order_relaxed);
        limit = Bn - GG[need] + SLACK;
        if (a2 && Bn - a2 + SLACK < limit) limit = Bn - a2 + SLACK;
    }
}

// ---------------------------------------------------------------------------
// Parallel driver: cut the tree at depth `tdepth` into independent tasks.
struct Task {
    std::vector<int> pre;              // elements chosen after the fixed 1
    std::vector<int> cand;             // candidate list at the task root
    std::vector<uint64_t> b2, b3;
    int need, a2;
};

static const uint64_t FNV_OFFSET = 14695981039346656037ULL;
static const uint64_t FNV_PRIME = 1099511628211ULL;

static void fnv_u64(uint64_t &h, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        h ^= (value >> (8 * i)) & 0xffULL;
        h *= FNV_PRIME;
    }
}

static void fnv_prefix(uint64_t &h, uint64_t id, const std::vector<int> &pre) {
    fnv_u64(h, id);
    fnv_u64(h, pre.size());
    for (int value : pre) fnv_u64(h, (uint64_t)value);
}

struct ShardCollector {
    int shard_index;
    int shard_count;
    Arena worker;
    long long prefix_count = 0;
    long long split_nodes = 0;
    uint64_t catalog_hash = FNV_OFFSET;
    uint64_t processed_hash = FNV_OFFSET;
    std::vector<long long> prefix_ids;
    std::vector<std::vector<int>> prefixes;

    ShardCollector(int index, int count, int levels, int cc)
        : shard_index(index), shard_count(count), worker(levels, cc) {}

    void accept(const std::vector<int> &pre, const int *cand, int m, int need,
                const uint64_t *b2, const uint64_t *b3, int a2) {
        const long long id = prefix_count++;
        fnv_prefix(catalog_hash, (uint64_t)id, pre);
        if (id % shard_count != shard_index) return;

        fnv_prefix(processed_hash, (uint64_t)id, pre);
        prefix_ids.push_back(id);
        prefixes.push_back(pre);

        worker.ns = 0;
        worker.S[worker.ns++] = 1;
        for (int value : pre) worker.S[worker.ns++] = value;
        memcpy(worker.B2(0), b2, sizeof(uint64_t) * (size_t)W);
        memcpy(worker.B3(0), b3, sizeof(uint64_t) * (size_t)W);
        int *dst = worker.CANDB(0);
        if (m > 0) memcpy(dst, cand, sizeof(int) * (size_t)m);
        dfs(worker, 0, dst, m, need, worker.B2(0), worker.B3(0), a2);
    }
};

// Generate the same prefix frontier independently in every process.  split_bound
// is intentionally immutable: discoveries in one shard must not change which
// prefixes exist in that shard or in any other shard.
static void gen_shard_tasks(Arena &A, int depth, const int *cand, int m, int need,
                            const uint64_t *b2, const uint64_t *b3, int a2,
                            int tdepth, int split_bound, std::vector<int> &pre,
                            ShardCollector &out) {
    if (STATUS.load(std::memory_order_relaxed)) return;
    if (depth == tdepth) {
        out.accept(pre, cand, m, need, b2, b3, a2);
        return;
    }

    // This call is a real DFS node above the frontier.  It is reported once,
    // by shard zero, so summing complete no-witness shards reproduces the
    // unsharded node count exactly.
    out.split_nodes++;
    if (!(out.split_nodes & 255)) {
        const double t = now_s();
        if (T_BUDGET > 0.0 && t > T_BUDGET) STATUS.store(1);
        else if (T_BLOCK > 0.0 && t > T_BLOCK) STATUS.store(2);
        if (STATUS.load(std::memory_order_relaxed)) return;
    }
    if (m < need) return;
    if (need == 1) {
        fprintf(stderr, "prefix depth exceeds the searchable tree depth\n");
        exit(8);
    }
    for (int i = 1; i <= need; i++) {
        const int thr = split_bound - GG[need - i + 1] + SLACK;
        if ((int)(std::upper_bound(cand, cand + m, thr) - cand) < i) return;
    }
    int limit = split_bound - GG[need] + SLACK;
    if (a2 && split_bound - a2 + SLACK < limit)
        limit = split_bound - a2 + SLACK;

    std::vector<uint64_t> nb2(W), nb3(W);
    std::vector<int> sub(m);
    for (int i = 0; i < m; i++) {
        if (m - i < need) return;
        const int y = cand[i];
        if (y > limit) return;
        bool dup = false;
        advance(A.S, A.ns, b2, b3, y, nb2.data(), nb3.data(), &dup);
        A.S[A.ns++] = y;
        pre.push_back(y);
        int sm = 0;
        if (!dup)
            for (int j = i + 1; j < m; j++)
                if (ok_add(A.S, A.ns, nb2.data(), nb3.data(), cand[j]))
                    sub[sm++] = cand[j];
        gen_shard_tasks(A, depth + 1, sub.data(), sm, need - 1, nb2.data(),
                        nb3.data(), a2 ? a2 : y, tdepth, split_bound, pre, out);
        pre.pop_back();
        A.ns--;
        if (STATUS.load(std::memory_order_relaxed)) return;
    }
}

static void gen_tasks(Arena &A, int depth, const int *cand, int m, int need,
                      const uint64_t *b2, const uint64_t *b3, int a2,
                      int tdepth, std::vector<int> &pre,
                      std::vector<Task> &out) {
    if (depth == tdepth) {
        Task t;
        t.pre = pre;
        t.cand.assign(cand, cand + m);
        t.b2.assign(b2, b2 + W);
        t.b3.assign(b3, b3 + W);
        t.need = need;
        t.a2 = a2;
        out.push_back(std::move(t));
        return;
    }
    if (m < need) return;
    const int Bv = BEST.B.load();
    if (need == 1) {                   // must never happen: tdepth <= k-3
        fprintf(stderr, "gen_tasks: need==1 above tdepth\n");
        exit(8);
    }
    for (int i = 1; i <= need; i++) {
        const int thr = Bv - GG[need - i + 1] + SLACK;
        if ((int)(std::upper_bound(cand, cand + m, thr) - cand) < i) return;
    }
    int limit = Bv - GG[need] + SLACK;
    if (a2 && Bv - a2 + SLACK < limit) limit = Bv - a2 + SLACK;

    std::vector<uint64_t> nb2(W), nb3(W);
    std::vector<int> sub(m);
    for (int i = 0; i < m; i++) {
        if (m - i < need) return;
        const int y = cand[i];
        if (y > limit) return;
        bool dup = false;
        advance(A.S, A.ns, b2, b3, y, nb2.data(), nb3.data(), &dup);
        A.S[A.ns++] = y;
        pre.push_back(y);
        int sm = 0;
        if (!dup)
            for (int j = i + 1; j < m; j++)
                if (ok_add(A.S, A.ns, nb2.data(), nb3.data(), cand[j]))
                    sub[sm++] = cand[j];
        gen_tasks(A, depth + 1, sub.data(), sm, need - 1, nb2.data(), nb3.data(),
                  a2 ? a2 : y, tdepth, pre, out);
        pre.pop_back();
        A.ns--;
    }
}

// ---------------------------------------------------------------------------
static std::vector<int> parse_ints(const char *s) {
    std::vector<int> v;
    const char *p = s;
    while (*p) {
        char *e;
        long x = strtol(p, &e, 10);
        if (e == p) break;
        v.push_back((int)x);
        p = e;
        while (*p == ',' || *p == ' ') p++;
    }
    return v;
}

static bool parse_shard(const std::string &spec, int *index, int *count) {
    const size_t slash = spec.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= spec.size() ||
        spec.find('/', slash + 1) != std::string::npos)
        return false;
    char *end = nullptr;
    const long i = strtol(spec.substr(0, slash).c_str(), &end, 10);
    if (!end || *end) return false;
    const long n = strtol(spec.substr(slash + 1).c_str(), &end, 10);
    if (!end || *end || i < 0 || n < 1 || i >= n || i > INT_MAX || n > INT_MAX)
        return false;
    *index = (int)i;
    *count = (int)n;
    return true;
}

static std::string csv_ints(const std::vector<int> &values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); i++) {
        if (i) out << ',';
        out << values[i];
    }
    return out.str();
}

static uint64_t fnv_text(const std::string &value) {
    uint64_t h = FNV_OFFSET;
    for (unsigned char ch : value) {
        h ^= ch;
        h *= FNV_PRIME;
    }
    return h;
}

static std::string hex64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

static bool write_jsonl(const std::string &path, const std::string &json) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return false;
    out << json << '\n';
    out.close();
    return (bool)out;
}

// --checkfile: run the incremental core (ok_add/advance, i.e. exactly what the
// DFS uses) over sets given one per line, and print 1/0 per line.  The harness
// compares this against the independent Python verifier.
static int checkfile(const char *path, int cap) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    W = (3 * cap) / 64 + 1;
    char *line = nullptr;
    size_t n = 0;
    std::vector<uint64_t> b2(W), b3(W), nb2(W), nb3(W);
    while (getline(&line, &n, f) != -1) {
        std::vector<int> A = parse_ints(line);
        std::fill(b2.begin(), b2.end(), 0);
        std::fill(b3.begin(), b3.end(), 0);
        int S[160];
        int ns = 0;
        bool dup = false, ok = true;
        for (int y : A) {
            if (y < 1 || y > cap) { ok = false; break; }
            bool present = false;
            for (int i = 0; i < ns; i++) if (S[i] == y) present = true;
            if (dup || present) { ok = false; break; }
            if (!ok_add(S, ns, b2.data(), b3.data(), y)) { ok = false; break; }
            bool d = false;
            advance(S, ns, b2.data(), b3.data(), y, nb2.data(), nb3.data(), &d);
            b2 = nb2;
            b3 = nb3;
            dup = dup || d;
            S[ns++] = y;
            std::sort(S, S + ns);
        }
        printf("%d\n", ok ? 1 : 0);
    }
    free(line);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    int k = 0, cap = 0, nthreads = 1;
    int shard_index = -1, shard_count = 0, prefix_depth = -1;
    std::vector<int> gvals, seedset;
    std::string mode = "canonical", variant = "multiset", shard_spec, output_path;
    const char *chkfile = nullptr;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value after %s\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--k") k = atoi(next());
        else if (a == "--cap") cap = atoi(next());
        else if (a == "--g") gvals = parse_ints(next());
        else if (a == "--best") seedset = parse_ints(next());
        else if (a == "--threads") nthreads = atoi(next());
        else if (a == "--mode") mode = next();
        else if (a == "--variant") variant = next();
        else if (a == "--budget-deadline") T_BUDGET = atof(next());
        else if (a == "--block-deadline") T_BLOCK = atof(next());
        else if (a == "--paranoid") PARANOID = true;
        else if (a == "--checkfile") chkfile = next();
        else if (a == "--shard") shard_spec = next();
        else if (a == "--prefix-depth") prefix_depth = atoi(next());
        else if (a == "--output") output_path = next();
        else { fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if (variant != "multiset" && variant != "distinct") {
        fprintf(stderr, "--variant must be multiset or distinct\n");
        return 2;
    }
    if (mode != "canonical" && mode != "compat") {
        fprintf(stderr, "--mode must be canonical or compat\n");
        return 2;
    }
    MULTI = (variant == "multiset");
    if (chkfile) return checkfile(chkfile, cap > 0 ? cap : 4096);
    if (k < 1 || cap < 1) { fprintf(stderr, "need --k and --cap\n"); return 2; }
    const bool shard_mode = !shard_spec.empty();
    if (shard_mode && !parse_shard(shard_spec, &shard_index, &shard_count)) {
        fprintf(stderr, "--shard must be zero-based i/N with 0 <= i < N\n");
        return 2;
    }
    if (!shard_mode && (prefix_depth >= 0 || !output_path.empty())) {
        fprintf(stderr, "--prefix-depth and --output require --shard\n");
        return 2;
    }
    CANON = (mode == "canonical");
    SLACK = CANON ? 1 : 0;
    if (!CANON) nthreads = 1;
    if (nthreads < 1) nthreads = 1;
    if (shard_mode) {
        if (!CANON) {
            fprintf(stderr, "shard mode requires --mode canonical\n");
            return 2;
        }
        if (nthreads != 1) {
            fprintf(stderr, "shard mode requires --threads 1\n");
            return 2;
        }
        if (k < 2) {
            fprintf(stderr, "shard mode requires k >= 2\n");
            return 2;
        }
        if (prefix_depth < 0) prefix_depth = std::min(2, k - 2);
        if (prefix_depth < 0 || prefix_depth > k - 2) {
            fprintf(stderr, "--prefix-depth must be between 0 and k-2\n");
            return 2;
        }
    }
    KK = k;
    CAP = cap;
    // Every element ever placed satisfies y <= B: at need >= 2 the loop bound is
    // y <= B - g[need] + SLACK <= B - 2 + 1, and at need == 1 acceptance is
    // y < B + SLACK <= B + 1 with SLACK <= 1 -- and B never grows.  So when an
    // incumbent seeds B, the masks only have to be wide enough for 3*max(seed).
    int wcap = cap;
    if (!seedset.empty()) {
        int sm = *std::max_element(seedset.begin(), seedset.end());
        if (sm < wcap) wcap = sm;
    }
    W = (3 * wcap) / 64 + 1;
    GG.assign(k + 2, 0);
    for (int j = 1; j <= k - 1; j++) {
        if ((int)gvals.size() < j) { fprintf(stderr, "missing g(%d)\n", j); return 2; }
        GG[j] = gvals[j - 1];
    }

    const double t0 = now_s();
    BEST.B.store(cap + 1);
    if (!seedset.empty()) {
        std::sort(seedset.begin(), seedset.end());
        BEST.set = seedset;
        BEST.have = true;
        BEST.B.store(seedset.back());
    }

    const int levels = k + 3;
    Arena root(levels, wcap + 2);

    // the set {1}
    std::vector<uint64_t> zero(W, 0);
    uint64_t *rb2 = root.B2(0), *rb3 = root.B3(0);
    bool dup0 = false;
    root.ns = 0;
    advance(root.S, 0, zero.data(), zero.data(), 1, rb2, rb3, &dup0);
    root.S[root.ns++] = 1;
    int *cand0 = root.CANDB(0);
    int m0 = 0;
    for (int y = 2; y <= wcap; y++)
        if (ok_add(root.S, root.ns, rb2, rb3, y)) cand0[m0++] = y;

    long long total_nodes = 0;
    int used_threads = 1;
    long long ntasks = 0;

    if (shard_mode) {
        Arena ga(levels, wcap + 2);
        ga.ns = root.ns;
        memcpy(ga.S, root.S, sizeof(int) * (size_t)root.ns);
        ShardCollector shard(shard_index, shard_count, levels, wcap + 2);
        std::vector<int> pre;
        const int split_bound = BEST.B.load(std::memory_order_relaxed);
        gen_shard_tasks(ga, 0, cand0, m0, k - 1, rb2, rb3, 0, prefix_depth,
                        split_bound, pre, shard);

        const long long attributed_split = shard_index == 0 ? shard.split_nodes : 0;
        total_nodes = shard.worker.nodes + attributed_split;
        ntasks = (long long)shard.prefix_ids.size();
        const double dt = now_s() - t0;
        const int st = STATUS.load();
        const char *status = "none";
        if (st == 1) status = "timeout_budget";
        else if (st == 2) status = "timeout_block";
        else if (BEST.have) status = "found";

        std::vector<int> used_g;
        for (int j = 1; j < k; j++) used_g.push_back(GG[j]);
        std::ostringstream config;
        config << "b3core-shard-v1;k=" << k << ";cap=" << cap
               << ";mode=" << mode << ";variant=" << variant
               << ";g=" << csv_ints(used_g) << ";seed=" << csv_ints(seedset)
               << ";prefix_depth=" << prefix_depth
               << ";shard_count=" << shard_count;

        std::ostringstream json;
        json << "{\"schema\":\"b3core-shard-v1\",\"config_hash\":\"fnv1a64:"
             << hex64(fnv_text(config.str())) << "\",\"k\":" << k
             << ",\"cap\":" << cap << ",\"mode\":\"" << mode
             << "\",\"variant\":\"" << variant << "\",\"g\":[";
        for (size_t i = 0; i < used_g.size(); i++)
            json << (i ? "," : "") << used_g[i];
        json << "],\"seed\":[";
        for (size_t i = 0; i < seedset.size(); i++)
            json << (i ? "," : "") << seedset[i];
        json << "],\"range\":{\"max_element_min\":" << GG[k - 1] + 1
             << ",\"max_element_max\":" << cap << ",\"diameter_min\":"
             << GG[k - 1] << ",\"diameter_max\":" << cap - 1
             << "},\"shard_index\":" << shard_index
             << ",\"shard_count\":" << shard_count
             << ",\"assignment\":\"prefix_id_mod_shard_count\""
             << ",\"prefix_depth\":" << prefix_depth
             << ",\"prefix_count\":" << shard.prefix_count
             << ",\"prefix_catalog_hash\":\"fnv1a64:"
             << hex64(shard.catalog_hash) << "\",\"processed_prefix_hash\":\"fnv1a64:"
             << hex64(shard.processed_hash) << "\",\"prefix_ids\":[";
        for (size_t i = 0; i < shard.prefix_ids.size(); i++)
            json << (i ? "," : "") << shard.prefix_ids[i];
        json << "],\"prefixes\":[";
        for (size_t i = 0; i < shard.prefixes.size(); i++) {
            if (i) json << ',';
            json << '[';
            for (size_t j = 0; j < shard.prefixes[i].size(); j++)
                json << (j ? "," : "") << shard.prefixes[i][j];
            json << ']';
        }
        json << "],\"threads\":1,\"tasks\":" << ntasks
             << ",\"nodes\":" << total_nodes
             << ",\"dfs_nodes\":" << shard.worker.nodes
             << ",\"split_nodes\":" << shard.split_nodes
             << ",\"attributed_split_nodes\":" << attributed_split
             << ",\"node_accounting\":\"split_nodes_attributed_to_shard_zero\""
             << ",\"status\":\"" << status << "\",\"complete\":"
             << (st == 0 ? "true" : "false") << ",\"seconds\":"
             << std::fixed << std::setprecision(3) << dt << ",\"set\":[";
        if (BEST.have && st == 0)
            for (size_t i = 0; i < BEST.set.size(); i++)
                json << (i ? "," : "") << BEST.set[i];
        json << "]}";
        const std::string line = json.str();
        if (!output_path.empty() && !write_jsonl(output_path, line)) {
            fprintf(stderr, "cannot write %s\n", output_path.c_str());
            return 2;
        }
        printf("%s\n", line.c_str());
        return st == 0 ? 0 : 3;
    } else if (nthreads > 1 && k - 1 >= 4) {
        int tdepth = 1;
        std::vector<Task> tasks;
        while (true) {
            tasks.clear();
            std::vector<int> pre;
            Arena ga(levels, wcap + 2);
            ga.ns = root.ns;
            memcpy(ga.S, root.S, sizeof(int) * root.ns);
            gen_tasks(ga, 0, cand0, m0, k - 1, rb2, rb3, 0, tdepth, pre, tasks);
            if ((int)tasks.size() >= 16 * nthreads || tdepth >= k - 3) break;
            tdepth++;
        }
        ntasks = (long long)tasks.size();
        used_threads = nthreads;
        std::atomic<long long> next{0};
        std::vector<std::thread> th;
        std::atomic<long long> nodes_acc{0};
        for (int t = 0; t < nthreads; t++) {
            th.emplace_back([&]() {
                Arena A(levels, wcap + 2);
                for (;;) {
                    long long idx = next.fetch_add(1);
                    if (idx >= (long long)tasks.size()) break;
                    if (STATUS.load()) break;
                    Task &tk = tasks[idx];
                    A.ns = 0;
                    A.S[A.ns++] = 1;
                    for (int v : tk.pre) A.S[A.ns++] = v;
                    memcpy(A.B2(0), tk.b2.data(), sizeof(uint64_t) * W);
                    memcpy(A.B3(0), tk.b3.data(), sizeof(uint64_t) * W);
                    int *c = A.CANDB(0);
                    if (!tk.cand.empty())
                        memcpy(c, tk.cand.data(), sizeof(int) * tk.cand.size());
                    dfs(A, 0, c, (int)tk.cand.size(), tk.need, A.B2(0), A.B3(0),
                        tk.a2);
                }
                nodes_acc.fetch_add(A.nodes);
            });
        }
        for (auto &x : th) x.join();
        total_nodes = nodes_acc.load();
    } else {
        dfs(root, 0, cand0, m0, k - 1, rb2, rb3, 0);
        total_nodes = root.nodes;
    }

    const double dt = now_s() - t0;
    const char *status = "none";
    const int st = STATUS.load();
    if (st == 1) status = "timeout_budget";
    else if (st == 2) status = "timeout_block";
    else if (BEST.have) status = "found";

    printf("{\"status\":\"%s\",\"k\":%d,\"cap\":%d,\"mode\":\"%s\",\"variant\":\"%s\","
           "\"threads\":%d,\"tasks\":%lld,\"nodes\":%lld,\"seconds\":%.3f,\"set\":[",
           status, k, cap, mode.c_str(), variant.c_str(), used_threads, ntasks,
           total_nodes, dt);
    if (BEST.have && st == 0)
        for (size_t i = 0; i < BEST.set.size(); i++)
            printf("%s%d", i ? "," : "", BEST.set[i]);
    printf("]}\n");
    return 0;
}
