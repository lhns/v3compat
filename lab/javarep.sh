#!/bin/bash
# javarep.sh REPS: Java's checksum under base and new, interleaved, to tell a wrong result from noise.
set -u
grep -m1 'model name' /proc/cpuinfo
B=/tmp/bench
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
java -XX:UseAVX=2 -cp $B/classes Bench 8 1 | grep checksum | sed 's/^/RESULT native /'
for r in $(seq $1); do
  for c in base new; do
    timeout 900 ${RUN[$c]} -- java -XX:UseAVX=2 -cp $B/classes Bench 8 1 > /tmp/o 2>/tmp/e; rc=$?
    echo "RESULT $c rep=$r rc=$rc $(grep checksum /tmp/o)"
    [ $rc != 0 ] && head -3 /tmp/e
  done
done
