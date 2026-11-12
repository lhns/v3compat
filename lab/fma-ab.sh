#!/bin/bash
# fma-ab.sh REPS: FMA cost, base (/tmp/base build) against new (./build), interleaved per rep on this runner.
set -u
REPS=${1:-4}
B=/tmp/bench
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
grep -m1 'model name' /proc/cpuinfo; nproc
$B/fma 1 | sed 's/^/NATIVE /'
for r in $(seq "$REPS"); do
  for c in base new; do
    ${RUN[$c]} -- $B/fma 1 micro | sed "s/^/RESULT fma $c rep=$r /"
    ${RUN[$c]} -- $B/fma 1 kernels | sed "s/^/RESULT kernel $c rep=$r /"
    ${RUN[$c]} -- java -XX:UseAVX=2 -cp $B/classes Bench 8 1 >/tmp/j-out 2>/tmp/j-err; rc=$?
    grep -E '^(round arrays|total|checksum)' /tmp/j-out | sed "s/^/RESULT java $c rep=$r rc=$rc /"
  done
done
java -XX:UseAVX=2 -cp $B/classes Bench 8 1 | grep -E '^checksum' | sed 's/^/NATIVE java /'
