#!/bin/bash
# arrays.sh N: Bench's arrays kernel alone, 8 rounds, values against native; configurations interleaved.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
J="java -XX:UseAVX=2 -cp /tmp/bench/classes"
$J Bench 8 1 arrays | grep -E '^(value|parts)' >/tmp/native; sed 's/^/NATIVE /' /tmp/native
C="csp hsp"
run() { case $1 in client) build/inst/bin/v3compat-run -- $J Bench 8 1 arrays;;
  helpers) V3COMPAT_HELPERS_ONLY=1 build/inst/bin/v3compat-run -- $J Bench 8 1 arrays;;
  cdsp) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c $PWD/build/inst/libexec/v3compat/libv3compat.so -- $J Bench 8 1 arrays sp;;
  cxl) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c $PWD/build/inst/libexec/v3compat/libv3compat.so -store-xl -- $J Bench 8 1 arrays sp;;
  exl) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c /tmp/null-b/libnull.so -s -- $J Bench 8 1 arrays sp;;
  csp) build/inst/bin/v3compat-run -- $J Bench 8 1 arrays sp;;
  hsp) V3COMPAT_HELPERS_ONLY=1 build/inst/bin/v3compat-run -- $J Bench 8 1 arrays sp;;
  esp) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c /tmp/null-b/libnull.so -- $J Bench 8 1 arrays sp;;
  dsp) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -- $J Bench 8 1 arrays sp;;
  hXbatch) V3COMPAT_HELPERS_ONLY=1 build/inst/bin/v3compat-run -- $J -Xbatch Bench 8 1 arrays;;
  empty) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c /tmp/null-b/libnull.so -- $J Bench 8 1 arrays;; esac; }
declare -A bad wrong; for c in $C; do bad[$c]=0; wrong[$c]=0; done
for i in $(seq $1); do for c in $C; do
  timeout 900 bash -c "$(declare -f run); J='$J'; DR='$DR'; run $c" >/tmp/o 2>/tmp/e; rc=$?
  grep -m40 '^V3R' /tmp/e | sed "s/^/$c $i /" >> lab-out/v3r.txt; echo "V3RCOUNT $c $i $(grep -c '^V3R' /tmp/e)"
  if [ $rc != 0 ]; then bad[$c]=$((bad[$c]+1)); echo "FAIL $c $i rc=$rc $(grep -m1 -E 'crash at PC|SIG' /tmp/e)"
  elif ! diff -q <(grep -E '^(value|parts)' /tmp/o) /tmp/native >/dev/null; then wrong[$c]=$((wrong[$c]+1)); diff <(grep -E '^(value|parts)' /tmp/o) /tmp/native | grep '^<' | sed "s/^/WRONG $c $i /"; fi
done; done
for c in $C; do echo "RESULT arrays $c: ${bad[$c]} of $1 crashed, ${wrong[$c]} wrong"; done
