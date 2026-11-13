#!/bin/bash
# LAB ONLY: V3COMPAT_UNPROTECTED_GENCODE=1 against the default, same build, interleaved per rep; then perf-stat counts.
set -u
SHORT=${1:-20}; LONG=${2:-6}
grep -m1 'model name' /proc/cpuinfo; nproc; echo "KERNEL $(uname -r)"
L=$PWD/build/inst/bin/v3compat-run
echo "MAPS default: $($L -- build/test/proc gencode) / on: $(V3COMPAT_UNPROTECTED_GENCODE=1 $L -- build/test/proc gencode)"
for v in 0 1; do echo "ANON-RX unprotected=$v: $(env V3COMPAT_UNPROTECTED_GENCODE=$v $L -- cat /proc/self/maps | awk 'NF==5 {print $2}' | sort | uniq -c | tr '\n' ' ')"; done
(cd build && ctest -R gencode --output-on-failure | tail -4)
B=/tmp/bench; mkdir -p $B/javac-out
declare -A RUN=([default]="$L --" [unprot]="env V3COMPAT_UNPROTECTED_GENCODE=1 $L --")
declare -A W=([java-version]="java -XX:UseAVX=2 -version"
              [javac]="javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java"
              [node]="node bench/jit.js 8 1"
              [duckdb]="./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'")
CFGS="default unprot"
one() { # rep workload
  for c in $CFGS; do
    /usr/bin/time -f '%e %U %S' -o /tmp/t timeout 1200 bash -c "${RUN[$c]} ${W[$2]}" >/tmp/o 2>/tmp/e; rc=$?
    echo "RESULT $2 $c rep=$1 rc=$rc wall/user/sys=$(tail -1 /tmp/t | tr ' ' /) out=$(grep -vE '^(round|total) ' /tmp/o | sha256sum | cut -c1-12)"
    [ $rc != 0 ] && head -c 400 /tmp/e | head -4 | sed "s/^/STDERR $2 $c rep=$1 /"
  done
}
for r in $(seq $SHORT); do for w in java-version node duckdb; do one $r $w; done; done
for r in $(seq $LONG); do one $r javac; done
for w in java-version javac node duckdb; do
  for c in $CFGS; do
    sudo perf stat -x, -e syscalls:sys_enter_read,syscalls:sys_enter_mprotect -o /tmp/ps \
      timeout 1200 bash -c "${RUN[$c]} ${W[$w]}" >/dev/null 2>&1; rc=$?
    echo "COUNT $w $c rc=$rc $(grep -E 'syscalls' /tmp/ps | awk -F, '{sub(/.*:sys_enter_/,"",$3); printf "%s=%s ", $3, $1}')"
  done
done
