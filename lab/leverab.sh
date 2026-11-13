#!/bin/bash
# LAB ONLY: on top of the #9 pin (2c501c4): the i#1760 check skipped (this branch's pin, a stub) and -protect_mask 0,
# interleaved per rep; then one perf-stat run per config. Also the kernel version and DR's rwx mappings.
set -u
REPS=${1:-6}
grep -m1 'model name' /proc/cpuinfo; nproc; echo "KERNEL $(uname -r)"
grep -c PROCMAP_QUERY /usr/include/linux/fs.h | sed 's/^/PROCMAP_QUERY in headers: /'
rm -rf /tmp/n9 && git worktree add -q /tmp/n9 HEAD && sed -i 's/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT 2c501c46186934b8d11571d950d402c26ad04342)/' /tmp/n9/cmake/FetchDynamoRIO.cmake
cmake -S /tmp/n9 -B /tmp/n9/build >/dev/null && cmake --build /tmp/n9/build -j >/dev/null || { echo "RESULT n9 build failed"; exit 1; }
NDR=~/.cache/v3compat/dr-2c501c46186934b8d11571d950d402c26ad04342; SDR=~/.cache/v3compat/dr-42319db295d71ead8bd00cfdc6b64bf679eac0cc
ls -d $NDR $SDR || exit 1
echo "RWX under DR: $($NDR/bin64/drrun -- bash -c 'grep -E " r[w-]x[ps] " /proc/self/maps' | awk '{print $2}' | sort | uniq -c | tr '\n' ' ')"
B=/tmp/bench; mkdir -p $B/javac-out
DRO="-no_enable_reset -no_ignore_syscalls"
declare -A RUN=([n9-client]="/tmp/n9/build/inst/bin/v3compat-run --" [pm0-client]="env V3COMPAT_DR_OPTS=-protect_mask\ 0 /tmp/n9/build/inst/bin/v3compat-run --"
                [stub-client]="$PWD/build/inst/bin/v3compat-run --"
                [n9-dr]="$NDR/bin64/drrun $DRO --" [pm0-dr]="$NDR/bin64/drrun $DRO -protect_mask 0 --" [stub-dr]="$SDR/bin64/drrun $DRO --")
declare -A W=([java-version]="java -XX:UseAVX=2 -version"
              [javac]="javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java"
              [node]="node bench/jit.js 8 1")
CFGS="n9-client pm0-client stub-client n9-dr pm0-dr stub-dr"
for r in $(seq $REPS); do
  for w in java-version javac node; do
    for c in $CFGS; do
      /usr/bin/time -f '%e %U %S' -o /tmp/t timeout 1200 bash -c "${RUN[$c]} ${W[$w]}" >/tmp/o 2>/tmp/e; rc=$?
      echo "RESULT $w $c rep=$r rc=$rc wall/user/sys=$(tail -1 /tmp/t | tr ' ' /) out=$(sha256sum </tmp/o | cut -c1-12)"
    done
  done
done
for w in java-version javac node; do
  for c in $CFGS; do
    sudo perf stat -x, -e syscalls:sys_enter_read,syscalls:sys_enter_openat,syscalls:sys_enter_mprotect,signal:signal_deliver -o /tmp/ps \
      timeout 1200 bash -c "${RUN[$c]} ${W[$w]}" >/dev/null 2>&1; rc=$?
    echo "COUNT $w $c rc=$rc $(grep -E 'syscalls|signal' /tmp/ps | awk -F, '{sub(/.*:sys_enter_|.*:/,"",$3); printf "%s=%s ", $3, $1}')"
  done
done
