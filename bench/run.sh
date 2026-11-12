#!/bin/bash
# run.sh MODE REPS: the performance baseline on one runner. Needs: build/ (v3compat), ./duckdb, bench/ built
# into $B (fma, classes/). MODE: time | profile | all. Lines starting RESULT, PERF, STAT, OPS are the data.
set -u
MODE=${1:-all}; REPS=${2:-3}
B=${B:-/tmp/bench}; RUN=$PWD/build/inst/bin/v3compat-run
DRRUN=$(ls -d ~/.cache/v3compat/dr-*[0-9a-f]/ | head -1)bin64/drrun
SQL=$PWD/test/apps/duckdb-varied.sql
grep -m1 'model name' /proc/cpuinfo; nproc; node --version; java -version 2>&1 | head -1
JAVA_V2="-XX:UseAVX=2"   # native on an AVX-512 runner would otherwise run other code than under the client
# V8 and HotSpot flags that stop the JIT emitting v3 forms: under the client, what the JIT's own use costs.
NODE_NOV3=$(node --v8-options | grep -oE -- '--enable-(avx2|fma3|bmi1|bmi2|lzcnt|popcnt)' | sed 's/--enable-/--no-enable-/' | tr '\n' ' ')
JAVA_NOV3="-XX:UseAVX=1 -XX:-UseBMI1Instructions -XX:-UseBMI2Instructions -XX:-UseFMA -XX:-UseCountLeadingZerosInstruction -XX:-UseCountTrailingZerosInstruction"
echo "node no-v3 flags: $NODE_NOV3"

declare -A W=(
  [node]="node bench/jit.js 8 1"
  [node-nov3]="node $NODE_NOV3 bench/jit.js 8 1"
  [node-start]="node -e 0"
  [npm-version]="npm --version"
  [java]="java $JAVA_V2 -cp $B/classes Bench 8 1"
  [java-nov3]="java $JAVA_NOV3 -cp $B/classes Bench 8 1"
  [java-version]="java $JAVA_V2 -version"
  [javac]="javac -J$JAVA_V2 -d $B/javac-out bench/Bench.java"
  [fma]="$B/fma 1"
  [duckdb-compute]="./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'"
  [duckdb-varied]="./duckdb -csv -noheader -f $SQL"
)
ORDER="fma node node-nov3 node-start npm-version java java-nov3 java-version javac duckdb-compute duckdb-varied"
declare -A C=(
  [native]=""
  [dr]="$DRRUN -no_enable_reset -no_ignore_syscalls --"
  [dr-default]="$DRRUN --"
  [client]="$RUN --"
  [helpers]="env V3COMPAT_HELPERS_ONLY=1 $RUN --"
)
CFGS="native dr dr-default client helpers"
# A digest of the output without its timings: must equal native's.
digest() { grep -vE '^(round|total|micro) ' "$1" | sed -E 's/[0-9]+\.[0-9]+ (s|ms)//g' | sha256sum | cut -c1-12; }

one() { # label cfg workload
  local out=/tmp/bench-out err=/tmp/bench-err
  eval "/usr/bin/time -f '%e %U %S %M' -o /tmp/bench-time timeout 1800 ${C[$2]} ${W[$3]}" >$out 2>$err; local rc=$?
  read -r wall user sys rss </tmp/bench-time 2>/dev/null || { wall=NA; user=NA; sys=NA; rss=NA; }
  echo "RESULT $3 $2 $1 rc=$rc wall=$wall user=$user sys=$sys maxrss_kb=$rss digest=$(digest $out)"
  grep -E '^(round|micro|kernel|total) ' $out | sed "s/^/DETAIL $3 $2 $1 /"
  [ $rc = 0 ] || { echo "STDERR $3 $2 $1:"; tail -5 $err; }
}

if [ "$MODE" = profile ] || [ "$MODE" = all ]; then
  # Executed v3 forms and DR's block counts (lab client prints dr_get_stats with -stats); not timed.
  for w in $ORDER; do
    eval "V3COMPAT_STATS=1 timeout 3600 $RUN -- ${W[$w]}" >/dev/null 2>/tmp/stats-err
    grep -E 'v3compat: (stats|op|drstats)' /tmp/stats-err | sed "s/^/OPS $w /"
    eval "V3COMPAT_AUDIT=1 timeout 1800 $RUN -- ${W[$w]}" >/dev/null 2>/tmp/audit-err
    grep 'v3compat: audit' /tmp/audit-err | sed 's/ at 0x.*//' | sort | uniq -c | sed "s/^/AUDIT $w /"
  done
  # Where the time goes: perf by DSO and symbol; kernel-side counts of faults, signals, mprotect.
  for w in node java javac fma duckdb-compute; do for c in native dr client; do
    [ $c = native ] || eval "perf record -q -e cpu-clock -F 1999 -o /tmp/perf.data -- ${C[$c]} ${W[$w]}" >/dev/null 2>&1
    [ $c = native ] || perf report -i /tmp/perf.data --sort dso --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -12 | sed "s/^/PERF $w $c dso /"
    [ $c = native ] || perf report -i /tmp/perf.data --sort dso,sym --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -40 | sed "s/^/PERF $w $c sym /"
    eval "perf stat -x, -e task-clock,page-faults,context-switches,signal:signal_deliver,syscalls:sys_enter_mprotect,syscalls:sys_enter_munmap,syscalls:sys_enter_futex -- ${C[$c]} ${W[$w]}" >/dev/null 2>/tmp/pstat
    grep -vE '^#|^$' /tmp/pstat | sed "s/^/STAT $w $c /"
  done; done
fi

if [ "$MODE" = time ] || [ "$MODE" = all ]; then
  for r in $(seq "$REPS"); do for w in $ORDER; do for c in $CFGS; do
    case "$w:$c" in *-nov3:native|*-nov3:dr|*-nov3:dr-default|*-nov3:helpers) continue ;; esac
    one "$r" "$c" "$w"
  done; done; done
fi
