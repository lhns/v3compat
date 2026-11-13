#!/bin/bash
# arrays.sh N: Bench's arrays kernel alone, 8 rounds, values against native; configurations interleaved.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
J="java -XX:UseAVX=2 -cp /tmp/bench/classes"
$J Bench 8 1 arrays | grep '^value' >/tmp/native; sed 's/^/NATIVE /' /tmp/native
C="client helpers noSW noFMA empty"
run() { case $1 in client) build/inst/bin/v3compat-run -- $J Bench 8 1 arrays;;
  helpers) V3COMPAT_HELPERS_ONLY=1 build/inst/bin/v3compat-run -- $J Bench 8 1 arrays;;
  noSW) build/inst/bin/v3compat-run -- $J -XX:-UseSuperWord Bench 8 1 arrays;;
  noFMA) build/inst/bin/v3compat-run -- $J -XX:-UseFMA Bench 8 1 arrays;;
  empty) ${DR}bin64/drrun -no_enable_reset -no_ignore_syscalls -c /tmp/null-b/libnull.so -- $J Bench 8 1 arrays;; esac; }
declare -A bad wrong; for c in $C; do bad[$c]=0; wrong[$c]=0; done
for i in $(seq $1); do for c in $C; do
  timeout 600 bash -c "$(declare -f run); J='$J'; DR='$DR'; run $c" >/tmp/o 2>/tmp/e; rc=$?
  if [ $rc != 0 ]; then bad[$c]=$((bad[$c]+1)); echo "FAIL $c $i rc=$rc $(grep -m1 -E 'crash at PC|SIG' /tmp/e)"
  elif ! diff -q <(grep '^value' /tmp/o) /tmp/native >/dev/null; then wrong[$c]=$((wrong[$c]+1)); echo "WRONG $c $i $(grep '^value' /tmp/o)"; fi
done; done
for c in $C; do echo "RESULT arrays $c: ${bad[$c]} of $1 crashed, ${wrong[$c]} wrong"; done
