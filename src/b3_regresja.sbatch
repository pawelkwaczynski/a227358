#!/bin/bash -l
#SBATCH --account=project_465003389
#SBATCH --partition=small
#SBATCH --job-name=b3_regresja
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=18
#SBATCH --hint=nomultithread
#SBATCH --mem=18G
#SBATCH --time=01:00:00
#SBATCH --output=/scratch/project_465003389/logs/b3_regresja_%j.out
# WP0-a for workload C (EuroHPC proposal, section C.3): b3core correctness on LUMI-C against the reference
# measurements of 28-29.08.2026 (GCP e2, same source, b3core.cpp sha256 ce4cede...). Every check must pass:
#   R1 k=9 cap=208 unsharded: status none, nodes == 1437666229
#   R2 k=9 cap=209 unsharded: status found, set == [1,2,18,27,128,139,186,205,209], nodes == 1466633770
#   R3 k=9 cap=208 as 16 shards: merger accepts (every prefix exactly once) and summed nodes == R1 nodes
#   R4 k=8 cap=129 none, cap=130 found (a(8) = 129, convention cap = a + 1)
# Also records the per-core node rate on LUMI-C, which sizes the WP2 production run.
set -u
B=/project/project_465003389/green27/b3
OUT=/scratch/project_465003389/b3/regresja_$SLURM_JOB_ID
mkdir -p "$OUT"
module load gcc-native/14.2 cray-python/3.11.7
BIN=$B/b3core-portable
COMMON="--mode canonical --variant multiset --threads 1"
G8=1,2,5,12,24,46,83
G9=1,2,5,12,24,46,83,130
echo "host=$(hostname) start=$(date -u +%FT%TZ)"
sha256sum $B/b3core.cpp $BIN $B/merge_b3_shards.py

$BIN --k 8 --cap 129 --g $G8 $COMMON > "$OUT/k8_cap129.json"
$BIN --k 8 --cap 130 --g $G8 $COMMON > "$OUT/k8_cap130.json"

PIDS=()
$BIN --k 9 --cap 208 --g $G9 $COMMON > "$OUT/k9_cap208.json" & PIDS+=($!)
$BIN --k 9 --cap 209 --g $G9 $COMMON > "$OUT/k9_cap209.json" & PIDS+=($!)
for i in $(seq 0 15); do
  $BIN --k 9 --cap 208 --g $G9 $COMMON --prefix-depth 2 --shard "$i/16" \
       --output "$OUT/shard-$i.jsonl" > /dev/null & PIDS+=($!)
done
PROC_FAIL=0
for pid in "${PIDS[@]}"; do wait "$pid" || PROC_FAIL=1; done
echo "process_failures=$PROC_FAIL"

python3 "$B/merge_b3_shards.py" "$OUT"/shard-*.jsonl --baseline "$OUT/k9_cap208.json" \
        --output "$OUT/merged_k9_cap208.json"
MERGE_RC=$?
echo "merge_rc=$MERGE_RC"

python3 - "$OUT" <<'EOF'
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
load = lambda name: json.loads((out / name).read_text().strip().splitlines()[-1])
k8a, k8b = load("k8_cap129.json"), load("k8_cap130.json")
r1, r2 = load("k9_cap208.json"), load("k9_cap209.json")
merged = load("merged_k9_cap208.json") if (out / "merged_k9_cap208.json").exists() else {}
checks = {
    "R1": r1["status"] == "none" and r1["nodes"] == 1437666229,
    "R2": r2["status"] == "found" and r2["set"] == [1, 2, 18, 27, 128, 139, 186, 205, 209]
          and r2["nodes"] == 1466633770,
    "R3": merged.get("coverage") == "every_prefix_exactly_once" and merged.get("status") == "none"
          and merged.get("nodes") == r1["nodes"],
    "R4": k8a["status"] == "none" and k8b["status"] == "found",
}
for name, ok in checks.items():
    print(name, "PASS" if ok else "FAIL")
print("k8 cap129 nodes", k8a["nodes"], "| k8 cap130 set", k8b["set"])
rate = r1["nodes"] / r1["seconds"] if r1["seconds"] else 0
print(f"rate_k9_cap208 {rate:,.0f} nodes/s/core ({r1['seconds']:.1f} s)")
print("REGRESSION", "PASS" if all(checks.values()) else "FAIL")
sys.exit(0 if all(checks.values()) else 1)
EOF
CHECK_RC=$?
echo "end=$(date -u +%FT%TZ)"
# the job state must be FAILED whenever a check, the merge or any process failed
[ "$CHECK_RC" -eq 0 ] && [ "$MERGE_RC" -eq 0 ] && [ "$PROC_FAIL" -eq 0 ] || exit 1
