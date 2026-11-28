#!/bin/bash
# regab.sh SCALE: base (/tmp/base, origin/main's pin) against this branch's pin, interleaved run by run, on the exit,
# vfork and stress tests a change to DR's exit can break. A run is bad if its status or output is not the expected
# one; rc=137 is a hang (killed at the timeout). Per test and arm: RESULT name arm: bad of N (hang H; codes).
grep -m1 "model name" /proc/cpuinfo
S=${1:-1}
declare -A RUN=([base]="/tmp/base/build/inst/bin/v3compat-run" [fix]="build/inst/bin/v3compat-run")
declare -A bad hang runs codes
T=build/test
# ab NAME N TIMEOUT WANT CMD...: CMD with @R for the runner; WANT is rc=N, or out=STR for the last stdout line.
ab() {
  name=$1 n=$2 to=$3 want=$4; shift 4
  for i in $(seq $n); do for a in base fix; do
    cmd=("${@//@R/${RUN[$a]}}")
    timeout -s KILL $to "${cmd[@]}" >/tmp/ra.out 2>/tmp/ra.err </dev/null; rc=$?
    k=$name.$a; runs[$k]=$(( ${runs[$k]:-0} + 1 ))
    ok=0
    case $want in rc=*) [ "rc=$rc" = "$want" ] && ok=1;; out=*) [ $rc = 0 ] && [ "$(tail -1 /tmp/ra.out)" = "${want#out=}" ] && ok=1;; esac
    if [ $ok = 0 ]; then
      bad[$k]=$(( ${bad[$k]:-0} + 1 )); codes[$k]="${codes[$k]} $rc"
      [ $rc = 137 ] && hang[$k]=$(( ${hang[$k]:-0} + 1 ))
      echo "$name $a run $i: rc=$rc $(tail -1 /tmp/ra.out | cut -c1-80) $(head -c 200 /tmp/ra.err | tr '\n' ' ')"
    fi
  done; done
}
gcc -O2 -pthread lab/te_sig.c -o /tmp/te_sig || exit 1
mkdir -p /tmp/bench/javac-out
JOPT="-XX:UseAVX=2"
for r in 1 2; do
  ab te64 $((40 * S)) 60 rc=3 @R -- $T/thread_exit 64 300
  ab te1k $((5 * S)) 120 rc=3 @R -- $T/thread_exit 1000 2000
  ab tenproc $((10 * S)) 60 rc=3 @R -- $T/thread_exit 64 300 nproc
  ab tevfork $((10 * S)) 60 rc=3 @R -- $T/thread_exit 64 300 vfork
  ab vfexit $((10 * S)) 300 out=PASS @R -- $T/vfork_exit_dyn 20
  ab vfexitload $((1 * S)) 200 rc=0 sh test/vfork_exit_load.sh @R $T/vfork_exit_dyn 60
  ab vfkill $((5 * S)) 120 rc=0 sh test/vfork_kill.sh @R $T/vfork_kill_dyn 50
  ab vfsig $((10 * S)) 60 out=PASS @R -- $T/vfork_sig_dyn
  ab vfsigterm $((10 * S)) 60 rc=143 @R -- $T/vfork_sig_dyn term
  ab sigchld $((10 * S)) 120 out=PASS @R -- $T/sigchld_exit 300 vfork
  ab regsig $((15 * S)) 60 rc=0 @R -- $T/regstress 8 3 sig jit
  ab regjit $((5 * S)) 60 rc=0 @R -- $T/regstress 8 3 jit
  ab regidle $((5 * S)) 60 rc=0 @R -- $T/regstress 8 3 idle jit
  ab short $((30 * S)) 60 rc=0 @R -- $T/regstress 8 1 sig jit
  ab xlsig $((5 * S)) 60 rc=0 @R -- $T/regstress 8 3 xlsig
  ab segvpre $((10 * S)) 60 rc=139 @R -- /tmp/te_sig 64 300 pre segv
  ab segvpost $((10 * S)) 60 rc=3 @R -- /tmp/te_sig 64 300 post segv
  ab abrtpost $((10 * S)) 60 rc=3 @R -- /tmp/te_sig 64 300 post abrt
  ab java $((2 * S)) 900 rc=0 @R -- java $JOPT -cp /tmp/bench/classes Bench 8 1
  ab javac $((2 * S)) 1200 rc=0 @R -- javac -J$JOPT -d /tmp/bench/javac-out bench/Bench.java
done
for k in $(echo "${!runs[@]}" | tr ' ' '\n' | sort); do
  echo "RESULT ${k%.*} ${k##*.}: ${bad[$k]:-0} of ${runs[$k]} bad (hang ${hang[$k]:-0}; codes:${codes[$k]})"
done
