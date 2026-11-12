#!/bin/bash
# dralone.sh: trace_flush on DynamoRIO alone: which options avoid the SIGSEGV, and what the core shows.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
T=$PWD/build/test/trace_flush
for o in "" "-no_enable_reset" "-no_enable_reset -disable_traces" "-no_enable_reset -thread_private" "-no_enable_reset -sandbox2ro_threshold 0" "-no_enable_reset -ro2sandbox_threshold 0" "-no_enable_reset -no_shared_traces" "-reset_every_nth_pending 0"; do
  bad=0; for i in $(seq 10); do timeout 120 ${DR}bin64/drrun $o -- $T 2 >/dev/null 2>&1 || bad=$((bad+1)); done
  echo "RESULT dr [$o]: $bad of 10 bad"
done
mkdir -p lab-out/core; sudo sysctl -q kernel.core_pattern="$PWD/lab-out/core/core.%p"; ulimit -c unlimited
for i in 1 2 3 4 5 6; do timeout 120 ${DR}bin64/drrun -no_enable_reset -- $T 2 >/dev/null 2>/tmp/e; echo "core run $i rc=$?"; head -5 /tmp/e; done
DBG=${DR}lib64/release/libdynamorio.so.debug
for c in lab-out/core/core.*; do
  [ -f "$c" ] || continue
  echo "== $c"
  gdb -q -batch -c "$c" $T -ex 'info registers rip rsp rax rbx rcx rdx rsi rdi' -ex 'x/8i $pc-16' -ex 'info proc mappings' -ex 'thread apply all bt 4' 2>&1 | grep -vE '^warning|^\[New LWP' | head -120
  base=$(gdb -q -batch -c "$c" -ex 'info proc mappings' 2>/dev/null | grep -m1 libdynamorio | awk '{print $1}')
  echo "libdynamorio base $base"
  [ -n "$base" ] && gdb -q -batch -c "$c" -ex "add-symbol-file $DBG -o $(printf '%#x' $((base - 0x71000000)))" -ex 'thread apply all bt 12' 2>&1 | grep -vE '^warning|^\[New LWP|^Reading' | head -150
done
