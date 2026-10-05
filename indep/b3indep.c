/*
 * b3indep.c -- independent exhaustive enumerator for finite B_3 sets.
 *
 * A set S is B_3 when all a+b+c with a <= b <= c and a,b,c in S
 * are distinct.  The search fixes the first element to zero and performs no
 * reflection/orientation symmetry reduction.
 *
 * Catalog hash format:
 *   Start with the 64-bit FNV-1a offset basis.  For every catalog prefix in
 *   lexicographic order, feed its zero-based catalog index followed by all
 *   D+1 prefix elements (including the initial zero).  Every value is an
 *   unsigned 64-bit integer encoded as exactly eight little-endian bytes.
 *   Feed each byte using FNV-1a (xor, then multiply by the FNV prime).
 * The JSON representation is a lower-case, zero-padded hexadecimal string.
 *
 * "nodes" counts valid tree nodes in the subtrees assigned to this shard,
 * including each assigned prefix root.  Work used by every process merely to
 * regenerate the common prefix catalog is deliberately not counted.  Thus
 * node counts add exactly across a complete set of shards and are independent
 * of N (for a fixed depth).
 *
 * Build:
 *   cc -O3 -std=c11 -Wall -Wextra -pedantic b3indep.c -o b3indep
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FNV1A_OFFSET UINT64_C(14695981039346656037)
#define FNV1A_PRIME  UINT64_C(1099511628211)

typedef struct {
    int k;
    int maxel;
    int depth;
    uint64_t shard;
    uint64_t nshards;

    bool bounds_enabled;
    int *bounds;
    size_t bounds_count;

    size_t max_print;
    int *elements;
    uint64_t *sum_bits;
    size_t sum_words;

    /* One scratch row per old-set size; child recursion uses another row. */
    int *scratch;
    size_t scratch_stride;

    uint64_t catalog_count;
    uint64_t catalog_hash;
    uint64_t prefixes_done;
    uint64_t nodes;
    uint64_t solutions;

    bool have_min;
    int min_span;
    uint64_t solutions_at_min;
    int *first_solutions;
    size_t first_count;

    bool counter_overflow;
} Search;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "usage: %s --k K --maxel M [--bounds a1,a2,...] "
            "[--depth D --shard i/N] [--max-print P]\n",
            argv0);
}

