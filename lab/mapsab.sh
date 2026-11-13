#!/bin/bash
# LAB ONLY: base (/tmp/base, origin/main's pin) against the branch's pin, interleaved per rep, under the
# client and on DynamoRIO alone; then one untimed perf-stat run per config for syscall counts.
set -u
REPS=${1:-6}
grep -m1 'model name' /proc/cpuinfo; nproc
pin() { sed -nE 's/^set\(V3COMPAT_DR_COMMIT ([0-9a-f]+)\)/\1/p' "$1/cmake/FetchDynamoRIO.cmake"; }
BDR=~/.cache/v3compat/dr-$(pin /tmp/base); NDR=~/.cache/v3compat/dr-$(pin .)
echo "base DR $BDR"; echo "new DR $NDR"; ls -d $BDR $NDR || exit 1
B=/tmp/bench; mkdir -p $B/javac-out
DRO="-no_enable_reset -no_ignore_syscalls"
declare -A RUN=([base-client]="/tmp/base/build/inst/bin/v3compat-run --" [new-client]="$PWD/build/inst/bin/v3compat-run --"
                [base-dr]="$BDR/bin64/drrun $DRO --" [new-dr]="$NDR/bin64/drrun $DRO --")
declare -A W=([java-version]="java -XX:UseAVX=2 -version"
              [javac]="javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java"
              [node]="node bench/jit.js 8 1"
              [duck]="./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'")
CFGS="base-client new-client base-dr new-dr"
for r in $(seq $REPS); do
  for w in java-version javac node duck; do
    for c in $CFGS; do
      /usr/bin/time -f '%e %U %S' -o /tmp/t timeout 1200 bash -c "${RUN[$c]} ${W[$w]}" >/tmp/o 2>/tmp/e; rc=$?
      echo "RESULT $w $c rep=$r rc=$rc wall/user/sys=$(tail -1 /tmp/t | tr ' ' /) out=$(sha256sum </tmp/o | cut -c1-12)"
    done
  done
done
for w in java-version javac node duck; do
  for c in $CFGS; do
    sudo perf stat -x, -e syscalls:sys_enter_read,syscalls:sys_enter_openat,syscalls:sys_enter_mprotect,signal:signal_deliver -o /tmp/ps \
      timeout 1200 bash -c "${RUN[$c]} ${W[$w]}" >/dev/null 2>&1; rc=$?
    echo "COUNT $w $c rc=$rc $(grep -E 'syscalls|signal' /tmp/ps | awk -F, '{sub(/.*:sys_enter_|.*:/,"",$3); printf "%s=%s ", $3, $1}')"
  done
done
