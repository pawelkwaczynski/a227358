#!/bin/bash -l
#SBATCH --account=project_465003389
#SBATCH --nodes=1
#SBATCH --hint=nomultithread
#SBATCH --time=01:00:00
#SBATCH --job-name=hq_campaign
#SBATCH --output=/scratch/project_465003389/logs/hq_campaign_%j.out
# hq_campaign.sbatch: run a list of single-core tasks on whole LUMI-C nodes through a HyperQueue pool
# (WP0-c of the EuroHPC proposal: one Slurm job, one HQ worker per node, every task pinned to its core).
#
#   whole nodes:  CAMPAIGN=... TASKS=... sbatch -p standard --exclusive --mem=0 [-N 4] [-t 6:00:00] hq_campaign.sbatch
#   test run:     CAMPAIGN=... TASKS=... WORKERS=16 sbatch -p small -c 16 --mem=16G -t 0:30:00 hq_campaign.sbatch
#   optional:     PIN=taskset|none (default taskset), TASK_LIMIT=2h (per-task limit in HyperQueue),
#                 TASK_SETUP=<file> sourced before the workers start (modules, PATH; tasks inherit it),
#                 PLACEMENT=compact|spread (which cores when WORKERS is below the core count),
#                 PRIORITY_SPLIT=128,384 (the first 128 lines run at the highest priority, the next 384 below,
#                 the rest last; HyperQueue does not start tasks in file order, so "longest first" needs this),
#                 BUCKET_MEM_MB=4000,2000,1000,600,400 with WORKER_MEM_MB=200000 (memory-aware admission: each
#                 task of a bucket declares that much memory and a worker never runs more than it holds)
#
# Each line of TASKS is one shell command that writes its own result file; the launcher keeps only
# per-task stdout/stderr and HyperQueue's task states. CAMPAIGN must not exist, so results of an older
# run are never mixed into a new one (a rerun of missing tasks gets its own CAMPAIGN directory and the
# auditor takes the union). HyperQueue does NOT start the tasks of one job in file order (LUMI job
# 22510323: the longest cubes started anywhere in the first 15 minutes); use PRIORITY_SPLIT for that.
set -euo pipefail
: "${CAMPAIGN:?set CAMPAIGN}" "${TASKS:?set TASKS}"
WORKERS=${WORKERS:-128}
PIN=${PIN:-taskset}
TASK_LIMIT=${TASK_LIMIT:-}
HQ_DIR=${HQ_DIR:-/project/project_465003389/green27/hq}   # Slurm runs a spool copy of this script, so no $0

[ -e "$CAMPAIGN" ] && { echo "refusing: $CAMPAIGN already exists"; exit 3; }
mkdir -p "$CAMPAIGN"/out "$CAMPAIGN"/err "$CAMPAIGN"/results
cp "$TASKS" "$CAMPAIGN/tasks.txt"
module use /appl/local/csc/modulefiles
module load hyperqueue/0.18.0 cray-python/3.11.7 >/dev/null 2>&1
SRV="$CAMPAIGN/hq-server"
{
    echo "slurm_job=$SLURM_JOB_ID nodes=$SLURM_JOB_NODELIST workers_per_node=$WORKERS pin=$PIN placement=${PLACEMENT:-compact}"
    echo "task_limit=${TASK_LIMIT:-none} start=$(date -u +%FT%TZ) hq=$(hq --version)"
    sha256sum "$CAMPAIGN/tasks.txt"
    echo "tasks=$(wc -l < "$CAMPAIGN/tasks.txt")"
} > "$CAMPAIGN/LAUNCH.txt"
cat "$CAMPAIGN/LAUNCH.txt"

# Tasks inherit the environment of the HQ worker, not of "hq submit", so the setup has to be in place
# before the workers start (LUMI job 22510293: every task failed with cadical not on PATH).
if [ -n "${TASK_SETUP:-}" ]; then
    set +eu; source "$TASK_SETUP"; set -eu    # module scripts do not survive -u
    sha256sum "$TASK_SETUP" >> "$CAMPAIGN/LAUNCH.txt"
fi

hq server start --server-dir="$SRV" > "$CAMPAIGN/hq-server.log" 2>&1 &
until hq server info --server-dir="$SRV" > /dev/null 2>&1; do sleep 1; done