static bool parse_i64(const char *text, int64_t lo, int64_t hi, int64_t *out)
{
    char *end = NULL;
    intmax_t value;

    if (text == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < lo || value > hi) {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

static bool parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    uintmax_t value;

    if (text == NULL || *text == '\0' || *text == '-') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT64_MAX) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool parse_shard(const char *text, uint64_t *index, uint64_t *count)
{
    const char *slash;
    char *left;
    size_t left_len;
    bool ok;

    if (text == NULL) {
        return false;
    }
    slash = strchr(text, '/');
    if (slash == NULL || slash == text || slash[1] == '\0' || strchr(slash + 1, '/') != NULL) {
        return false;
    }
    left_len = (size_t)(slash - text);
    left = (char *)malloc(left_len + 1);
    if (left == NULL) {
        return false;
    }
    memcpy(left, text, left_len);
    left[left_len] = '\0';
    ok = parse_u64(left, index) && parse_u64(slash + 1, count) &&
         *count > 0 && *index < *count;
    free(left);
    return ok;
}

static bool parse_bounds(const char *text, int **values_out, size_t *count_out)
{
    const char *p;
    size_t count = 1;
    size_t i = 0;
    int *values;

    if (text == NULL || *text == '\0') {
        return false;
    }
    for (p = text; *p != '\0'; ++p) {
        if (*p == ',') {
            ++count;
        }
    }
    if (count > SIZE_MAX / sizeof(*values)) {
        return false;
    }
    values = (int *)malloc(count * sizeof(*values));
    if (values == NULL) {
        return false;
    }

    p = text;
    while (*p != '\0') {
        char *end = NULL;
        long value;

        if (*p == ',') {
            free(values);
            return false;
        }
        errno = 0;
        value = strtol(p, &end, 10);
        if (errno != 0 || end == p || value < 0 || value > INT_MAX ||
            (*end != ',' && *end != '\0')) {
            free(values);
            return false;
        }
        values[i++] = (int)value;
        if (*end == '\0') {
            p = end;
        } else {
            p = end + 1;
            if (*p == '\0') {
                free(values);
                return false;
            }
        }
    }
    if (i != count) {
        free(values);
        return false;
    }
    *values_out = values;
    *count_out = count;
    return true;
}

static bool checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool increment_counter(uint64_t *value, Search *search)
{
    if (*value == UINT64_MAX) {
        search->counter_overflow = true;
        return false;
    }
    ++*value;
    return true;
}

static inline bool sum_is_set(const Search *search, int sum)
{
    size_t word = (size_t)sum >> 6;
    uint64_t mask = UINT64_C(1) << ((unsigned)sum & 63U);
    return (search->sum_bits[word] & mask) != 0;
}

static inline void set_sum(Search *search, int sum)
{
    size_t word = (size_t)sum >> 6;
    search->sum_bits[word] |= UINT64_C(1) << ((unsigned)sum & 63U);
}

static inline void clear_sum(Search *search, int sum)
{
    size_t word = (size_t)sum >> 6;
    search->sum_bits[word] &= ~(UINT64_C(1) << ((unsigned)sum & 63U));
}

static bool add_one_new_sum(Search *search, int sum, int *scratch, size_t *count)
{
    if (sum_is_set(search, sum)) {
        return false;
    }
    set_sum(search, sum);
    scratch[(*count)++] = sum;
    return true;
}

/*
 * Add y to an already valid set elements[0..len-1].  Exactly the new triples
 * are y+a+b (a<=b old), 2y+a (a old), and 3y.  Testing against the live bitset
 * also tests pairwise distinctness among these new sums.
 */
static bool try_add(Search *search, int len, int y, int *scratch, size_t *added)
{
    int a;
    int b;
    size_t count = 0;

    for (a = 0; a < len; ++a) {
        for (b = a; b < len; ++b) {
            if (!add_one_new_sum(search, y + search->elements[a] + search->elements[b],
                                 scratch, &count)) {
                goto conflict;
            }
        }
    }
    for (a = 0; a < len; ++a) {
        if (!add_one_new_sum(search, 2 * y + search->elements[a], scratch, &count)) {
            goto conflict;
        }
    }
    if (!add_one_new_sum(search, 3 * y, scratch, &count)) {
        goto conflict;
    }

    *added = count;
    return true;

conflict:
    while (count > 0) {
        clear_sum(search, scratch[--count]);
    }
    *added = 0;
    return false;
}

static void undo_add(Search *search, const int *scratch, size_t added)
{
    while (added > 0) {
        clear_sum(search, scratch[--added]);
    }
}

/* Old set has len elements.  Return the largest possible next element. */
static int candidate_upper(const Search *search, int len)
{
    int remaining_after_pick = search->k - len - 1;
    int upper = search->maxel - remaining_after_pick;

    if (search->bounds_enabled) {
        int bound = search->bounds[remaining_after_pick]; /* a(r+1) */
#ifdef B3_STRICT_BOUND_BUG
        /* Test-only fault injection: deliberately changes >= into >. */
        int bounded_upper = search->maxel - bound - 1;
#else
        int bounded_upper = search->maxel - bound;
#endif
        if (bounded_upper < upper) {
            upper = bounded_upper;
        }
    }
    return upper;
}

static int lex_compare(const int *a, const int *b, int len)
{
    int i;
    for (i = 0; i < len; ++i) {
        if (a[i] < b[i]) {
            return -1;
        }
        if (a[i] > b[i]) {
            return 1;
        }
    }
    return 0;
}

static void remember_solution(Search *search)
{
    size_t pos;

    if (search->max_print == 0) {
        return;
    }
    for (pos = 0; pos < search->first_count; ++pos) {
        int cmp = lex_compare(search->elements,
                              search->first_solutions + pos * (size_t)search->k,
                              search->k);
        if (cmp == 0) {
            return;
        }
        if (cmp < 0) {
            break;
        }
    }

    if (search->first_count < search->max_print) {
        size_t moving = search->first_count - pos;
        if (moving > 0) {
            memmove(search->first_solutions + (pos + 1) * (size_t)search->k,
                    search->first_solutions + pos * (size_t)search->k,
                    moving * (size_t)search->k * sizeof(int));
        }
        memcpy(search->first_solutions + pos * (size_t)search->k,
               search->elements, (size_t)search->k * sizeof(int));
        ++search->first_count;
    } else if (pos < search->max_print) {
        size_t moving = search->max_print - pos - 1;
        if (moving > 0) {
            memmove(search->first_solutions + (pos + 1) * (size_t)search->k,
                    search->first_solutions + pos * (size_t)search->k,
                    moving * (size_t)search->k * sizeof(int));
        }
        memcpy(search->first_solutions + pos * (size_t)search->k,
               search->elements, (size_t)search->k * sizeof(int));
    }
}

static void record_solution(Search *search)
{
    int span = search->elements[search->k - 1];

    if (!increment_counter(&search->solutions, search)) {
        return;
    }
    if (!search->have_min || span < search->min_span) {
        search->have_min = true;
        search->min_span = span;
        search->solutions_at_min = 1;
        search->first_count = 0;
        remember_solution(search);
    } else if (span == search->min_span) {
        if (!increment_counter(&search->solutions_at_min, search)) {
            return;
        }
        remember_solution(search);
    }
}

/* Return true only when this assigned subtree was completely enumerated. */
static bool search_subtree(Search *search, int len)
{
    int y;
    int upper;
    int *scratch;

    if (stop_requested || search->counter_overflow) {
        return false;
    }
    if (!increment_counter(&search->nodes, search)) {
        return false;
    }
    if (len == search->k) {
        record_solution(search);
        return !stop_requested && !search->counter_overflow;
    }

    upper = candidate_upper(search, len);
    scratch = search->scratch + (size_t)len * search->scratch_stride;
    for (y = search->elements[len - 1] + 1; y <= upper; ++y) {
        size_t added = 0;
        bool complete;

        if (stop_requested || search->counter_overflow) {
            return false;
        }
        if (!try_add(search, len, y, scratch, &added)) {
            continue;
        }
        search->elements[len] = y;
        complete = search_subtree(search, len + 1);
        undo_add(search, scratch, added);
        if (!complete) {
            return false;
        }
    }
    return !stop_requested && !search->counter_overflow;
}

static void fnv_feed_u64_le(uint64_t *hash, uint64_t value)
{
    unsigned shift;
    for (shift = 0; shift < 64; shift += 8) {
        *hash ^= (value >> shift) & UINT64_C(0xff);
        *hash *= FNV1A_PRIME;
    }
}

/* Generate every valid catalog prefix, even after an interrupted subtree. */
static void generate_catalog(Search *search, int len, int target_len)
{
    int y;
    int upper;
    int *scratch;

    if (len == target_len) {
        uint64_t index = search->catalog_count;
        int j;

        fnv_feed_u64_le(&search->catalog_hash, index);
        for (j = 0; j < len; ++j) {
            fnv_feed_u64_le(&search->catalog_hash,
                            (uint64_t)(unsigned)search->elements[j]);
        }
        if (!increment_counter(&search->catalog_count, search)) {
            return;
        }

        if (index % search->nshards == search->shard &&
            !stop_requested && !search->counter_overflow) {
            if (search_subtree(search, len)) {
                (void)increment_counter(&search->prefixes_done, search);
            }
        }
        return;
    }

    upper = candidate_upper(search, len);
    scratch = search->scratch + (size_t)len * search->scratch_stride;
    for (y = search->elements[len - 1] + 1; y <= upper; ++y) {
        size_t added = 0;

        if (!try_add(search, len, y, scratch, &added)) {
            continue;
        }
        search->elements[len] = y;
        generate_catalog(search, len + 1, target_len);
        undo_add(search, scratch, added);
    }
}

static double now_seconds(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return (double)clock() / (double)CLOCKS_PER_SEC;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void print_json(const Search *search, double seconds)
{
    size_t i;
    int j;

    printf("{\"k\":%d,\"maxel\":%d,\"bounds\":", search->k, search->maxel);
    if (!search->bounds_enabled) {
        printf("null");
    } else {
        putchar('[');
        for (i = 0; i < search->bounds_count; ++i) {
            if (i != 0) {
                putchar(',');
            }
            printf("%d", search->bounds[i]);
        }
        putchar(']');
    }
    printf(",\"depth\":%d,\"shard\":%" PRIu64 ",\"N\":%" PRIu64,
           search->depth, search->shard, search->nshards);
    printf(",\"max_print\":%zu,\"symmetry_pruning\":false", search->max_print);
    printf(",\"catalog_count\":%" PRIu64, search->catalog_count);
    printf(",\"catalog_hash\":\"0x%016" PRIx64 "\"", search->catalog_hash);
    printf(",\"prefixes_done\":%" PRIu64, search->prefixes_done);
    printf(",\"nodes\":%" PRIu64 ",\"solutions\":%" PRIu64,
           search->nodes, search->solutions);
    if (search->have_min) {
        printf(",\"min_span\":%d,\"solutions_at_min\":%" PRIu64,
               search->min_span, search->solutions_at_min);
    } else {
        printf(",\"min_span\":null,\"solutions_at_min\":0");
    }
    printf(",\"first_solutions\":[");
    for (i = 0; i < search->first_count; ++i) {
        if (i != 0) {
            putchar(',');
        }
        putchar('[');
        for (j = 0; j < search->k; ++j) {
            if (j != 0) {
                putchar(',');
            }
            printf("%d", search->first_solutions[i * (size_t)search->k + (size_t)j]);
        }
        putchar(']');
    }
    printf("],\"seconds\":%.6f,\"complete\":%s}\n",
           seconds, (!stop_requested && !search->counter_overflow) ? "true" : "false");
}

int main(int argc, char **argv)
{
    Search search;
    bool have_k = false;
    bool have_maxel = false;
    bool have_depth = false;
    bool have_shard = false;
    const char *bounds_text = NULL;
    int64_t parsed;
    uint64_t parsed_u64;
    size_t i;
    size_t max_new;
    size_t scratch_cells;
    size_t solution_cells;
    size_t bit_count;
    double started;
    double elapsed;

    memset(&search, 0, sizeof(search));
    search.shard = 0;
    search.nshards = 1;
    search.depth = 0;
    search.max_print = 10;
    search.catalog_hash = FNV1A_OFFSET;

    for (i = 1; i < (size_t)argc; ++i) {
        const char *arg = argv[i];
        const char *value = NULL;

        if (strcmp(arg, "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (i + 1 >= (size_t)argc) {
            fprintf(stderr, "missing value after %s\n", arg);
            usage(stderr, argv[0]);
            return 2;
        }
        value = argv[++i];
        if (strcmp(arg, "--k") == 0) {
            if (!parse_i64(value, 1, INT_MAX, &parsed)) {
                fprintf(stderr, "invalid --k: %s\n", value);
                return 2;
            }
            search.k = (int)parsed;
            have_k = true;
        } else if (strcmp(arg, "--maxel") == 0) {
            if (!parse_i64(value, -1, INT_MAX / 3, &parsed)) {
                fprintf(stderr, "invalid --maxel: %s\n", value);
                return 2;
            }
            search.maxel = (int)parsed;
            have_maxel = true;
        } else if (strcmp(arg, "--bounds") == 0) {
            if (bounds_text != NULL) {
                fprintf(stderr, "--bounds specified more than once\n");
                return 2;
            }
            bounds_text = value;
        } else if (strcmp(arg, "--depth") == 0) {
            if (!parse_i64(value, 0, INT_MAX, &parsed)) {
                fprintf(stderr, "invalid --depth: %s\n", value);
                return 2;
            }
            search.depth = (int)parsed;
            have_depth = true;
        } else if (strcmp(arg, "--shard") == 0) {
            if (!parse_shard(value, &search.shard, &search.nshards)) {
                fprintf(stderr, "invalid --shard (expected i/N with 0 <= i < N): %s\n", value);
                return 2;
            }
            have_shard = true;
        } else if (strcmp(arg, "--max-print") == 0) {
            if (!parse_u64(value, &parsed_u64) || parsed_u64 > SIZE_MAX) {
                fprintf(stderr, "invalid --max-print: %s\n", value);
                return 2;
            }
            search.max_print = (size_t)parsed_u64;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (!have_k || !have_maxel) {
        fprintf(stderr, "--k and --maxel are required\n");
        usage(stderr, argv[0]);
        return 2;
    }
    if (have_depth != have_shard) {
        fprintf(stderr, "--depth and --shard must be specified together\n");
        return 2;
    }
    if (search.depth < 0 || search.depth > search.k - 1) {
        fprintf(stderr, "--depth must satisfy 0 <= D <= K-1\n");
        return 2;
    }

    if (bounds_text != NULL) {
        if (!parse_bounds(bounds_text, &search.bounds, &search.bounds_count)) {
            fprintf(stderr, "invalid --bounds list\n");
            return 2;
        }
        search.bounds_enabled = true;
        if (search.bounds_count < (size_t)(search.k - 1)) {
            fprintf(stderr, "--bounds needs at least K-1 values (a1 through a(K-1))\n");
            free(search.bounds);
            return 2;
        }
        if (search.bounds[0] != 0) {
            fprintf(stderr, "--bounds must start with a(1)=0\n");
            free(search.bounds);
            return 2;
        }
        for (i = 1; i < search.bounds_count; ++i) {
            if (search.bounds[i] < search.bounds[i - 1]) {
                fprintf(stderr, "--bounds must be nondecreasing\n");
                free(search.bounds);
                return 2;
            }
        }
    }

    if (!checked_mul_size((size_t)search.k, sizeof(*search.elements), &solution_cells)) {
        fprintf(stderr, "K is too large\n");
        free(search.bounds);
        return 2;
    }
    search.elements = (int *)malloc(solution_cells);
    if (search.elements == NULL) {
        fprintf(stderr, "out of memory for elements\n");
        free(search.bounds);
        return 2;
    }
    search.elements[0] = 0;

    /* Max new sums at a level with K-1 old elements: K*(K+1)/2. */
    if ((size_t)search.k > SIZE_MAX / ((size_t)search.k + 1U)) {
        fprintf(stderr, "K is too large\n");
        free(search.elements);
        free(search.bounds);
        return 2;
    }
    max_new = (size_t)search.k * ((size_t)search.k + 1U) / 2U;
    search.scratch_stride = max_new;
    if (!checked_mul_size((size_t)search.k, max_new, &scratch_cells) ||
        !checked_mul_size(scratch_cells, sizeof(*search.scratch), &scratch_cells)) {
        fprintf(stderr, "K is too large for scratch storage\n");
        free(search.elements);
        free(search.bounds);
        return 2;
    }
    search.scratch = (int *)malloc(scratch_cells == 0 ? 1 : scratch_cells);
    if (search.scratch == NULL) {
        fprintf(stderr, "out of memory for scratch storage\n");
        free(search.elements);
        free(search.bounds);
        return 2;
    }

    bit_count = search.maxel < 0 ? 1U : (size_t)(3 * search.maxel) + 1U;
    search.sum_words = (bit_count + 63U) / 64U;
    search.sum_bits = (uint64_t *)calloc(search.sum_words, sizeof(*search.sum_bits));
    if (search.sum_bits == NULL) {
        fprintf(stderr, "out of memory for sum bitset\n");
        free(search.scratch);
        free(search.elements);
        free(search.bounds);
        return 2;
    }

    if (!checked_mul_size(search.max_print, (size_t)search.k, &solution_cells) ||
        !checked_mul_size(solution_cells, sizeof(*search.first_solutions), &solution_cells)) {
        fprintf(stderr, "--max-print is too large\n");
        free(search.sum_bits);
        free(search.scratch);
        free(search.elements);
        free(search.bounds);
        return 2;
    }
    if (solution_cells != 0) {
        search.first_solutions = (int *)malloc(solution_cells);
        if (search.first_solutions == NULL) {
            fprintf(stderr, "out of memory for printed solutions\n");
            free(search.sum_bits);
            free(search.scratch);
            free(search.elements);
            free(search.bounds);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    started = now_seconds();

    if (search.maxel >= 0) {
        set_sum(&search, 0); /* the sole triple of the initial set {0} */
        generate_catalog(&search, 1, search.depth + 1);
    }

    elapsed = now_seconds() - started;
    if (elapsed < 0.0) {
        elapsed = 0.0;
    }
    print_json(&search, elapsed);

    free(search.first_solutions);
    free(search.sum_bits);
    free(search.scratch);
    free(search.elements);
    free(search.bounds);
    return (stop_requested || search.counter_overflow) ? 1 : 0;
}
