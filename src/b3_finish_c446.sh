#!/bin/bash
# b3_finish_c446.sh: close the k=11 cap 446 campaign (A227358(11)).
# 1. audit against the expected campaign (every shard once, binary, config, independent prefix catalog),
# 2. merge with --expect-g 446: the merger fails unless the global optimum has largest element 446,
# 3. check the witness from the definition with code that shares nothing with b3core,
# 4. persist merged result, manifests and shard archive to project space with SHA256SUMS.
# Exit 0 only if all four pass. FOUND is the expected result here (b3_audit.py exits 2 on FOUND).
set -u
B=/project/project_465003389/green27/b3
C=/scratch/project_465003389/hq/wp2_k11_c446
OUT=$B/wynik_k11_cap446
G11=1,2,5,12,24,46,83,130,209,310
module load cray-python/3.11.7 >/dev/null 2>&1
SHA=$(grep b3core-portable "$B/MANIFEST.txt" | cut -c1-64)

python3 "$B/b3_audit.py" --k 11 --cap 446 --g $G11 --depth 3 --N 4096 --bin-sha "$SHA" \
    --catalog-count 3654793 --catalog-hash fnv1a64:83701211d99c38d9 "$C/results"
AUDIT_RC=$?
[ "$AUDIT_RC" -eq 2 ] || { echo "AUDIT: expected FOUND (rc 2), got rc $AUDIT_RC"; exit 1; }

[ -e "$OUT" ] && { echo "refusing: $OUT exists"; exit 3; }
mkdir -p "$OUT"
python3 "$B/merge_b3_shards.py" "$C"/results/shard-*.jsonl --expect-g 446 --output "$OUT/merged_k11_cap446.json" \
    || { echo "MERGE with --expect-g 446 failed"; exit 1; }

python3 - "$OUT/merged_k11_cap446.json" <<'EOF' | tee "$OUT/witness_check.txt"
import itertools, json, sys
m = json.load(open(sys.argv[1]))
s = m["set"]
t = [x - min(s) for x in s]                      # shift so the set starts at 0, as in the OEIS definition
sums = [a + b + c for a, b, c in itertools.combinations_with_replacement(t, 3)]
ok = (m["status"] == "found" and len(t) == 11 and len(set(t)) == 11 and max(t) == 445
      and len(sums) == 286 and len(set(sums)) == 286)
print("witness (from 0):", t)
print("elements", len(t), "span", max(t) - min(t), "triple sums", len(sums), "distinct", len(set(sums)))
print("coverage", m["coverage"], "| prefixes", m["prefix_count"], "| nodes", m["nodes"])
print("WITNESS CHECK", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
EOF
W_RC=${PIPESTATUS[0]}
[ "$W_RC" -eq 0 ] || { echo "witness check failed"; exit 1; }

cp "$B/MANIFEST.txt" "$OUT/"
cp /project/project_465003389/green27/hq/tasks_b3_k11_c446.txt "$OUT/"
tar -C /scratch/project_465003389/hq -cf - wp2_k11_c446/results wp2_k11_c446/LAUNCH.txt \
    | zstd -q -10 -o "$OUT/shards_k11_cap446.tar.zst"
(cd "$OUT" && sha256sum merged_k11_cap446.json witness_check.txt MANIFEST.txt tasks_b3_k11_c446.txt \
    shards_k11_cap446.tar.zst > SHA256SUMS && cat SHA256SUMS)
echo "FINISH PASS: a(11) = 445 established by this run"