srun --overlap --nodes="$SLURM_JOB_NUM_NODES" --ntasks-per-node=1 --cpu-bind=none \
    bash "$HQ_DIR/hq_worker.sh" "$SRV" "$WORKERS" "${PLACEMENT:-compact}" > "$CAMPAIGN/hq-workers.log" 2>&1 &
workers() { hq worker list --server-dir="$SRV" --output-mode=json 2>/dev/null \
            | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))' 2>/dev/null || echo 0; }
# wait for one worker per node, but not forever: a node whose worker does not start must not keep
# the whole allocation idle until the walltime
for _ in $(seq 150); do [ "$(workers)" -ge "$SLURM_JOB_NUM_NODES" ] && break; sleep 2; done
UP=$(workers)
[ "$UP" -ge 1 ] || { echo "no HQ worker started, giving up"; hq server stop --server-dir="$SRV"; exit 4; }
[ "$UP" -lt "$SLURM_JOB_NUM_NODES" ] && echo "WARNING: only $UP of $SLURM_JOB_NUM_NODES workers up" | tee -a "$CAMPAIGN/LAUNCH.txt"
echo "workers up: $UP at $(date -u +%FT%TZ)"

SUBMIT=(--server-dir="$SRV" --each-line="$CAMPAIGN/tasks.txt" --cpus=1
        --stdout="$CAMPAIGN/out/%{TASK_ID}.out" --stderr="$CAMPAIGN/err/%{TASK_ID}.err")
[ "$PIN" = taskset ] && SUBMIT+=(--pin=taskset)
[ -n "$TASK_LIMIT" ] && SUBMIT+=(--time-limit="$TASK_LIMIT")
set +e
# split the task list into priority buckets (one HQ job each); without PRIORITY_SPLIT it is one job
IFS=, read -r -a BUCKETS <<< "${PRIORITY_SPLIT:-}"
TOTAL=$(( $(wc -l < "$CAMPAIGN/tasks.txt") )); FIRST=1; PRIO=${#BUCKETS[@]}; NJOBS=0
for SIZE in ${BUCKETS[@]+"${BUCKETS[@]}"} "$TOTAL"; do
    LAST=$(( FIRST + SIZE - 1 )); [ "$LAST" -gt "$TOTAL" ] && LAST=$TOTAL
    [ "$FIRST" -gt "$TOTAL" ] && break
    sed -n "${FIRST},${LAST}p" "$CAMPAIGN/tasks.txt" > "$CAMPAIGN/tasks.p$PRIO.txt"
    S2=("${SUBMIT[@]}"); S2[1]="--each-line=$CAMPAIGN/tasks.p$PRIO.txt"
    MEM=""
    if [ -n "${BUCKET_MEM_MB:-}" ]; then
        IFS=, read -r -a MEMS <<< "$BUCKET_MEM_MB"
        MEM=${MEMS[$NJOBS]:-${MEMS[${#MEMS[@]}-1]}}
        S2+=(--resource "memory=$MEM")
    fi
    hq submit "${S2[@]}" --priority="$PRIO" -- bash -c 'eval "$HQ_ENTRY"'
    echo "bucket priority=$PRIO lines $FIRST-$LAST memory_mb=${MEM:-none}" | tee -a "$CAMPAIGN/LAUNCH.txt"
    NJOBS=$(( NJOBS + 1 )); FIRST=$(( LAST + 1 )); PRIO=$(( PRIO - 1 ))
done
hq job wait all --server-dir="$SRV"
echo "hq_wait_rc=$? jobs=$NJOBS" | tee -a "$CAMPAIGN/LAUNCH.txt"
for j in $(seq 1 "$NJOBS"); do
    hq job info "$j" --server-dir="$SRV" >> "$CAMPAIGN/hq-job.txt" 2>&1
    hq task list "$j" --server-dir="$SRV" --output-mode=json > "$CAMPAIGN/hq-tasks-$j.json" 2>/dev/null
done
hq worker stop all --server-dir="$SRV" > /dev/null 2>&1
hq server stop --server-dir="$SRV" > /dev/null 2>&1
echo "end=$(date -u +%FT%TZ) results=$(find "$CAMPAIGN/results" -type f -name '*.json' | wc -l)" \
    | tee -a "$CAMPAIGN/LAUNCH.txt"
