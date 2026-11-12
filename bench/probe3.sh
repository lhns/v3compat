#!/bin/bash
# probe3.sh PART: the Java crash (where, which options avoid it) and who reads /proc/self/maps.
set -u
PART=$1
B=/tmp/bench; RUN=$PWD/build/inst/bin/v3compat-run
DRROOT=$(ls -d ~/.cache/v3compat/dr-*[0-9a-f]/ | head -1); DRRUN=${DRROOT}bin64/drrun
LIB=${DRROOT}lib64/release/libdynamorio.so; DBG=$LIB.debug
grep -m1 'model name' /proc/cpuinfo
JAVAC="javac -J-XX:UseAVX=2 -d $B/javac-out bench/Bench.java"
perf buildid-cache -a "$DBG" 2>&1 | head -2; perf buildid-cache -a "$LIB" 2>&1 | head -2
# Link-time address of the text: perf and crash offsets are from the load base.
sym() { python3 - "$DBG" "$@" <<'PY'
import subprocess, sys
dbg = sys.argv[1]
out = subprocess.run(['addr2line', '-f', '-i', '-C', '-e', dbg] + [hex(int(a, 16)) for a in sys.argv[2:]], capture_output=True, text=True).stdout
print(out)
PY
}
case $PART in
crash)
  readelf -lW "$LIB" | grep LOAD | head -3
  echo "SYM crash offsets:"; sym 0x93799 0x9359d
  objdump -d --no-show-raw-insn --start-address=0x93700 --stop-address=0x937c0 "$DBG" | tail -40
  objdump -d --no-show-raw-insn --start-address=0x93520 --stop-address=0x935c0 "$DBG" | tail -30
  # A backtrace at DR's crash report, under gdb (the app's and DR's own SIGSEGVs pass through).
  cat > /tmp/gdbcmds <<'G'
set pagination off
set breakpoint pending on
set follow-exec-mode same
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGILL nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGCHLD nostop noprint pass
handle SIGQUIT nostop noprint pass
handle SIG62 nostop noprint pass
handle SIG63 nostop noprint pass
handle SIG64 nostop noprint pass
break report_dynamorio_problem
run
bt 30
frame 0
info registers
thread apply all bt 8
G
  for i in 1 2 3; do
    echo "== gdb run $i"
    timeout 600 gdb -q -batch -x /tmp/gdbcmds --args env V3COMPAT_NULL=1 $RUN -- $JAVAC 2>&1 | grep -vE '^\[(New|Thread|Detaching)|^warning: ' | head -150
  done
  # Which DR options avoid it: javac under the null client and the client, 6 runs each.
  for o in "" "-sandbox2ro_threshold 0" "-thread_private" "-ro2sandbox_threshold 0 -sandbox2ro_threshold 0"; do
    for c in null client; do
      ok=0; walls=""
      for i in 1 2 3 4 5 6; do
        s=$(date +%s.%N)
        if [ $c = null ]; then V3COMPAT_NULL=1 V3COMPAT_DR_OPTS="$o" timeout 1800 $RUN -- $JAVAC >/dev/null 2>/tmp/e; else V3COMPAT_DR_OPTS="$o" timeout 1800 $RUN -- $JAVAC >/dev/null 2>/tmp/e; fi
        rc=$?; [ $rc = 0 ] && ok=$((ok+1)); walls="$walls $(echo "$(date +%s.%N) - $s" | bc | cut -c1-6):$rc"
      done
      echo "RESULT javac $c [$o] ok $ok of 6:$walls"
    done
  done ;;
maps)
  # Callers of read() (the /proc/self/maps scans) and a symbolized flat profile.
  for w in "java -XX:UseAVX=2 -version" "$JAVAC" "node bench/jit.js 8 1"; do for c in dr client; do
    [ $c = dr ] && P="$DRRUN -no_enable_reset -no_ignore_syscalls --" || P="$RUN --"
    echo "== $c: $w"
    sudo -E perf record -q -e syscalls:sys_enter_read -g --call-graph dwarf,16384 -o /tmp/r.data -- sudo -E -u "$USER" env PATH="$PATH" $P $w >/dev/null 2>&1
    sudo perf buildid-cache -a "$DBG" >/dev/null 2>&1
    sudo perf report -i /tmp/r.data --stdio --no-children -G --percent-limit 3 2>/dev/null | grep -vE '^$|^#' | head -70 | sed "s/^/READS /"
    perf record -q -e cpu-clock -F 1999 -g --call-graph dwarf,16384 -o /tmp/c.data -- $P $w >/dev/null 2>&1
    perf report -i /tmp/c.data --stdio --no-children --sort dso,sym --percent-limit 0.7 -g none 2>/dev/null | grep -E '^ +[0-9]' | head -50 | sed "s/^/FLAT /"
    perf report -i /tmp/c.data --stdio --children --sort sym --percent-limit 4 -g none 2>/dev/null | grep -E '^ +[0-9]' | head -50 | sed "s/^/INCL /"
  done; done ;;
esac
