#!/bin/bash
# iso.sh SIDE RUNS: javac's crash under fork and upstream DynamoRIO, with and without a client and the launcher's options.
set -u
SIDE=$1; N=$2
grep -m1 'model name' /proc/cpuinfo
B=/tmp/bench; JAVAC="javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java"
L="-no_enable_reset -no_ignore_syscalls"   # what the launcher adds to -code_api
FORK=$(ls -d ~/.cache/v3compat/dr-$FORK_COMMIT/); UP=$(ls -d ~/.cache/v3compat/dr-$UP_COMMIT/)
RUN=$PWD/build/inst/bin/v3compat-run
declare -A C
if [ "$SIDE" = fork ]; then
  C=([dr-launcheropts]="${FORK}bin64/drrun $L --"
     [drnull-launcheropts]="${FORK}bin64/drrun -c /tmp/null-fork/libnull.so $L --"
     [drnull-default]="${FORK}bin64/drrun -c /tmp/null-fork/libnull.so --"
     [v3null]="env V3COMPAT_NULL=1 $RUN --"
     [v3null+enable_reset]="env V3COMPAT_NULL=1 V3COMPAT_DR_OPTS=-enable_reset $RUN --"
     [v3null+ignore_syscalls]="env V3COMPAT_NULL=1 V3COMPAT_DR_OPTS=-ignore_syscalls $RUN --"
     [v3client]="$RUN --")
else
  C=([dr-launcheropts]="${UP}bin64/drrun $L --"
     [drnull-launcheropts]="${UP}bin64/drrun -c /tmp/null-up/libnull.so $L --"
     [drnull-default]="${UP}bin64/drrun -c /tmp/null-up/libnull.so --")
fi
for i in $(seq "$N"); do for c in "${!C[@]}"; do
  s=$(date +%s.%N); timeout 1800 ${C[$c]} $JAVAC >/dev/null 2>/tmp/e; rc=$?; w=$(echo "$(date +%s.%N) - $s" | bc | cut -c1-6)
  where=""
  if [ $rc != 0 ]; then
    pc=$(grep -oE 'crash at PC 0x[0-9a-f]+' /tmp/e | head -1 | awk '{print $4}'); base=$(grep -oE '^Base: 0x[0-9a-f]+' /tmp/e | head -1 | awk '{print $2}')
    [ -n "$pc" ] && [ -n "$base" ] && where=$(printf 'off=0x%x' $((pc - base)))
    [ -z "$where" ] && where="msg=$(head -c 160 /tmp/e | tr '\n' ' ')"
  fi
  echo "RESULT $SIDE $c run=$i rc=$rc wall=$w $where"
done; done
