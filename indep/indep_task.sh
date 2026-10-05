#!/bin/bash
# indep_task.sh <k> <maxel> <bounds> <depth> <i> <N> <result_dir>: one b3indep shard for HyperQueue.
# Writes shard-<i>.json (the program's JSON line plus exit code, binary sha256 and pinned cores) atomically;
# never overwrites, so a rerun of missing shards goes to a new directory.
set -u
k=$1; maxel=$2; bounds=$3; depth=$4; i=$5; N=$6; out=$7
BIN=${B3I_BIN:-/project/project_465003389/green27/b3indep/b3indep}
mkdir -p "$out"
res="$out/shard-$i.json"
[ -e "$res" ] && { echo "refusing: $res exists" >&2; exit 3; }
tmp=$(mktemp "$out/.shard-$i.XXXXXX")
line=$("$BIN" --k "$k" --maxel "$maxel" --bounds "$bounds" --depth "$depth" --shard "$i/$N" --max-print 1000)
rc=$?
python3 -c '
import json, sys
r = json.loads(sys.argv[1]) if sys.argv[1].strip() else {}
r.update({"rc": int(sys.argv[2]), "bin_sha256": sys.argv[3], "cpus": sys.argv[4]})
print(json.dumps(r))' "$line" "$rc" "$(sha256sum "$BIN" | cut -c1-64)" "$(taskset -pc $$ | sed 's/.*: //')" > "$tmp"
mv "$tmp" "$res"
exit "$rc"
