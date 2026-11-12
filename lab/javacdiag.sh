#!/bin/bash
# javacdiag.sh CONFIG RUNS: javac on the diagnostic build that classifies DynamoRIO-internal faults.
# CONFIG: client | empty | dr | dr-default; WL: javac | java (the Java benchmark). JVM error reports are kept.
set -u
C=$1; N=${2:-30}; WL=${3:-javac}
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
L="-no_enable_reset -no_ignore_syscalls"
case $C in client) P="build/inst/bin/v3compat-run --";; empty) P="${DR}bin64/drrun $L -c /tmp/null-b/libnull.so --";;
  dr) P="${DR}bin64/drrun $L --";; dr-default) P="${DR}bin64/drrun --";; esac
mkdir -p /tmp/bench/javac-out lab-out/fail
bad=0
for i in $(seq "$N"); do
  E="-XX:ErrorFile=$PWD/lab-out/fail/hs_err.$WL.$C.$i.%p.log"
  if [ $WL = javac ]; then timeout 1200 $P javac -J-XX:UseAVX=2 -J$E -d /tmp/bench/javac-out bench/Bench.java >/tmp/o 2>/tmp/e
  else timeout 1200 $P java -XX:UseAVX=2 $E -cp /tmp/bench/classes Bench 8 1 >/tmp/o 2>/tmp/e; fi
  rc=$?
  [ $rc = 0 ] && continue
  bad=$((bad+1)); cp /tmp/e lab-out/fail/$WL.$C.$i.err; cp /tmp/o lab-out/fail/$WL.$C.$i.out
  hs=$(ls lab-out/fail/hs_err.$WL.$C.$i.* 2>/dev/null | head -1)
  [ -n "$hs" ] && echo "HSERR $WL $C $i $(grep -m1 -E '^# +(SIGSEGV|SIGILL|SIGBUS|Internal Error|guarantee|assert)' $hs) | $(grep -m1 -E '^# Problematic frame' -A1 $hs | tail -1)"
  echo "FAIL $WL $C $i rc=$rc $(grep -m1 'V3DIAG fault' /tmp/e) $(grep -m1 -c 'V3DIAG forge' /tmp/e) forges"
done
echo "RESULT $WL $C: $bad of $N bad"
DBG=${DR}lib64/release/libdynamorio.so.debug
for off in $(grep -oh 'in_dr=1 [^>]*dr_off=0x[0-9a-f]*' lab-out/fail/*.err 2>/dev/null | grep -oE 'dr_off=0x[0-9a-f]+' | cut -d= -f2 | sort -u); do
  echo "SYM $off: $(addr2line -f -i -e $DBG $(printf '%#x' $((0x71000000 + off))) | tr '\n' ' ')"
done
