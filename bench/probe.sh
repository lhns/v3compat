#!/bin/bash
# probe.sh PART: lab 2 of the performance baseline. PART: crash | sym | opts | pr22.
set -u
PART=$1
B=${B:-/tmp/bench}; RUN=$PWD/build/inst/bin/v3compat-run; RUN22=${RUN22:-/tmp/pr22/build/inst/bin/v3compat-run}
DRROOT=$(ls -d ~/.cache/v3compat/dr-*[0-9a-f]/ | head -1); DRRUN=${DRROOT}bin64/drrun
grep -m1 'model name' /proc/cpuinfo; nproc
JAVA_V2="-XX:UseAVX=2"
declare -A W=(
  [node]="node bench/jit.js 8 1"
  [node-start]="node -e 0"
  [npm-version]="npm --version"
  [java]="java $JAVA_V2 -cp $B/classes Bench 8 1"
  [java-version]="java $JAVA_V2 -version"
  [javac]="javac -J$JAVA_V2 -d $B/javac-out bench/Bench.java"
  [fma-micro]="$B/fma 1 micro"
  [duckdb-compute]="./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'"
  [duckdb-varied]="./duckdb -csv -noheader -f $PWD/test/apps/duckdb-varied.sql"
)
# npm is a script: DR alone must be given node itself.
declare -A C=(
  [native]=""
  [dr]="$DRRUN -no_enable_reset -no_ignore_syscalls --"
  [null]="env V3COMPAT_NULL=1 $RUN --"
  [client]="$RUN --"
  [pr22]="$RUN22 --"
)
wl() { local w=${W[$1]}; [ "$1" = npm-version ] && [ "$2" = dr ] && w="node $(dirname "$(readlink -f "$(command -v npm)")")/npm-cli.js --version"; echo "$w"; }
digest() { grep -vE '^(round|total|micro) ' "$1" | sed -E 's/[0-9]+\.[0-9]+ (s|ms)//g' | sha256sum | cut -c1-12; }
one() { # label cfg workload [extra env]
  local out=/tmp/p-out err=/tmp/p-err
  eval "${4:-} /usr/bin/time -f '%e %U %S %M' -o /tmp/p-time timeout 1800 ${C[$2]} $(wl "$3" "$2")" >$out 2>$err; local rc=$?
  read -r wall user sys rss < <(tail -1 /tmp/p-time)
  echo "RESULT $3 $2 $1 rc=$rc wall=$wall user=$user sys=$sys maxrss_kb=$rss digest=$(digest $out)"
  grep -E '^(round|micro|kernel|total) ' $out | sed "s/^/DETAIL $3 $2 $1 /"
  if [ $rc != 0 ]; then echo "STDERR-HEAD $3 $2 $1:"; head -c 3000 $err | head -40; fi
}

case $PART in
crash)
  # Java under the client fails often: which way, and is it the out-of-memory abort of ADR 0010?
  ls "$DRROOT"lib64/release/
  for i in 1 2 3 4 5 6; do
    one "$i" client java; one "$i" client javac; one "$i" dr javac; one "$i" null javac
  done
  for w in java javac; do
    eval "V3COMPAT_STATS=1 timeout 1800 $RUN -- $(wl $w client)" >/dev/null 2>/tmp/s-err; echo "STATS-RC $w $?"
    grep -E 'v3compat: (stats|drstats)' /tmp/s-err | sed "s/^/OPS $w /"; head -c 2000 /tmp/s-err | grep -v 'v3compat: op' | head -20
  done ;;
sym)
  # Flat profile with DR's symbols (addresses map through the split .debug file), and kernel-side counts.
  DBG=$(ls "$DRROOT"lib64/release/libdynamorio.so.debug 2>/dev/null); echo "debug file: ${DBG:-none}"
  for r in 1 2 3; do for w in node node-start npm-version java-version javac; do for c in native dr null client; do one "$r" "$c" "$w"; done; done; done
  for w in node java javac java-version; do for c in dr null client; do
    eval "perf record -q -e cpu-clock -F 1999 -o /tmp/perf.data -- ${C[$c]} $(wl $w $c)" >/dev/null 2>&1
    perf report -i /tmp/perf.data --sort dso --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -8 | sed "s/^/PERF $w $c dso /"
    perf report -i /tmp/perf.data --sort dso,sym --stdio 2>/dev/null | grep -E '^ +[0-9]' | head -60 | sed "s/^/PERF $w $c sym /"
    if [ -n "$DBG" ]; then
      perf report -i /tmp/perf.data --dsos libdynamorio.so --sort sym --stdio 2>/dev/null | grep -E '^ +[0-9]' |
        awk '{print $1, $NF}' > /tmp/dr-addr
      awk '{print $2}' /tmp/dr-addr | grep -E '^0x' | addr2line -f -e "$DBG" | awk 'NR%2==1' > /tmp/dr-fn
      paste -d' ' /tmp/dr-addr /tmp/dr-fn | awk '{gsub("%","",$1); s[$3]+=$1} END {for (f in s) printf "%7.2f%% %s\n", s[f], f}' |
        sort -rn | head -40 | sed "s/^/DRSYM $w $c /"
    fi
    perf stat -x, -e task-clock,page-faults,context-switches -- bash -c "${C[$c]} $(wl $w $c) >/dev/null 2>&1" 2>&1 >/dev/null | sed "s/^/STAT $w $c /"
    sudo -E perf stat -x, -e 'signal:signal_deliver,syscalls:sys_enter_mprotect,syscalls:sys_enter_munmap,syscalls:sys_enter_openat,syscalls:sys_enter_read' \
      -- sudo -E -u "$USER" env PATH="$PATH" bash -c "${C[$c]} $(wl $w $c) >/dev/null 2>&1" 2>&1 >/dev/null | sed "s/^/STAT $w $c /"
  done; done ;;
opts)
  # DR options that bear on JIT code, under the client.
  for r in 1 2; do for w in node javac java-version npm-version; do
    for o in "" "-no_enable_traces" "-ro2sandbox_threshold 0" "-sandbox2ro_threshold 0" "-thread_private"; do
      one "$r" client "$w" "V3COMPAT_DR_OPTS='$o'" | sed "s/^RESULT $w client/RESULT $w client[${o// /_}]/"
    done
  done; done ;;
pr22)
  # PR #22 rebased against main, interleaved on this runner.
  for r in 1 2 3; do for w in java node javac java-version fma-micro duckdb-compute duckdb-varied; do for c in native client pr22; do
    [ "$c" = native ] && [ "$r" != 1 ] && continue
    one "$r" "$c" "$w"
  done; done; done ;;
esac
