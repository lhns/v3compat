#!/bin/bash
# emit.sh N: the stale-trace fix against its base (the IBL fix alone), interleaved: trace_flush on DynamoRIO alone with
# -lazy_deletion_max_pending 1 (every trace emission walks the deletion list) and with defaults; then javac and the Java
# benchmark (values against native) under the client with that option.
set -u
grep -m1 'model name' /proc/cpuinfo
BASEC=cff4839b5084695e130a0a7706f1ca4065b35633
rm -rf /tmp/bt && mkdir /tmp/bt && git archive HEAD | tar -x -C /tmp/bt && sed -i "s/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT $BASEC)/" /tmp/bt/cmake/FetchDynamoRIO.cmake
(cmake -S /tmp/bt -B /tmp/bt/build >/dev/null && cmake --build /tmp/bt/build -j >/dev/null) || { echo "RESULT base build failed"; exit 1; }
pin() { sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' $1/cmake/FetchDynamoRIO.cmake; }
declare -A DR=([base]=$HOME/.cache/v3compat/dr-$BASEC/ [fix]=$HOME/.cache/v3compat/dr-$(pin .)/)
declare -A CL=([base]=/tmp/bt/build/inst/bin/v3compat-run [fix]=build/inst/bin/v3compat-run)
T=$PWD/build/test/trace_flush
declare -A bad; for k in base fix; do for m in lazy1 default; do bad[$k-$m]=0; done; done
mkdir -p lab-out/fail /tmp/bench/javac-out
for i in $(seq $1); do for v in base fix; do for m in lazy1 default; do
  O="-no_enable_reset -no_ignore_syscalls"; [ $m = lazy1 ] && O="$O -lazy_deletion_max_pending 1"
  timeout 120 ${DR[$v]}bin64/drrun $O -- $T 2 >/tmp/o 2>/tmp/e; rc=$?
  [ $rc = 0 ] || { bad[$v-$m]=$((bad[$v-$m]+1)); echo "FAIL tf $v-$m $i rc=$rc $(head -c 160 /tmp/e | head -1)"; }
done; done; done
for k in base fix; do for m in lazy1 default; do echo "RESULT trace_flush $k $m: ${bad[$k-$m]} of $1 bad"; done; done
java -XX:UseAVX=2 -cp /tmp/bench/classes Bench 8 1 | grep -E '^(value|checksum)' >/tmp/native
declare -A jb jw; for v in base fix; do jb[$v]=0; jw[$v]=0; done
for i in $(seq 4); do for v in base fix; do
  V3COMPAT_DR_OPTS="-lazy_deletion_max_pending 1" timeout 1200 ${CL[$v]} -- java -XX:UseAVX=2 -cp /tmp/bench/classes Bench 8 1 >/tmp/o 2>/tmp/e; rc=$?
  if [ $rc != 0 ]; then jb[$v]=$((jb[$v]+1)); echo "FAIL java $v $i rc=$rc $(grep -m1 -E 'crash at PC|SIG' /tmp/e)"; cp /tmp/e lab-out/fail/java.$v.$i.err
  elif ! diff -q <(grep -E '^(value|checksum)' /tmp/o) /tmp/native >/dev/null; then jw[$v]=$((jw[$v]+1)); diff <(grep -E '^(value|checksum)' /tmp/o) /tmp/native | grep '^<' | sed "s/^/WRONG java $v $i /"; fi
done; done
for v in base fix; do echo "RESULT java client lazy1 $v: ${jb[$v]} of 4 crashed, ${jw[$v]} wrong"; done
declare -A cb; for v in base fix; do cb[$v]=0; done
for i in $(seq 3); do for v in base fix; do
  V3COMPAT_DR_OPTS="-lazy_deletion_max_pending 1" timeout 1200 ${CL[$v]} -- javac -J-XX:UseAVX=2 -d /tmp/bench/javac-out bench/Bench.java >/tmp/o 2>/tmp/e; rc=$?
  [ $rc = 0 ] || { cb[$v]=$((cb[$v]+1)); echo "FAIL javac $v $i rc=$rc $(grep -m1 -E 'crash at PC|SIG' /tmp/e)"; }
done; done
for v in base fix; do echo "RESULT javac client lazy1 $v: ${cb[$v]} of 3 crashed"; done
