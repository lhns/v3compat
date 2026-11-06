#!/bin/bash
# Stress of what is on main. Args: kind runs
. lab/loop.sh
RUN="build/inst/bin/v3compat-run --"
DRRUN="$(ls -d ~/.cache/v3compat/dr-*/)bin64/drrun"
RS=build/test/regstress
mkdir -p lab-out/core; sudo sysctl -q kernel.core_pattern="$PWD/lab-out/core/core.%p"; ulimit -c unlimited
OLD="-ignore_syscalls"
case "$1" in
sig) loop sig $2 60 $RUN $RS 8 10 sig jit ;;
regc) for k in "jit" "sig" "idle jit"; do loop regc "$2" 60 $RUN $RS 8 10 $k; done ;;
regd) for k in "jit" "sig" "sig jit"; do loop regd "$2" 60 $DRRUN -no_enable_reset -no_ignore_syscalls -- $RS 8 10 $k; done ;;
short) loop short $2 60 $RUN $RS 8 1 sig jit ;;
shortold) V3COMPAT_DR_OPTS="$OLD" loop short $2 60 $RUN $RS 8 1 sig jit ;;
gather) loop gather $2 120 $RUN build/test/gather ;;
m32)
  gcc -O1 -static lab/m32.c -o m32s; gcc -O1 lab/m32.c -o m32p
  echo "kernel $(uname -r), randomize_va_space $(cat /proc/sys/kernel/randomize_va_space)"
  $RUN ./m32s maps > lab-out/m32-maps-static.txt; $RUN ./m32p maps > lab-out/m32-maps-pie.txt; ./m32s maps > lab-out/m32-maps-native.txt
  cat lab-out/m32-maps-static.txt
  loop m32native $2 30 ./m32s; loop m32static $2 30 $RUN ./m32s; loop m32pie $2 30 $RUN ./m32p
  head -c 6000 lab-out/m32static.fail1.err 2>/dev/null
  ;;
tests) loop gather $2 120 $RUN build/test/gather; loop seg1 $2 120 $RUN build/test/seg_threads; loop seg2 $2 120 $RUN build/test/seg_operand ;;
duck)
  img='duckdb/duckdb:2.0.0@sha256:453caf4f1c445ee8363acaa6cd383751396d2eae0d02f362ada64d87267a4221'
  docker pull -q "$img" >/dev/null; c=$(docker create "$img"); docker cp "$c:/duckdb" ./duckdb 2>/dev/null; docker rm "$c" >/dev/null
  sh test/apps/duckdb.sh "" ./duckdb > lab-out/native.txt
  sh test/apps/duckdb.sh "build/inst/bin/v3compat-run --" ./duckdb $2 > lab-out/client.txt 2>lab-out/client.err </dev/null; echo "duckdb.sh rc=$?"
  diff <(sed 's/ wall.*//' lab-out/native.txt | sort) <(sed 's/ wall.*//' lab-out/client.txt | sort -u) | head -20; echo "RESULT duck: diff rc=${PIPESTATUS[0]}, runs $2"; cat lab-out/client.err | head -5 ;;
esac
ls lab-out/core | wc -l
rm -f lab-out/core/core.*
