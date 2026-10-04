#!/bin/bash
# b3_task.sh <k> <cap> <g> <depth> <i> <N> <result_dir>: one b3core shard (i of N) for HyperQueue.
# Writes shard-<i>.jsonl (b3core's own record, read by merge_b3_shards.py) and shard-<i>.meta.json
# (exit code, binary sha256, pinned cores, wall time). Never overwrites: a rerun goes to a new directory.
set -u
k=$1; cap=$2; g=$3; depth=$4; i=$5; N=$6; out=$7
BIN=${B3_BIN:-/project/project_465003389/green27/b3/b3core-portable}
res="$out/shard-$i.jsonl"
[ -e "$res" ] && { echo "refusing: $res exists" >&2; exit 3; }
tmp=$(mktemp "$out/.shard-$i.XXXXXX")
sha=$(sha256sum "$BIN" | cut -c1-64)
cpus=$(taskset -pc $$ | sed 's/.*: //')
t0=$(date +%s.%N)
"$BIN" --k "$k" --cap "$cap" --g "$g" --mode canonical --variant multiset --threads 1 \
       --prefix-depth "$depth" --shard "$i/$N" --output "$tmp" > /dev/null
rc=$?
t1=$(date +%s.%N)
printf '{"shard":%d,"N":%d,"rc":%d,"bin_sha256":"%s","host":"%s","cpus":"%s","wall":%s}\n' \
    "$i" "$N" "$rc" "$sha" "$(hostname)" "$cpus" "$(awk -v a="$t0" -v b="$t1" 'BEGIN { print b - a }')" \
    > "$out/shard-$i.meta.json"
[ "$rc" -eq 0 ] && mv "$tmp" "$res"
exit "$rc"
