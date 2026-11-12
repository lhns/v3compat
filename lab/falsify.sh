#!/bin/bash
# falsify.sh: trace_flush on DynamoRIO alone, the diagnostic pin against the same build without the FAKE_TAG store,
# interleaved; then default options many times; then javac on the diagnostic pin.
set -u
grep -m1 'model name' /proc/cpuinfo
A=$HOME/.cache/v3compat/dr-335d2f7b641e7a93ba4f1af1ddc87e1d6da8f80f/
B=$HOME/.cache/v3compat/dr-f24b67026c51e1b990e8efba22fa6e8716d0c851/
ls -d $A $B || exit 1
T=$PWD/build/test/trace_flush
declare -A bad=([pin]=0 [nofake]=0) forge=([pin]=0 [nofake]=0)
for i in $(seq 40); do for c in pin nofake; do
  D=$A; [ $c = nofake ] && D=$B
  timeout 120 ${D}bin64/drrun -no_enable_reset -no_ignore_syscalls -- $T 2 >/tmp/o 2>/tmp/e; rc=$?
  [ $rc = 0 ] || bad[$c]=$((bad[$c]+1))
  grep -q 'V3DIAG forge' /tmp/e && forge[$c]=$((forge[$c]+1))
  echo "RUN $c $i rc=$rc $(head -1 /tmp/o)"
done; done
for c in pin nofake; do echo "RESULT reset-off $c: ${bad[$c]} of 40 bad, ${forge[$c]} with a forge"; done
# Default options (reset on): the rate, and how many writes a run makes.
b=0; f=0
for i in $(seq 150); do
  timeout 120 ${A}bin64/drrun -- $T 2 >/tmp/o 2>/tmp/e; rc=$?
  [ $rc = 0 ] || b=$((b+1)); grep -q 'V3DIAG forge' /tmp/e && f=$((f+1))
  [ $((i % 15)) = 0 ] && echo "RUN default $i rc=$rc $(head -1 /tmp/o)"
done
echo "RESULT default options pin: $b of 150 bad, $f with a forge"
# javac on DynamoRIO alone, launcher options, diagnostic pin: which failures are a forge at -1.
mkdir -p /tmp/bench/javac-out
for i in $(seq 30); do
  timeout 1200 ${A}bin64/drrun -no_enable_reset -no_ignore_syscalls -- javac -J-XX:UseAVX=2 -d /tmp/bench/javac-out bench/Bench.java >/dev/null 2>/tmp/e; rc=$?
  [ $rc = 0 ] && continue
  fg=$(grep -c 'V3DIAG forge' /tmp/e); f1=$(grep -m1 'V3DIAG forge' /tmp/e | grep -oE 'target=0x[0-9a-f]+ type=[0-9]+ ra0=0x[0-9a-f]+')
  cr=$(grep -m1 -oE 'crash at PC 0x[0-9a-f]+|Received SIG[A-Za-z]+ at[a-z ]* pc 0x[0-9a-f]+' /tmp/e); base=$(grep -m1 -oE '^Base: 0x[0-9a-f]+' /tmp/e)
  echo "JAVAC $i rc=$rc forges=$fg first=[$f1] crash=[$cr] $base"
  mkdir -p lab-out/javac; cp /tmp/e lab-out/javac/$i.err
done
echo "RESULT javac done"
