#!/bin/bash
# fmaone.sh REPS: does running a packed form first change the scalar rows? base and new, interleaved.
# Sequences of rows in one process: "sd", "pd_ymm sd", "sd pd_ymm sd".
set -u
REPS=${1:-6}
gcc -O3 -march=x86-64-v3 bench/fmaone.c -o /tmp/bench/fmaone -lm 2>/dev/null || exit 1
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
echo "model name $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"
mkdir -p lab-out
for r in $(seq "$REPS"); do
  for c in base new; do
    for seq in "sd" "ss" "pd_ymm sd" "ps_ymm sd" "sd pd_ymm sd" "ss sd" "ss sd pd_xmm sd"; do
      echo "RESULT $c rep=$r [$seq] -> $(${RUN[$c]} -- /tmp/bench/fmaone $seq 2>&1 | tr '\n' ' ')" | tee -a lab-out/fmaone.txt
    done
  done
done
