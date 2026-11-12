#!/bin/sh
# trace_flush.sh RUN PROG [RUNS]: test/trace_flush under RUN, RUNS times (default 25); fails on the first bad run.
# A run failed in about a third of tries before the fork's IBL target_delete fix, so 25 runs miss it with p < 1e-4.
RUN=$1; P=$2; N=${3:-25}; i=0
while [ "$i" -lt "$N" ]; do
  i=$((i+1))
  out=$($RUN -- "$P" 2 2>&1); rc=$?
  [ "$rc" = 0 ] || { echo "run $i: exit $rc"; echo "$out" | tail -3; exit 1; }
done
echo "PASS $N runs"
