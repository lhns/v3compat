#!/bin/bash
# sigymm.sh REPS SECS: test/sigymm in each mode natively, on DynamoRIO alone, an empty client, the client and helpers only.
set -u
grep -m1 'model name' /proc/cpuinfo
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
[ -x ${DR}bin64/drrun ] || { echo "RESULT no DynamoRIO build"; exit 1; }
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
T=$PWD/build/test/sigymm
[ -x $T ] || { echo "RESULT no sigymm build"; exit 1; }
L="-no_enable_reset -no_ignore_syscalls"
declare -A bad
C="native dr empty client helpers"
for c in $C; do for m in a h; do bad[$c-$m]=0; done; done
for i in $(seq $1); do for m in a h; do for c in $C; do
  case $c in native) P="";; dr) P="${DR}bin64/drrun $L --";; empty) P="${DR}bin64/drrun $L -c /tmp/null-b/libnull.so --";;
    client) P="build/inst/bin/v3compat-run --";; helpers) P="env V3COMPAT_HELPERS_ONLY=1 build/inst/bin/v3compat-run --";; esac
  timeout 120 $P $T $m $2 >/tmp/o 2>/tmp/e; rc=$?
  if [ $rc != 0 ]; then bad[$c-$m]=$((bad[$c-$m]+1)); echo "FAIL $c $m $i rc=$rc $(head -1 /tmp/o) $(grep -v WARNING /tmp/e | head -c 900 | tr '\n' ' ')"; fi
  [ $i = 1 ] && echo "SAMPLE $c $m $(head -1 /tmp/o)"
done; done; done
for c in $C; do for m in a h; do echo "RESULT sigymm $c $m: ${bad[$c-$m]} of $1 bad"; done; done
