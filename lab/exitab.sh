#!/bin/bash
# exitab.sh N: vfork_exit_dyn 20 under the client, main's pin (base) against the fix, interleaved.
set -u
grep -m1 'model name' /proc/cpuinfo
declare -A CL=([base]=/tmp/base/build/inst/bin/v3compat-run [fix]=build/inst/bin/v3compat-run)
declare -A B=([base]=/tmp/base/build/test/vfork_exit_dyn [fix]=build/test/vfork_exit_dyn)
declare -A bad=([base]=0 [fix]=0)
for i in $(seq $1); do for v in base fix; do
  timeout 300 ${CL[$v]} -- ${B[$v]} 20 >/tmp/o 2>/tmp/e; rc=$?
  if [ $rc != 0 ] || [ "$(tail -1 /tmp/o)" != PASS ]; then bad[$v]=$((bad[$v]+1)); echo "FAIL $v $i rc=$rc $(head -c 200 /tmp/e)"; fi
done; done
for v in base fix; do echo "RESULT vfork_exit $v: ${bad[$v]} of $1 bad"; done
