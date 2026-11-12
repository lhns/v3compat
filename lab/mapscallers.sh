#!/bin/bash
# LAB ONLY: /proc/self/maps reads by caller (fork branch lab-perf-maps-callers prints LABQ lines at exit).
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$(ls -d ~/.cache/v3compat/dr-*[0-9a-f]/ | head -1)
DBG=${DR}lib64/release/libdynamorio.so.debug
ls -l $DBG
B=/tmp/bench
mkdir -p $B/javac-out
declare -A C=([client]="$PWD/build/inst/bin/v3compat-run --"
              [dr]="${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls --")
declare -A W=([java-version]="java -XX:UseAVX=2 -version"
              [javac]="javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java"
              [node]="node bench/jit.js 8 1"
              [duck]="./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'")
sym() { # base addr -> func@file:line chain
  local base=$1; shift
  for a in "$@"; do
    [ "$a" = "0x0000000000000000" ] && continue
    local off=$((a - base))
    if [ $off -ge 0 ] && [ $off -lt $((16*1024*1024)) ]; then
      addr2line -f -i -s -e $DBG $(printf '%#x' $((off + 0x71000000))) | paste -sd' ' | awk '{printf "%s ", $0}'
      printf '| '
    else printf '%s | ' $a; fi
  done
}
for w in java-version javac node duck; do
  for c in dr client; do
    sudo perf stat -e 'syscalls:sys_enter_read,syscalls:sys_enter_openat,syscalls:sys_enter_mprotect,signal:signal_deliver' -x, -o /tmp/ps \
      bash -c "timeout 1200 ${C[$c]} ${W[$w]} >/dev/null 2>/tmp/e"; rc=$?
    echo "RESULT $w $c rc=$rc $(grep -E 'syscalls|signal' /tmp/ps | awk -F, '{printf "%s=%s ", $3, $1}')"
    grep LABQ /tmp/e > /tmp/q
    grep '^LABQ base' /tmp/q | sed "s/^/PROC $w $c /"
    # one table per process: lines after each "LABQ base" belong to that base
    awk '/^LABQ base/{b=$3; next} {print b, $0}' /tmp/q | sort -k3,3nr | head -25 | while read -r base _ calls reads r0 r1 r2 r3 r4 r5; do
      echo "CALLER $w $c calls=$calls reads=$reads :: $(sym $base $r0 $r1 $r2 $r3 $r4 $r5)"
    done
    grep -v LABQ /tmp/e | head -5
  done
done
