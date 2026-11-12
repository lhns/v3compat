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
ph = subprocess.run(['readelf', '-lW', dbg], capture_output=True, text=True).stdout
base = min(int(l.split()[2], 16) for l in ph.splitlines() if l.strip().startswith('LOAD'))
print('link base', hex(base))
out = subprocess.run(['addr2line', '-f', '-i', '-C', '-e', dbg] + [hex(base + int(a, 16)) for a in sys.argv[2:]], capture_output=True, text=True).stdout
print(out)
PY
}
case $PART in
crash)
  readelf -lW "$LIB" | grep LOAD | head -3
  BASE=$(readelf -lW "$DBG" | awk '$1=="LOAD" {print $3; exit}')
  echo "SYM crash offsets:"; sym 0x93799 0x9359d
  objdump -d --no-show-raw-insn --start-address=$((BASE+0x93700)) --stop-address=$((BASE+0x937c0)) "$DBG" | tail -40
  objdump -d --no-show-raw-insn --start-address=$((BASE+0x93520)) --stop-address=$((BASE+0x935c0)) "$DBG" | tail -30
  # A backtrace: DR pauses at its crash report (-pause_on_error); gdb attaches, loads DR's symbols at its base,
  # puts the faulting registers from the report into the crashing thread and unwinds.
  for i in 1 2 3 4; do
    echo "== attach run $i"
    V3COMPAT_NULL=1 V3COMPAT_DR_OPTS="-pause_on_error" $RUN -- $JAVAC >/dev/null 2>/tmp/pe &
    for t in $(seq 600); do grep -q 'Received SIGSEGV' /tmp/pe 2>/dev/null && break; kill -0 $! 2>/dev/null || break; sleep 1; done
    sleep 2
    if ! grep -q 'Received SIGSEGV' /tmp/pe; then echo "no crash"; wait; continue; fi
    pid=$(grep -oE '\(([0-9]+)\)\.' /tmp/pe | head -1 | tr -dc 0-9)
    tid=$(grep -oE 'in thread [0-9]+' /tmp/pe | head -1 | awk '{print $3}')
    reg() { grep -oE "$1=0x[0-9a-f]+" /tmp/pe | head -1 | cut -d= -f2; }
    pc=$(grep -oE 'Received SIGSEGV at pc 0x[0-9a-f]+' /tmp/pe | awk '{print $5}')
    base=$(grep -m1 'libdynamorio.so' /proc/$pid/maps | cut -d- -f1)
    echo "pid=$pid tid=$tid pc=$pc base=0x$base"; head -c 1500 /tmp/pe
    gt=$(sudo gdb -q -batch -p $tid -ex 'info threads' 2>/dev/null | grep -m1 "LWP $tid" | grep -oE '^\*? *[0-9]+' | tr -dc 0-9)
    sudo gdb -q -batch -p $pid \
      -ex "add-symbol-file $DBG -o $(printf '%#x' $((0x$base - 0x71000000)))" \
      -ex "thread find LWP $tid" -ex "info threads" \
      -ex "python import re; t=[x for x in gdb.selected_inferior().threads() if x.ptid[1]==$tid][0]; t.switch()" \
      -ex "set \$rip=$pc" -ex "set \$rsp=$(reg esp)" -ex "set \$rbp=$(reg ebp)" -ex "set \$rax=$(reg eax)" -ex "set \$rbx=$(reg ebx)" \
      -ex "set \$rcx=$(reg ecx)" -ex "set \$rdx=$(reg edx)" -ex "set \$rsi=$(reg esi)" -ex "set \$rdi=$(reg edi)" \
      -ex "set \$r12=$(reg r12)" -ex "set \$r13=$(reg r13)" -ex "set \$r14=$(reg r14)" -ex "set \$r15=$(reg r15)" \
      -ex "bt 30" -ex "info locals" -ex "up" -ex "info locals" -ex "up" -ex "info locals" -ex "up" -ex "info locals" \
      2>&1 | grep -vE '^\[New LWP|^warning:' | head -150
    kill -9 $pid 2>/dev/null; wait
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
