#!/bin/bash
# tex.sh ROUNDS N: thread_exit's exit-time SIGSEGV (lab-te-exit-*). Per round and arm (native, client, empty client,
# DynamoRIO alone), interleaved: the fixed test (ctl), an app fault at ms/2 (pre) or right after main printed (post),
# pre with 1,000 threads (pre1k), and the old pthread_detach test (old, client and empty client only). Every core is
# classified by lab/texclass.py (whose fault the dumping thread took) and deleted; the first two per scenario and arm
# are also kept symbolized with every thread's backtrace.
grep -m1 "model name" /proc/cpuinfo
R=${1:-4}; N=${2:-25}
sudo apt-get install -y -qq elfutils >/dev/null 2>&1
DRC=$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)
DR=$HOME/.cache/v3compat/dr-$DRC; DBG=$DR/lib64/release/libdynamorio.so.debug
echo "DynamoRIO $DRC"
cmake -S lab/nullclient -B /tmp/null-b -DDynamoRIO_DIR=$DR/cmake >/dev/null && cmake --build /tmp/null-b >/dev/null || exit 1
gcc -O2 -pthread lab/te_fault.c -o /tmp/te_fault && gcc -O2 -pthread lab/te_old.c -o /tmp/te_old || exit 1
CLIENT="build/inst/bin/v3compat-run --"
NULLRUN="$DR/bin64/drrun -no_enable_reset -no_ignore_syscalls -c /tmp/null-b/libnull.so --"
DRRUN="$DR/bin64/drrun -no_enable_reset -no_ignore_syscalls --"
CD=/tmp/tex-core; mkdir -p $CD lab-out/keep; sudo sysctl -q kernel.core_pattern="$CD/core.%p"
declare -A kept
# one ARM SCEN BIN ARGS...: a run, its status and, for a core, its class.
one() {
  arm=$1 scen=$2 bin=$3; shift 3
  case $arm in native) pre="";; client) pre=$CLIENT;; null) pre=$NULLRUN;; dr) pre=$DRRUN;; esac
  rm -f $CD/core.*
  # 64 MB of core holds every thread's notes; the full core only for the kept ones.
  lim=65536; k=${kept[$arm.$scen]:-0}; [ $k -lt 2 ] && [ $scen != pre1k ] && lim=unlimited
  timeout -s KILL 60 bash -c "ulimit -c $lim; exec \"\$@\"" _ $pre $bin "$@" >/tmp/tex.out 2>/tmp/tex.err
  rc=$?
  c=$(ls $CD/core.* 2>/dev/null | head -1)
  if [ -n "$c" ]; then
    cls=$(python3 lab/texclass.py $c $bin $DBG /tmp/tex.err /tmp/tex.out)
    if [ $k -lt 2 ]; then
      kept[$arm.$scen]=$((k + 1)); f=lab-out/keep/$arm.$scen.$((k + 1))
      cp /tmp/tex.err $f.err; cp /tmp/tex.out $f.out; echo "$cls" > $f.txt
      base=$(gdb -q -batch -c $c $bin -ex 'info proc mappings' 2>/dev/null | awk '/libdynamorio.so/ && $4=="0x0" {print $1; exit}')
      args=(-ex 'p $_siginfo' -ex 'info registers' -ex 'x/6i $pc')
      [ -n "$base" ] && args=(-ex "add-symbol-file $DBG -o $(printf '0x%x' $((base - 0x71000000)))" "${args[@]}")
      gdb -q -batch -c $c $bin "${args[@]}" -ex 'bt 25' -ex 'info threads' -ex 'thread apply all bt 12' >> $f.txt 2>&1
    fi
    rm -f $c
  else
    cls="CLS none tex=$(grep -c '^@@TEX' /tmp/tex.err)"
  fi
  echo "@@RUN $arm $scen rc=$rc $(echo "$cls" | head -1)"
  echo "$cls" | sed -n '2,$p' | head -4
}
for r in $(seq $R); do
  for arm in native client null dr; do
    for i in $(seq $N); do one $arm ctl build/test/thread_exit 64 300; done
    for i in $(seq $N); do one $arm pre /tmp/te_fault 64 300 pre; done
    for i in $(seq $N); do one $arm post /tmp/te_fault 64 300 post; done
    for i in $(seq $((N / 5))); do one $arm pre1k /tmp/te_fault 1000 300 pre; done
    case $arm in client|null) for i in $(seq $((N * 8))); do one $arm old /tmp/te_old 64 300; done;; esac
  done
done > lab-out/runs.txt
python3 - lab-out/runs.txt <<'EOF'
import sys, re, collections
c = collections.Counter(); runs = collections.Counter()
for l in open(sys.argv[1]):
    m = re.match(r'@@RUN (\S+) (\S+) rc=(\d+) CLS (.*)', l)
    if not m: continue
    arm, scen, rc, cls = m.groups(); runs[arm, scen] += 1
    if cls.startswith('none'):
        key = 'rc=' + rc + (' tex' if not cls.endswith('tex=0') else '')
    else:
        d = dict(kv.split('=', 1) for kv in cls.split() if '=' in kv)
        where = d['where']
        w = 'safe_read' if 'safe_read' in where else where
        tex = d.get('tex', '-')
        tex = ','.join(sorted({t.split(':')[0] for t in tex.split(',')})) if tex != '-' else '-'
        key = f"rc={rc} code={d['code']} {w} printed={d['printed']} tex={tex}"
    c[arm, scen, key] += 1
for (arm, scen), n in sorted(runs.items()):
    for (a, s, k), v in sorted(c.items()):
        if (a, s) == (arm, scen): print(f'RESULT {arm} {scen}: {v} of {n} {k}')
EOF
