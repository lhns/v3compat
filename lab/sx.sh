#!/bin/bash
# sx.sh N: lab/sx.c with fork children natively, on DynamoRIO alone and under the client, interleaved, N runs an arm.
# A stuck reap (@@STUCK) gets gdb on the parent and the child; cores are kept and get a backtrace.
set -u
grep -m1 'model name' /proc/cpuinfo
DRDIR=$(ls -d ~/.cache/v3compat/dr-*/ | head -1); DBG=$DRDIR/lib64/release/libdynamorio.so.debug
LIB=$(dirname $(find build/inst -name libdynamorio.so | head -1)); cp "$DBG" "$LIB/"
gcc -O2 -g -pthread lab/sx.c -o /tmp/sx
mkdir -p lab-out/core; sudo sysctl -q kernel.core_pattern="$PWD/lab-out/core/core.%p"; ulimit -c unlimited
C=lab-out/cap.txt; : > $C; caps=0
declare -A P=([native]="" [dr]="${DRDIR}bin64/drrun --" [client]="build/inst/bin/v3compat-run --") bad
gdbp() { sudo timeout 60 gdb -q -batch -p $1 -ex 'info threads' -ex 'thread apply all bt 30' 2>&1 | grep -vE '^(\[New|Reading|warning: (Could not|Unable))'; }
for a in native dr client; do bad[$a]=0; done
for i in $(seq $1); do for a in native dr client; do
  : > /tmp/o; timeout 600 ${P[$a]} /tmp/sx 300 > /tmp/o 2>/tmp/e & rp=$!; done_cap=0
  while kill -0 $rp 2>/dev/null; do
    if [ $done_cap = 0 ] && grep -q '^@@STUCK' /tmp/o; then done_cap=1
      if [ $caps -lt 6 ]; then caps=$((caps+1)); l=$(grep -m1 '^@@STUCK' /tmp/o); pp=$(echo "$l" | sed 's/.*parent=\([0-9]*\).*/\1/'); cp=$(echo "$l" | sed 's/.*child=\([0-9]*\).*/\1/')
        { echo "=== $a run $i"; echo "$l"; for t in /proc/$cp/task/*; do echo "child task ${t##*/}: $(cat $t/stat 2>&1) wchan=$(cat $t/wchan 2>&1) syscall=$(sudo cat $t/syscall 2>&1)"; grep -E 'SigPnd|SigBlk' $t/status; done
          echo "--- parent $pp"; grep -E 'Threads|SigPnd|ShdPnd|SigBlk|SigCgt' /proc/$pp/status; gdbp $pp; echo "--- child $cp"; gdbp $cp; } >> $C 2>&1; fi
    fi; sleep 0.1
  done; wait $rp; rc=$?
  if [ $rc != 0 ] || [ "$(tail -1 /tmp/o)" != PASS ]; then bad[$a]=$((bad[$a]+1)); echo "FAIL $a $i rc=$rc $(grep -hE '^@@STUCK|^after|^FAIL' /tmp/o | tr '\n' ' ' | cut -c1-700) $(head -c 200 /tmp/e | tr '\n' ' ')"
  else echo "ok $a $i $(grep -h slowest /tmp/o)"; fi
done; done
for a in native dr client; do echo "RESULT sx fork $a: ${bad[$a]} of $1 bad"; done
n=0; for c in lab-out/core/core.*; do [ -f "$c" ] || continue; n=$((n+1)); [ $n -le 4 ] || { rm -f "$c"; continue; }
  gdb -q -batch -c "$c" /tmp/sx -ex 'info threads' -ex 'thread apply all bt 30' > "$c.txt" 2>&1; zstd -q -T0 --rm "$c"; done
echo "cores: $n"
