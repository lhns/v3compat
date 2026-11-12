#!/bin/bash
# java-rate.sh REPS: Java Bench survival, base against new, with and without HotSpot's FMA intrinsic
# (-XX:-UseFMA executes no FMA instruction, so both builds then run the same client paths).
set -u
REPS=${1:-10}
B=/tmp/bench
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
grep -m1 'model name' /proc/cpuinfo
for r in $(seq "$REPS"); do
  for c in base new; do
    for fma in fma nofma; do
      opt=-XX:+UseFMA; [ $fma = nofma ] && opt=-XX:-UseFMA
      ${RUN[$c]} -- java -XX:UseAVX=2 $opt -cp $B/classes Bench 8 1 >/tmp/j-out 2>/tmp/j-err; rc=$?
      pc=$(grep -o -m1 'crash at PC 0x[0-9a-f]*' /tmp/j-err | awk '{print $4}')
      base=$(grep -o -m1 'Base: 0x[0-9a-f]*' /tmp/j-err | awk '{print $2}')
      off=""; [ -n "$pc" ] && [ -n "$base" ] && off=$(printf '%#x' $((pc - base)))
      echo "RESULT java $c $fma rep=$r rc=$rc off=$off $(grep -E '^checksum' /tmp/j-out)"
      sig=$(grep -o -m1 'Received SIG[A-Za-z]*' /tmp/j-err | awk '{print $2}')
      [ $rc != 0 ] && echo "FAIL java $c $fma rep=$r rc=$rc sig=${sig:-none} off=${off:-none} $(grep -o -m1 'hs_err_pid[0-9]*.log\|# Problematic frame:' /tmp/j-err)"
      [ $rc != 0 ] && head -c 400 /tmp/j-err | head -4 | sed "s/^/STDERR $c $fma rep=$r /"
    done
  done
done
