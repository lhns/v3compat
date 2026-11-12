#!/bin/bash
# javac.sh RUNS: the javac crash (plan, "Open items"): javac compiling bench/Bench.java under the client, under an empty
# client, and on DynamoRIO alone, RUNS times each. A crash prints its PC as an offset into libdynamorio.
set -u
N=${1:-8}
grep -m1 'model name' /proc/cpuinfo
DR=$(ls -d ~/.cache/v3compat/dr-*[0-9a-f]/ | head -1)
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
mkdir -p /tmp/bench/javac-out
JAVAC="javac -J-XX:UseAVX=2 -d /tmp/bench/javac-out bench/Bench.java"
declare -A C=([client]="$PWD/build/inst/bin/v3compat-run --"
              [empty-client]="${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c /tmp/null-b/libnull.so --"
              [dr-alone]="${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls --")
for c in client empty-client dr-alone; do
  bad=0; where=""
  for i in $(seq "$N"); do
    timeout 1200 ${C[$c]} $JAVAC >/dev/null 2>/tmp/e; rc=$?
    [ $rc = 0 ] && continue
    bad=$((bad+1)); pc=$(grep -oE 'crash at PC 0x[0-9a-f]+' /tmp/e | head -1 | awk '{print $4}'); base=$(grep -oE '^Base: 0x[0-9a-f]+' /tmp/e | head -1 | awk '{print $2}')
    if [ -n "$pc" ] && [ -n "$base" ]; then where="$where $(printf '%#x' $((pc - base)))"; else where="$where rc=$rc"; fi
  done
  echo "RESULT javac $c: $bad of $N bad;$where"
done
