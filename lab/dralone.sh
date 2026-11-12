#!/bin/bash
# dralone.sh: trace_flush on DynamoRIO alone with reset off; the lab-only report of write faults judged not ours.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
T=$PWD/build/test/trace_flush
bad=0
for i in $(seq 20); do
  timeout 120 ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -- $T 2 >/tmp/o 2>/tmp/e; rc=$?
  [ $rc = 0 ] || bad=$((bad+1))
  echo "RUN $i rc=$rc $(head -1 /tmp/o)"; grep -h V3DIAG /tmp/e | head -4 | sed "s/^/DIAG run $i rc=$rc /"
done
echo "RESULT dr reset off: $bad of 20 bad"
