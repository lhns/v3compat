#!/bin/bash
# donebar.sh WL N: javac or the Java benchmark (values against native), main's pin (base) against the branch's (fix),
# interleaved per iteration in four configurations: client (launcher), client-default (drrun -c, DR defaults),
# empty (empty client, launcher options), dr (DynamoRIO alone, launcher options).
set -u
WL=$1; N=$2
grep -m1 'model name' /proc/cpuinfo
pin() { sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' $1/cmake/FetchDynamoRIO.cmake; }
declare -A DR=([base]=$HOME/.cache/v3compat/dr-$(pin /tmp/base)/ [fix]=$HOME/.cache/v3compat/dr-$(pin .)/)
declare -A IN=([base]=/tmp/base/build/inst [fix]=$PWD/build/inst)
for v in base fix; do [ -x ${DR[$v]}bin64/drrun ] || { echo "RESULT no $v build"; exit 1; }
  cmake -S lab/nullclient -B /tmp/null-$v -DDynamoRIO_DIR=${DR[$v]}cmake >/dev/null && cmake --build /tmp/null-$v >/dev/null || exit 1; done
L="-no_enable_reset -no_ignore_syscalls"
cmd() { case $2 in client) echo "${IN[$1]}/bin/v3compat-run --";;
  client-default) echo "${DR[$1]}bin64/drrun -c ${IN[$1]}/libexec/v3compat/libv3compat.so --";;
  empty) echo "${DR[$1]}bin64/drrun $L -c /tmp/null-$1/libnull.so --";; dr) echo "${DR[$1]}bin64/drrun $L --";; esac; }
C="client client-default empty dr"
mkdir -p /tmp/bench/javac-out lab-out/fail
declare -A bad wrong
for v in base fix; do for c in $C; do bad[$v-$c]=0; wrong[$v-$c]=0; done; done
[ $WL = java ] && java -XX:UseAVX=2 -cp /tmp/bench/classes Bench 8 1 | grep -E '^(value|checksum)' >/tmp/native
for i in $(seq $N); do for c in $C; do for v in base fix; do
  P=$(cmd $v $c); k=$v-$c
  if [ $WL = javac ]; then timeout 1200 $P javac -J-XX:UseAVX=2 -d /tmp/bench/javac-out bench/Bench.java >/tmp/o 2>/tmp/e
  else timeout 1200 $P java -XX:UseAVX=2 -cp /tmp/bench/classes Bench 8 1 >/tmp/o 2>/tmp/e; fi
  rc=$?
  if [ $rc != 0 ]; then bad[$k]=$((bad[$k]+1)); cp /tmp/e lab-out/fail/$WL.$k.$i.err
    echo "FAIL $WL $k $i rc=$rc $(grep -m1 -oE 'crash at PC 0x[0-9a-f]+|Received SIG[A-Z]+' /tmp/e | tr '\n' ' ')"
  elif [ $WL = java ] && ! diff -q <(grep -E '^(value|checksum)' /tmp/o) /tmp/native >/dev/null; then wrong[$k]=$((wrong[$k]+1))
    diff <(grep -E '^(value|checksum)' /tmp/o) /tmp/native | grep '^<' | sed "s/^/WRONG $k $i /"; fi
done; done; done
for v in base fix; do for c in $C; do echo "RESULT $WL $v $c: ${bad[$v-$c]} of $N bad, ${wrong[$v-$c]} wrong"; done; done
