#!/bin/bash
# dralone.sh: trace_flush on DynamoRIO alone with reset off, under lab-only SIGSEGV prints. Positive control first.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
echo "V3DIAG strings in libdynamorio: $(strings -a ${DR}lib64/release/libdynamorio.so | grep -c V3DIAG)"
gcc -O0 lab/probe.c -o /tmp/probe
${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -- /tmp/probe >/dev/null 2>/tmp/e; echo "CONTROL rc=$?"; grep V3DIAG /tmp/e | head -5 | sed 's/^/CONTROL /'
T=$PWD/build/test/trace_flush
bad=0
for i in $(seq 30); do
  timeout 120 ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -- $T 2 >/tmp/o 2>/tmp/e; rc=$?
  if [ $rc != 0 ]; then
    bad=$((bad+1)); echo "RUN $i rc=$rc"
    echo "DIAG run $i: $(grep -c 'segv-in' /tmp/e) in, $(grep -c 'segv-out' /tmp/e) out, $(grep -c passing /tmp/e) passing, $(grep -c notwritable /tmp/e) notwritable"
    grep V3DIAG /tmp/e | grep -v 'segv-' | tail -14 | sed "s/^/DIAG run $i tail /"
  fi
done
echo "RESULT dr reset off: $bad of 30 bad"
