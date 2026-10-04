#!/bin/bash
# hq_worker.sh <server_dir> <workers> [compact|spread]: start one HyperQueue worker on this node, using
# <workers> physical cores of this Slurm step, one hardware thread per core: the first ones (compact,
# default; 64 of 128 means one socket) or evenly spaced over the node (spread: both sockets and every
# CCD, so a half-full node keeps the memory bandwidth of the whole node). Passing HQ
# a bare count makes it number the CPUs from 0, which on a shared node are not the job's cores and
# makes every pinned task fail (seen on LUMI small, job 22509787).
set -euo pipefail
SRV=$1; N=$2; PLACEMENT=${3:-compact}
LIST=$(python3 - "$N" "$PLACEMENT" <<'PYEOF'
import os
import sys

n, placement = int(sys.argv[1]), sys.argv[2]
cores, groups = [], set()
for cpu in sorted(os.sched_getaffinity(0)):
    with open(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list") as f:
        group = f.read().strip()
    if group not in groups:
        groups.add(group)
        cores.append(cpu)
if n > len(cores):
    sys.exit(f"asked for {n} workers, only {len(cores)} physical cores available")
chosen = cores[:n] if placement == "compact" else [cores[i * len(cores) // n] for i in range(n)]
print("[" + ",".join(str(c) for c in chosen) + "]")
PYEOF
)
echo "$(hostname): hq worker ($PLACEMENT) on cores $LIST; inherited $(grep SigBlk /proc/$$/status)"
# Processes started by srun on LUMI arrive with SIGCHLD blocked (SigBlk 0x10000, logged above; jobs
# 22510135 and 22510273). The worker then never learns that a task exited: finished tasks stay zombies
# and HyperQueue shows them RUNNING forever. A blocked mask survives exec, so clear it right before.
# optional memory budget for memory-aware admission (tasks declare --resource memory=<MB>)
MEMRES=()
[ -n "${WORKER_MEM_MB:-}" ] && MEMRES=(--resource "memory=sum($WORKER_MEM_MB)")
exec python3 -c 'import os, signal, sys; signal.pthread_sigmask(signal.SIG_SETMASK, set()); os.execvp(sys.argv[1], sys.argv[1:])' \
    hq worker start --server-dir="$SRV" --cpus="$LIST" --manager=slurm ${MEMRES[@]+"${MEMRES[@]}"}
