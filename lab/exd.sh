#!/bin/bash
# exd.sh sig ROUNDS N | k128 RUNS: the exit-time SIGSEGV (lab-exit-*).
# sig: te_sig under native and the client, interleaved, every fault kind pre and post: exit status and whose thread
#   dumped the core. Natively pre ends with the kind's signal, post with 3.
# k128: the old pthread_detach test under the client with bpftrace on the kernel's signals: for each SIGSEGV with
#   si_code SI_KERNEL its kernel stack, and the signals delivered to that thread before it.
grep -m1 "model name" /proc/cpuinfo; uname -r
mode=$1; shift
sudo apt-get install -y -qq elfutils bpftrace >/dev/null 2>&1
DRC=$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)
DR=$HOME/.cache/v3compat/dr-$DRC; DBG=$DR/lib64/release/libdynamorio.so.debug
echo "DynamoRIO $DRC"
gcc -O2 -pthread lab/te_sig.c -o /tmp/te_sig && gcc -O2 -pthread lab/te_old.c -o /tmp/te_old || exit 1
CLIENT="build/inst/bin/v3compat-run --"
CD=/tmp/exd-core; mkdir -p $CD lab-out/keep; sudo sysctl -q kernel.core_pattern="$CD/core.%p"
declare -A kept
# one ARM SCEN KEEP BIN ARGS...: a run, its status and, for a core, its class (and gdb detail for KEEP of them).
one() {
  arm=$1 scen=$2 keepn=$3 bin=$4; shift 4
  case $arm in native) pre="";; client) pre=$CLIENT;; esac
  rm -f $CD/core.*
  timeout -s KILL 60 bash -c "ulimit -c unlimited; exec \"\$@\"" _ $pre $bin "$@" >/tmp/exd.out 2>/tmp/exd.err
  rc=$?
  c=$(ls $CD/core.* 2>/dev/null | head -1)
  if [ -n "$c" ]; then
    cls=$(python3 lab/texclass.py $c $bin $DBG /tmp/exd.err /tmp/exd.out)
    case "$cls" in *code=128*) ;; *) [ $keepn -gt 2 ] && keepn=2;; esac
    k=${kept[$arm.$scen.$(echo "$cls" | grep -o 'code=[0-9-]*')]:-0}
    if [ $k -lt $keepn ]; then
      kept[$arm.$scen.$(echo "$cls" | grep -o 'code=[0-9-]*')]=$((k + 1)); f=lab-out/keep/$arm.$scen.$(echo "$cls" | grep -o 'code=[0-9-]*' | tr = _).$((k + 1))
      cp /tmp/exd.err $f.err; echo "$cls" > $f.txt
      base=$(gdb -q -batch -c $c $bin -ex 'info proc mappings' 2>/dev/null | awk '/libdynamorio.so/ && $4=="0x0" {print $1; exit}')
      args=(-ex 'p $_siginfo' -ex 'info registers' -ex 'x/6i $pc' -ex 'echo FRAME-SIGINFO\n' -ex 'x/4dw $rsi'
            -ex 'x/2gx $rsi+16' -ex 'echo FRAME-RIP-RSP\n' -ex 'x/gx $rdx+0xa8' -ex 'x/gx $rdx+0xa0'
            -ex 'x/6i *(long *)($rdx+0xa8)' -ex 'echo FRAME-UCSTACK\n' -ex 'x/3gx $rdx+0x10')
      [ -n "$base" ] && args=(-ex "add-symbol-file $DBG -o $(printf '0x%x' $((base - 0x71000000)))" "${args[@]}")
      gdb -q -batch -c $c $bin "${args[@]}" -ex 'bt 25' -ex 'info threads' -ex 'thread apply all bt 8' \
        -ex 'maint info sections' -ex 'info proc mappings' >> $f.txt 2>&1
      p=${c##*.}
      [ -f /tmp/bt.out ] && grep -E "^@@BT [gdk] $p " -A 30 /tmp/bt.out | grep -vE '^@@BT [gd] [0-9]+ ' > $f.bt.k
      [ -f /tmp/bt.out ] && grep -E "^@@BT [gd] $p " /tmp/bt.out | tail -400 > $f.bt
      sudo dmesg > $f.dmesg 2>&1
    fi
    rm -f $c
  else
    cls="CLS none"
  fi
  echo "@@RUN $arm $scen rc=$rc $(echo "$cls" | head -1)"
}
summary() {
python3 - lab-out/runs.txt <<'EOF'
import sys, re, collections
c = collections.Counter(); runs = collections.Counter()
for l in open(sys.argv[1]):
    m = re.match(r'@@RUN (\S+) (\S+) rc=(\d+) CLS (.*)', l)
    if not m: continue
    arm, scen, rc, cls = m.groups(); runs[arm, scen] += 1
    if cls.startswith('none'):
        key = 'rc=' + rc
    else:
        d = dict(kv.split('=', 1) for kv in cls.split() if '=' in kv)
        w = d['where']
        w = 'safe_read' if 'safe_read' in w else re.sub(r'\+0x[0-9a-f]+$', '', w)
        key = f"rc={rc} code={d['code']} {w}"
    c[arm, scen, key] += 1
for (arm, scen), n in sorted(runs.items()):
    for (a, s, k), v in sorted(c.items()):
        if (a, s) == (arm, scen): print(f'RESULT {arm} {scen}: {v} of {n} {k}')
EOF
}
case $mode in
sig)
  R=${1:-4}; N=${2:-10}
  for r in $(seq $R); do
    for kind in segv abrt bus ill fpe ksegv; do for when in pre post; do for arm in native client; do
      for i in $(seq $N); do one $arm $kind.$when 1 /tmp/te_sig 64 300 $when $kind; done
    done; done; done
  done > lab-out/runs.txt
  summary;;
