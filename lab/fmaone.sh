#!/bin/bash
# fmaone.sh REPS: packed rows with and without a zero in the last lane of both sources (_z: the vector the kernel
# cannot take whole), base and new, interleaved.
set -u
REPS=${1:-6}
gcc -O3 -march=x86-64-v3 bench/fmaone.c -o /tmp/bench/fmaone -lm 2>/dev/null || exit 1
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
echo "model name $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"
mkdir -p lab-out
for r in $(seq "$REPS"); do
  for c in base new; do
    for seq in "pd_ymm" "pd_ymm_z" "ps_ymm" "ps_ymm_z"; do
      echo "RESULT $c rep=$r [$seq] -> $(${RUN[$c]} -- /tmp/bench/fmaone $seq 2>&1 | tr '\n' ' ')" | tee -a lab-out/fmaone.txt
    done
  done
done
