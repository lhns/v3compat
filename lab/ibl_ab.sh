#!/bin/bash
# ibl_ab.sh REPS: the IBL target_delete fix (this branch's pin) against main's pin, interleaved on one runner:
# trace_flush failures, the IBL microbench and JIT/duckdb wall times. /tmp/old is the same tree at main's pin.
set -u
REPS=${1:-5}
grep -m1 'model name' /proc/cpuinfo
pin() { sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' $1/cmake/FetchDynamoRIO.cmake; }
declare -A RUN=([old]=/tmp/old/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
declare -A DRR=([old]=$HOME/.cache/v3compat/dr-$(pin /tmp/old)/bin64/drrun [new]=$HOME/.cache/v3compat/dr-$(pin .)/bin64/drrun)
echo "old pin $(pin /tmp/old), new pin $(pin .)"
T=$PWD/build/test/trace_flush; I=/tmp/bench/ibl
declare -A bad
for c in old new; do bad[$c-client]=0; bad[$c-dr]=0; done
for i in $(seq 40); do for c in old new; do
  timeout 120 ${RUN[$c]} -- $T 2 >/dev/null 2>&1 || bad[$c-client]=$((bad[$c-client]+1))
  timeout 120 ${DRR[$c]} -no_enable_reset -no_ignore_syscalls -- $T 2 >/dev/null 2>&1 || bad[$c-dr]=$((bad[$c-dr]+1))
done; done
for k in old-client new-client old-dr new-dr; do echo "RESULT trace_flush $k: ${bad[$k]} of 40 bad"; done
t() { /usr/bin/time -f '%e' -o /tmp/tt timeout 1200 "$@" >/tmp/to 2>/tmp/te; echo "rc=$? wall=$(tail -1 /tmp/tt)"; }
for r in $(seq $REPS); do for c in old new; do
  echo "RESULT ibl dr $c rep=$r $(${DRR[$c]} -no_enable_reset -no_ignore_syscalls -- $I 100000000 | grep micro | tr '\n' ' ')"
  echo "RESULT ibl client $c rep=$r $(${RUN[$c]} -- $I 100000000 | grep micro | tr '\n' ' ')"
  echo "RESULT node $c rep=$r $(t ${RUN[$c]} -- node bench/jit.js 8 1)"
  echo "RESULT java-version $c rep=$r $(t ${RUN[$c]} -- java -XX:UseAVX=2 -version)"
  mkdir -p /tmp/bench/javac-out
  echo "RESULT javac $c rep=$r $(t ${RUN[$c]} -- javac -J-XX:UseAVX=2 -d /tmp/bench/javac-out bench/Bench.java)"
  echo "RESULT duckdb-compute $c rep=$r $(t ${RUN[$c]} -- ./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)')"
  echo "RESULT duckdb-varied $c rep=$r $(t ${RUN[$c]} -- ./duckdb -csv -noheader -f test/apps/duckdb-varied.sql)"
done; done
