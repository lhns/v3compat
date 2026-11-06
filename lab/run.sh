#!/bin/bash
. lab/loop.sh
grep -m1 "model name" /proc/cpuinfo
grep -q avx512f /proc/cpuinfo || { echo "RESULT skip: no AVX-512"; exit 0; }
RUN="build/inst/bin/v3compat-run --"
RS=build/test/regstress
mkdir -p lab-out/core; sudo sysctl -q kernel.core_pattern="$PWD/lab-out/core/core.%p"; ulimit -c unlimited
N=$1; shift
loop segv $N 60 $RUN $RS 8 1 "$@"
for c in lab-out/core/core.*; do [ -f "$c" ] || continue; gdb -q -batch -c "$c" $RS -ex 'info registers' -ex 'x/8i $pc' > "$c.txt" 2>&1; zstd -q -T0 --rm "$c"; done
ls -d lab-out/core/*.zst 2>/dev/null | tail -n +5 | xargs -r rm -f
