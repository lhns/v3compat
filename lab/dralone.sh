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
    mkdir -p lab-out; grep -h 'V3DIAG forge' /tmp/e >> lab-out/forge.txt
    grep V3DIAG /tmp/e | grep -vE 'segv-|inherit|sighand' | tail -6 | sed "s/^/DIAG run $i tail /"
  fi
done
echo "RESULT dr reset off: $bad of 30 bad"
# Symbolize each forge's return address (offset from the library's start, link base 0x71000000).
DBG=${DR}lib64/release/libdynamorio.so.debug
for ra in $(grep -ohE 'forge .* ra0=0x[0-9a-f]+' lab-out/forge.txt 2>/dev/null | grep -oE 'ra0=0x[0-9a-f]+' | cut -d= -f2 | sort -u); do
  echo "SYM ra0 $ra: $(addr2line -f -i -e $DBG $(printf '%#x' $((0x71000000 + ra))) | tr '\n' ' ')"
done
