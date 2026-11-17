#!/bin/bash
# vfxab.sh N: vfork_exit_dyn 20 under the client, base (/tmp/base, origin/main's pin) and the fix (this branch), interleaved,
# N runs each; then sigchld_exit (a lost SIGCHLD is a timeout) under both with vfork children. 7763 only.
# MODES="fork vfork" adds fork children, which fail alike on base and fix (docs/plan.md, Open items).
set -u
grep -m1 'model name' /proc/cpuinfo; grep -q 7763 /proc/cpuinfo || { echo 'RESULT skip: not 7763'; exit 0; }
declare -A R=([base]=/tmp/base/build/inst/bin/v3compat-run [fix]=build/inst/bin/v3compat-run) bad
T=build/test/vfork_exit_dyn; bad[base]=0; bad[fix]=0
for i in $(seq $1); do for a in base fix; do
  timeout 300 ${R[$a]} -- $T 20 > /tmp/o 2>/tmp/e; rc=$?
  if [ $rc != 0 ] || [ "$(tail -1 /tmp/o)" != PASS ]; then bad[$a]=$((bad[$a]+1)); echo "FAIL $a $i rc=$rc $(grep -m1 -ohE 'multiple native handlers|FAIL [^:]*' /tmp/e /tmp/o | head -1)"; fi
done; done
for a in base fix; do echo "RESULT vfork_exit_dyn $a: ${bad[$a]} of $1 bad"; done
gcc -O2 -pthread lab/sigchld_exit.c -o /tmp/sigchld_exit
declare -A sb; K=$(( $1 / 10 + 1 ))
for i in $(seq $K); do for m in ${MODES:-vfork}; do for a in base fix; do
  timeout 120 ${R[$a]} -- /tmp/sigchld_exit 300 $m > /tmp/o 2>/tmp/e; rc=$?
  if [ $rc != 0 ] || [ "$(tail -1 /tmp/o)" != PASS ]; then sb[$a$m]=$((${sb[$a$m]:-0}+1)); echo "FAIL sigchld $a $m $i rc=$rc $(head -c 300 /tmp/o /tmp/e | tr '\n' ' ')"; fi
done; done; done
for m in ${MODES:-vfork}; do for a in base fix; do echo "RESULT sigchld_exit $m $a: ${sb[$a$m]:-0} of $K bad"; done; done
