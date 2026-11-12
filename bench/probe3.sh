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
  # A backtrace at the faulting instruction: every SIGSEGV stops, and is passed on unless it is at one of the two PCs.
  cat > /tmp/gdb.py <<'G'
import gdb
dbg = __import__('os').environ['DBG']
gdb.execute("set pagination off")
gdb.execute("set follow-exec-mode same")
for s in ("SIGBUS", "SIGILL", "SIGUSR1", "SIGUSR2", "SIGCHLD", "SIGQUIT", "SIGPIPE"):
    gdb.execute("handle %s nostop noprint pass" % s)
gdb.execute("handle SIGSEGV stop noprint pass")
gdb.execute("run")
n = 0
while True:
    try:
        pc = int(gdb.parse_and_eval("$pc"))
    except gdb.error:
        print("EXITED after", n, "SIGSEGVs"); break
    if (pc & 0xfffff) in (0x9359d, 0x93799):
        m = [l for l in gdb.execute("info proc mappings", to_string=True).splitlines() if "libdynamorio.so" in l]
        base = int(m[0].split()[0], 16)
        print("CAUGHT pc", hex(pc), "DRBASE", hex(base), "after", n)
        gdb.execute("add-symbol-file %s -o %#x" % (dbg, base - 0x71000000))
        for c in ("bt 40", "info registers rip rax rbx rcx rdx rsi rdi r12 r13 r14 r15", "x/8i $pc-16", "thread apply all bt 8"):
            print("### " + c); gdb.execute(c)
        break
    n += 1
    gdb.execute("continue")
G
  for i in 1 2 3 4; do
    echo "== gdb run $i"
    DBG=$DBG timeout 1500 gdb -q -batch -x /tmp/gdb.py --args env V3COMPAT_NULL=1 $RUN -- $JAVAC 2>&1 |
      grep -vE '^\[(New|Thread|Detaching|LWP|Inferior)|^warning: |^Program received|^process ' | head -250
  done
  exit 0
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
