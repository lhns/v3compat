#!/bin/bash
# LAB ONLY: fork lab-procmap-query-verify (e25b50f) answers every query both ways and prints LABV counts at exit:
# compared, mismatches (two ioctls agreeing against the walk), ENOENT where the walk found a mapping, races.
set -u
grep -m1 'model name' /proc/cpuinfo; nproc; echo "KERNEL $(uname -r)"
rm -rf /tmp/ver && git worktree add -q /tmp/ver HEAD && sed -i 's/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT e25b50ffabdc31b2f2142e4ff1750e36ca1d7733)/' /tmp/ver/cmake/FetchDynamoRIO.cmake
cmake -S /tmp/ver -B /tmp/ver/build -DV3COMPAT_DR_CACHE=/tmp/drc >/dev/null && cmake --build /tmp/ver/build -j >/dev/null || { echo "RESULT verify build failed"; exit 1; }
VDR=/tmp/drc/dr-e25b50ffabdc31b2f2142e4ff1750e36ca1d7733
B=/tmp/bench; mkdir -p $B/javac-out
declare -A RUN=([client]="/tmp/ver/build/inst/bin/v3compat-run --" [dr]="$VDR/bin64/drrun -no_enable_reset -no_ignore_syscalls --")
W=("java -XX:UseAVX=2 -version" "javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java" "node bench/jit.js 8 1"
   "./duckdb -csv -noheader -f test/apps/duckdb-varied.sql" "java -XX:UseAVX=2 -cp $B/classes Bench 8 1"
   "python3 -c 'import json,ssl,sqlite3; print(1)'" "bash -c 'ls / | wc -l; cat /proc/self/maps | wc -l'"
   "build/test/regstress 8 3 sig jit")
for c in client dr; do
  for w in "${W[@]}"; do
    timeout 1200 bash -c "${RUN[$c]} $w" >/dev/null 2>/tmp/e; rc=$?
    echo "RESULT $c rc=$rc '$w': $(grep '^LABV pid' /tmp/e | awk '{for(i=4;i<NF;i+=2) s[$i]+=$(i+1)} END {for(k in s) printf "%s=%d ", k, s[k]}') procs=$(grep -c '^LABV pid' /tmp/e)"
    grep '^LABV diff' /tmp/e | head -20
  done
done
