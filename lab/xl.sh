#!/bin/bash
# xl.sh N: Java benchmark with frequent safepoints and javac, in four configurations, on a fork build that prints a
# synchronous fault whose translated pc is not a memory instruction (V3XL). Keeps JVM error reports.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
[ -x ${DR}bin64/drrun ] || { echo "RESULT no DynamoRIO build"; exit 1; }
L="-no_enable_reset -no_ignore_syscalls"
cmd() { case $1 in client) echo "build/inst/bin/v3compat-run --";;
  client-default) echo "${DR}bin64/drrun -c $PWD/build/inst/libexec/v3compat/libv3compat.so --";;
  dr) echo "${DR}bin64/drrun $L --";; dr-default) echo "${DR}bin64/drrun --";; esac; }
C="dr client"
mkdir -p lab-out/fail /tmp/bench/javac-out
declare -A bad xl; for c in $C; do for w in java javac; do bad[$c-$w]=0; xl[$c-$w]=0; done; done
for i in $(seq $1); do for w in java; do for c in $C; do
  P=$(cmd $c); E="-XX:ErrorFile=$PWD/lab-out/fail/hs_err.$w.$c.$i.%p.log"
  if [ $w = java ]; then timeout 900 $P java -XX:UseAVX=2 $E -cp /tmp/bench/classes Bench 8 1 all sp >/tmp/o 2>/tmp/e
  else timeout 1200 $P javac -J-XX:UseAVX=2 -J$E -d /tmp/bench/javac-out bench/Bench.java >/tmp/o 2>/tmp/e; fi
  rc=$?
  n=$(grep -c 'V3XL sig' /tmp/e); [ $n -gt 0 ] && { xl[$c-$w]=$((xl[$c-$w]+1)); cp /tmp/e lab-out/fail/xl.$w.$c.$i.err; echo "XL $w $c $i rc=$rc $(grep -m1 'V3XL sig' /tmp/e)"; }
  if [ $rc != 0 ]; then bad[$c-$w]=$((bad[$c-$w]+1)); cp /tmp/e lab-out/fail/$w.$c.$i.err; grep "V3F" /tmp/e | tail -3 | sed "s/^/V3F $c $i /"
    hs=$(ls lab-out/fail/hs_err.$w.$c.$i.* 2>/dev/null | head -1)
    echo "FAIL $w $c $i rc=$rc xl=$n $( [ -n "$hs" ] && grep -m1 -E '^# +(SIGSEGV|SIGILL|SIGBUS|Internal Error|guarantee)' $hs) $(grep -m1 -oE 'refusing [^\n]*|crash at PC 0x[0-9a-f]+' /tmp/e)"; fi
done; done; done
for c in $C; do for w in java javac; do echo "RESULT $w $c: ${bad[$c-$w]} of $1 bad, ${xl[$c-$w]} with a V3XL"; done; done