k128)
  cat > /tmp/k.bt <<'EOF'
tracepoint:signal:signal_deliver /comm == "te_old"/ {
  @s2[tid] = @s1[tid]; @c2[tid] = @c1[tid]; @s1[tid] = args->sig; @c1[tid] = args->code;
  if (!(args->sig == 11 && (args->code == 1 || args->code == 2))) {
    printf("@@BT d %d %d sig=%d code=%d\n", pid, tid, args->sig, args->code);
  }
}
tracepoint:signal:signal_generate /comm == "te_old" && !(args->sig == 11 && (args->code == 1 || args->code == 2))/ {
  printf("@@BT g %d %d tgt=%d sig=%d code=%d res=%d\n", pid, tid, args->pid, args->sig, args->code, args->result);
  if (args->sig == 11 && args->code == 128) {
    printf("@@BT k %d %d tgt=%d last=%d/%d prev=%d/%d\n%s\n", pid, tid, args->pid, @s1[args->pid], @c1[args->pid],
           @s2[args->pid], @c2[args->pid], kstack);
  }
}
tracepoint:sched:sched_process_exit /comm == "te_old"/ { delete(@s1[tid]); delete(@c1[tid]); delete(@s2[tid]); delete(@c2[tid]); }
END { clear(@s1); clear(@c1); clear(@s2); clear(@c2); }
EOF
  sudo BPFTRACE_MAP_KEYS_MAX=262144 bpftrace /tmp/k.bt > /tmp/bt.out 2>&1 &
  for i in $(seq 60); do grep -q Attaching /tmp/bt.out && break; sleep 1; done; sleep 2; head -3 /tmp/bt.out
  for i in $(seq ${1:-3000}); do sudo dmesg -C; one client old 40 /tmp/te_old 64 300; done > lab-out/runs.txt
  sudo pkill bpftrace; sleep 2
  grep -c '^@@BT k' /tmp/bt.out; grep -A 25 '^@@BT k' /tmp/bt.out | head -300 > lab-out/k128-all.txt
  summary;;
esac
