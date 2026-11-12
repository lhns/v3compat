#!/bin/bash
# javabench.sh RUNS: the Java benchmark under the client RUNS times; failures' stderr kept in lab-out/fail.
set -u
grep -m1 'model name' /proc/cpuinfo
bad=0
for i in $(seq "${1:-10}"); do
  timeout 1200 build/inst/bin/v3compat-run -- java -XX:UseAVX=2 -cp /tmp/bench/classes Bench 8 1 >/tmp/o 2>/tmp/e; rc=$?
  grep -h V3DIAG /tmp/e | sed 's/flusher=[0-9]* target=[0-9]*/F T/' | sort | uniq -c | sed "s/^/DIAG java run $i rc=$rc /"
  if [ $rc != 0 ]; then bad=$((bad+1)); mkdir -p lab-out/fail; cp /tmp/e lab-out/fail/java.$i.err; fi
  echo "RUN java $i rc=$rc $(grep -E '^(total|checksum)' /tmp/o | tr '\n' ' ')"
done
echo "RESULT java client: $bad of ${1:-10} bad"
