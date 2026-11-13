#!/bin/bash
# LAB ONLY: PROCMAP_QUERY for memquery_from_os (fork 8de178b) against its base (origin/main, pin 14836b0) and the
# ceiling of skipping the i#1760 check (stub 42319db, on #9), interleaved per rep; then one perf-stat run per config.
set -u
SHORT=${1:-20}; LONG=${2:-6}
grep -m1 'model name' /proc/cpuinfo; nproc; echo "KERNEL $(uname -r)"
build() { # dir commit
  rm -rf $1 && git worktree add -q $1 HEAD && sed -i "s/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT $2)/" $1/cmake/FetchDynamoRIO.cmake
  cmake -S $1 -B $1/build -DV3COMPAT_DR_CACHE=/tmp/drc >/dev/null && cmake --build $1/build -j >/dev/null || { echo "RESULT $1 build failed"; exit 1; }
}
build /tmp/stub 42319db295d71ead8bd00cfdc6b64bf679eac0cc
C=~/.cache/v3compat
BDR=$C/dr-14836b0e1dfbc5b5d2f089102d83a38f3d38ed94; FDR=$C/dr-8de178bfe83d3b841a969e5136e24d8f075dbff4; SDR=/tmp/drc/dr-42319db295d71ead8bd00cfdc6b64bf679eac0cc
ls -d $BDR $FDR $SDR || exit 1
B=/tmp/bench; mkdir -p $B/javac-out
DRO="-no_enable_reset -no_ignore_syscalls"
declare -A RUN=([base-client]="/tmp/base/build/inst/bin/v3compat-run --" [fix-client]="$PWD/build/inst/bin/v3compat-run --"
                [stub-client]="/tmp/stub/build/inst/bin/v3compat-run --"
                [base-dr]="$BDR/bin64/drrun $DRO --" [fix-dr]="$FDR/bin64/drrun $DRO --" [stub-dr]="$SDR/bin64/drrun $DRO --")
declare -A W=([java-version]="java -XX:UseAVX=2 -version"
              [javac]="javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java"
              [node]="node bench/jit.js 8 1"
              [duckdb]="./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'")
CFGS="base-client fix-client stub-client base-dr fix-dr stub-dr"
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
    sudo perf stat -x, -e syscalls:sys_enter_read,syscalls:sys_enter_open,syscalls:sys_enter_ioctl,syscalls:sys_enter_mprotect -o /tmp/ps \
      timeout 1200 bash -c "${RUN[$c]} ${W[$w]}" >/dev/null 2>&1; rc=$?
    echo "COUNT $w $c rc=$rc $(grep -E 'syscalls' /tmp/ps | awk -F, '{sub(/.*:sys_enter_/,"",$3); printf "%s=%s ", $3, $1}')"
  done
done
