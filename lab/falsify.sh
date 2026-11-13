#!/bin/bash
# falsify.sh RUNS: trace_flush, base (origin/main's pin) against the fix, interleaved, on DynamoRIO alone and under the client.
set -u
grep -m1 'model name' /proc/cpuinfo
pin() { sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' $1/cmake/FetchDynamoRIO.cmake; }
declare -A DR=([base]=$HOME/.cache/v3compat/dr-$(pin /tmp/base)/ [fix]=$HOME/.cache/v3compat/dr-$(pin .)/)
declare -A CL=([base]=/tmp/base/build/inst/bin/v3compat-run [fix]=build/inst/bin/v3compat-run)
T=$PWD/build/test/trace_flush
declare -A bad
for k in dr-base dr-fix client-base client-fix; do bad[$k]=0; done
for i in $(seq $1); do for v in base fix; do
  timeout 120 ${DR[$v]}bin64/drrun -no_enable_reset -no_ignore_syscalls -- $T 2 >/tmp/o 2>/tmp/e; rc=$?
  [ $rc = 0 ] || { bad[dr-$v]=$((bad[dr-$v]+1)); echo "FAIL dr-$v $i rc=$rc $(head -c 200 /tmp/e | head -2 | tr '\n' ' ')"; }
  timeout 120 ${CL[$v]} -- $T 2 >/tmp/o 2>/tmp/e; rc=$?
  [ $rc = 0 ] || { bad[client-$v]=$((bad[client-$v]+1)); echo "FAIL client-$v $i rc=$rc $(head -c 200 /tmp/e | head -2 | tr '\n' ' ')"; }
done; done
for k in dr-base dr-fix client-base client-fix; do echo "RESULT trace_flush $k: ${bad[$k]} of $1 bad"; done
