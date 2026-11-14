#!/bin/bash
# sigstart.sh N: thread_start_sig, main's pin (base) against the fix, interleaved, under the client and on DynamoRIO alone.
set -u
grep -m1 'model name' /proc/cpuinfo
pin() { sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' $1/cmake/FetchDynamoRIO.cmake; }
declare -A DR=([base]=$HOME/.cache/v3compat/dr-$(pin /tmp/base)/ [fix]=$HOME/.cache/v3compat/dr-$(pin .)/)
declare -A CL=([base]=/tmp/base/build/inst/bin/v3compat-run [fix]=build/inst/bin/v3compat-run)
T=$PWD/build/test/thread_start_sig
declare -A bad; for k in dr-base dr-fix client-base client-fix; do bad[$k]=0; done
for i in $(seq $1); do for v in base fix; do
  timeout 120 ${DR[$v]}bin64/drrun -no_enable_reset -no_ignore_syscalls -- $T 20 >/tmp/o 2>/tmp/e; rc=$?
  grep -q '^ok' /tmp/o && [ $rc = 0 ] || { bad[dr-$v]=$((bad[dr-$v]+1)); [ ${bad[dr-$v]} -le 3 ] && echo "FAIL dr-$v $i rc=$rc $(grep -v WARNING /tmp/e | head -c 200 | tr '\n' ' ')"; }
  timeout 120 ${CL[$v]} -- $T 20 >/tmp/o 2>/tmp/e; rc=$?
  grep -q '^ok' /tmp/o && [ $rc = 0 ] || { bad[client-$v]=$((bad[client-$v]+1)); [ ${bad[client-$v]} -le 3 ] && echo "FAIL client-$v $i rc=$rc $(head -c 200 /tmp/e | tr '\n' ' ')"; }
done; done
for k in dr-base dr-fix client-base client-fix; do echo "RESULT thread_start_sig $k: ${bad[$k]} of $1 bad"; done
