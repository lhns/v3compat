#!/bin/bash
# traceflush.sh RUNS: test/trace_flush under base (/tmp/base, origin/main's pin) and the branch's pin, interleaved.
set -u
grep -m1 'model name' /proc/cpuinfo
T=build/test/trace_flush
declare -A R=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=build/inst/bin/v3compat-run)
declare -A bad=([base]=0 [new]=0)
for i in $(seq "${1:-20}"); do for c in base new; do
  timeout 120 ${R[$c]} -- $T 10 >/tmp/o 2>/tmp/e; rc=$?
  echo "RUN $c $i rc=$rc $(head -1 /tmp/o) $(grep -oE 'crash at PC 0x[0-9a-f]+|^Base: 0x[0-9a-f]+' /tmp/e | tr '\n' ' ')"
  [ $rc = 0 ] || bad[$c]=$((bad[$c]+1))
done; done
for c in base new; do echo "RESULT trace_flush $c: ${bad[$c]} of ${1:-20} bad"; done
