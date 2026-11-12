#!/bin/bash
# ab.sh REPS: base (/tmp/base build) against new (./build), interleaved per rep on this runner.
set -u
REPS=${1:-4}
B=/tmp/bench
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
declare -A BB=([base]=/tmp/base/build/test/bench_bmi [new]=$PWD/build/test/bench_bmi)
grep -m1 'model name' /proc/cpuinfo; nproc
digest() { grep -vE '^(round|total|micro) ' "$1" | sed -E 's/[0-9]+\.[0-9]+ (s|ms)//g' | sha256sum | cut -c1-12; }
one() { # rep cfg workload cmd...
  local r=$1 c=$2 w=$3; shift 3
  /usr/bin/time -f '%e' -o /tmp/p-time timeout 900 "$@" >/tmp/p-out 2>/tmp/p-err; local rc=$?
  echo "RESULT $w $c rep=$r rc=$rc wall=$(tail -1 /tmp/p-time) digest=$(digest /tmp/p-out)"
  grep -E '^(round|total|micro) ' /tmp/p-out | sed "s/^/DETAIL $w $c rep=$r /"
  [ $rc != 0 ] && { echo "STDERR-HEAD $w $c rep=$r"; head -c 600 /tmp/p-err | head -8; }
}
SQL=$PWD/test/apps/duckdb-varied.sql
one 0 native duckdb-compute ./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'
one 0 native duckdb-varied ./duckdb -csv -noheader -f $SQL
one 0 native java java -XX:UseAVX=2 -cp $B/classes Bench 8 1
one 0 native node node bench/jit.js 8 1
for r in $(seq $REPS); do
  for c in base new; do
    # the new bench binary under each launcher: base runs these forms through helpers, new inline
    for i in 1 2 3; do ${RUN[$c]} -- ${BB[new]} | sed "s/^/BENCH $c rep=$r /"; done
    one $r $c duckdb-compute ${RUN[$c]} -- ./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'
    one $r $c duckdb-varied ${RUN[$c]} -- ./duckdb -csv -noheader -f $SQL
    one $r $c java-version ${RUN[$c]} -- java -XX:UseAVX=2 -version
    one $r $c java ${RUN[$c]} -- java -XX:UseAVX=2 -cp $B/classes Bench 8 1
    one $r $c node ${RUN[$c]} -- node bench/jit.js 8 1
  done
done
