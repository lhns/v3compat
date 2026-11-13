#!/bin/bash
# arrays.sh N: Bench's arrays kernel alone, 8 rounds, values against native; configurations interleaved.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
J="java -XX:UseAVX=2 -cp /tmp/bench/classes"
$J Bench 8 1 arrays | grep -E '^(value|parts)' >/tmp/native; sed 's/^/NATIVE /' /tmp/native
C="addv avx2v"
run() { case $1 in addv) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c $PWD/build/inst/libexec/v3compat/libv3compat.so -native=fma,bmi,lzcnt,tzcnt,movbe -verify -emu-only=vpaddd -- $J Bench 8 1 arrays sp;;
  avx2v) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c $PWD/build/inst/libexec/v3compat/libv3compat.so -native=fma,bmi,lzcnt,tzcnt,movbe -verify -- $J Bench 8 1 arrays sp;; esac; }
declare -A bad wrong; for c in $C; do bad[$c]=0; wrong[$c]=0; done
for i in $(seq $1); do for c in $C; do
  timeout 900 bash -c "$(declare -f run); J='$J'; DR='$DR'; run $c" >/tmp/o 2>/tmp/e; rc=$?
  v=$(grep -c V3VERIFY /tmp/e); echo "VERIFY $c $i rc=$rc mismatches=$v wrong=$(diff -q <(grep -E '^(value|parts)' /tmp/o) /tmp/native >/dev/null && echo 0 || echo 1)"
  if [ $rc != 0 ]; then bad[$c]=$((bad[$c]+1)); echo "FAIL $c $i rc=$rc $(grep -m1 -E 'crash at PC|SIG' /tmp/e)"
  elif ! diff -q <(grep -E '^(value|parts)' /tmp/o) /tmp/native >/dev/null; then wrong[$c]=$((wrong[$c]+1)); diff <(grep -E '^(value|parts)' /tmp/o) /tmp/native | grep '^<' | sed "s/^/WRONG $c $i /"; fi
done; done
for c in $C; do echo "RESULT arrays $c: ${bad[$c]} of $1 crashed, ${wrong[$c]} wrong"; done
