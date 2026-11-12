#!/bin/bash
# tfdiag.sh: trace_flush natively, on DynamoRIO alone, an empty client and the client; first lines of stderr.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null
T=build/test/trace_flush
for c in native dr empty client; do
  case $c in native) P="";; dr) P="${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls --";;
    empty) P="${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c /tmp/null-b/libnull.so --";; client) P="build/inst/bin/v3compat-run --";; esac
  for s in 1 3; do
    timeout 120 $P $T $s >/tmp/o 2>/tmp/e; rc=$?
    echo "RESULT $c secs=$s rc=$rc out=[$(head -1 /tmp/o)]"; head -c 600 /tmp/e | head -6
  done
done
gdb -q -batch -ex run -ex bt -ex 'info registers rip' -ex 'x/6i $pc-8' --args $T 1 2>&1 | tail -15
