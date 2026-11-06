#!/bin/bash
. lab/loop.sh
grep -m1 "model name" /proc/cpuinfo
RS=build/test/regstress
git clone -q --filter=blob:none https://github.com/lhns/dynamorio drsrc && git -C drsrc checkout -q $DRSHA && git -C drsrc submodule update --init --depth 1 third_party/elfutils
cmake -S drsrc -B drb -DCMAKE_ASM_COMPILER=as -DDEBUG=OFF -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF -DBUILD_SAMPLES=OFF -DBUILD_CLIENTS=OFF >/dev/null && cmake --build drb -j >/dev/null 2>&1
LIB=drb/lib64/release/libdynamorio.so; file $LIB | cut -c1-120
ulimit -c 0
N=$1; K=$2; shift 2
if [ "$K" = dr ]; then loop res $N 60 drb/bin64/drrun -stderr_mask 15 -no_enable_reset -no_ignore_syscalls -- $RS 8 1 "$@"
else V3COMPAT_DR_OPTS="-stderr_mask 15" loop res $N 60 build/inst/bin/v3compat-run -- $RS 8 1 "$@"; fi
for f in lab-out/res.fail*.err; do [ -f "$f" ] || continue; echo "== $f"; grep -a -v '^WARNING' "$f" | grep -a -i -E "crash|error|0x0000" | head -14 | cut -c1-200
  for a in $(grep -a -o 'libdynamorio.so+0x[0-9a-f]*' "$f" | head -12 | sed 's/.*+//'); do echo "  $a $(addr2line -f -C -e $LIB $a | tr '\n' ' ')"; done; done
