#!/bin/bash
# stale.sh WL ITERS CONFIGS...: javac or the Java benchmark, the configurations interleaved per iteration.
# CONFIG: client (launcher) | client-default (drrun -c our client, DR defaults) | empty | dr | dr-default.
# Java: every kernel's value per round against native; a mismatch names the kernel and round.
set -u
WL=$1; N=$2; shift 2
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
L="-no_enable_reset -no_ignore_syscalls"
cmd() { case $1 in client) echo "build/inst/bin/v3compat-run --";;
  client-default) echo "${DR}bin64/drrun -c $PWD/build/inst/libexec/v3compat/libv3compat.so --";;
  empty) echo "${DR}bin64/drrun $L -c /tmp/null-b/libnull.so --";; dr) echo "${DR}bin64/drrun $L --";;
  dr-default) echo "${DR}bin64/drrun --";; esac; }
mkdir -p /tmp/bench/javac-out lab-out/fail
declare -A bad wrong ok
for c in "$@"; do bad[$c]=0; wrong[$c]=0; ok[$c]=0; done
if [ $WL = java ]; then java -XX:UseAVX=2 -cp /tmp/bench/classes Bench 8 1 | grep -E '^(value|checksum)' >/tmp/native; cat /tmp/native | sed 's/^/NATIVE /'; fi
for i in $(seq "$N"); do for c in "$@"; do
  P=$(cmd $c); E="-XX:ErrorFile=$PWD/lab-out/fail/hs_err.$WL.$c.$i.%p.log"
  if [ $WL = javac ]; then timeout 1200 $P javac -J-XX:UseAVX=2 -J$E -d /tmp/bench/javac-out bench/Bench.java >/tmp/o 2>/tmp/e
  else timeout 1200 $P java -XX:UseAVX=2 $E -cp /tmp/bench/classes Bench 8 1 >/tmp/o 2>/tmp/e; fi
  rc=$?
  if [ $WL = java ] && [ $rc = 0 ] && ! diff -q <(grep -E '^(value|checksum)' /tmp/o) /tmp/native >/dev/null; then
    wrong[$c]=$((wrong[$c]+1)); cp /tmp/o lab-out/fail/WRONG.$c.$i.out; cp /tmp/e lab-out/fail/WRONG.$c.$i.err
    diff <(grep -E '^(value|checksum)' /tmp/o) /tmp/native | grep '^<' | sed "s/^/WRONG $c $i /"
  fi
  if [ $rc = 0 ]; then ok[$c]=$((ok[$c]+1)); continue; fi
  bad[$c]=$((bad[$c]+1)); cp /tmp/e lab-out/fail/$WL.$c.$i.err; cp /tmp/o lab-out/fail/$WL.$c.$i.out
  hs=$(ls lab-out/fail/hs_err.$WL.$c.$i.* 2>/dev/null | head -1)
  [ -n "$hs" ] && echo "HSERR $WL $c $i $(grep -m1 -E '^# +(SIGSEGV|SIGILL|SIGBUS|SIGTRAP|Internal Error|guarantee|assert)' $hs) | $(grep -m1 -E '^# Problematic frame' -A1 $hs | tail -1)"
  echo "FAIL $WL $c $i rc=$rc $(grep -m1 'V3DIAG fault' /tmp/e | cut -c1-200) | $(grep -c 'V3DIAG freed' /tmp/e) freed-matches | $(grep -m1 'V3DIAG freed' /tmp/e)"
done; done
for c in "$@"; do echo "RESULT $WL $c: ${bad[$c]} of $N crashed, ${wrong[$c]} wrong with exit 0, ${ok[$c]} exit 0"; done
