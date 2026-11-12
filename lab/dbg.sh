#!/bin/bash
# dbg.sh: javac under a debug build of the fork with a null client; at the first assert or crash, DR pauses
# (-pause_on_error) and gdb attaches for every thread's backtrace.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$(ls -d ~/.cache/v3dbg/dr-*[0-9a-f]/); LIB=${DR}lib64/debug/libdynamorio.so; DBG=$LIB.debug
ls -la "$LIB" "$DBG"
JAVAC="javac -J-XX:UseAVX=2 -d /tmp/bench/javac-out bench/Bench.java"
for i in 1 2 3 4; do
  echo "== run $i"
  timeout 2400 ${DR}bin64/drrun -debug -c /tmp/null-dbg/libnull.so -pause_on_error -no_enable_reset -no_ignore_syscalls -- $JAVAC >/dev/null 2>/tmp/pe &
  w=$!
  for t in $(seq 2400); do grep -qE 'ASSERT|Received SIG|internal crash|CURIOSITY' /tmp/pe 2>/dev/null && break; kill -0 $w 2>/dev/null || break; sleep 1; done
  sleep 3
  head -c 2500 /tmp/pe
  if ! grep -qE 'ASSERT|Received SIG|internal crash' /tmp/pe; then echo "no failure (rc of wait follows)"; wait $w; echo "rc=$?"; continue; fi
  pid=$(pgrep -f 'javac-out' | head -1)
  base=$(grep -m1 'libdynamorio.so' /proc/$pid/maps | cut -d- -f1)
  echo "pid=$pid base=0x$base"
  sudo gdb -q -batch -p $pid -ex "add-symbol-file $DBG -o $(printf '%#x' $((0x$base - 0x71000000)))" \
    -ex "thread apply all bt 25" 2>&1 | grep -vE '^\[New LWP|^warning:|^Reading symbols' | grep -vE '^#[0-9]+ +0x[0-9a-f]+ in \?\? \(\)$' | head -400
  sudo kill -9 $pid 2>/dev/null; wait
done
