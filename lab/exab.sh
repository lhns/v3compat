#!/bin/bash
# exab.sh ROUNDS N: the exit-time SIGSEGV, base (/tmp/base, origin/main's pin) against this branch's pin, interleaved
# with native. Per round, kind and arm: te_sig pre and post (an app thread dies of the kind while threads start, or
# 1 ms after main's _exit(3)); the same with a vfork child alive (.vfork); the old pthread_detach test (8N runs) and thread_exit. Every core is classified by whose
# fault the dumping thread took. Natively: pre ends with the kind's signal from app:faulter, post and the rest with 3.
grep -m1 "model name" /proc/cpuinfo; uname -r
R=${1:-4}; N=${2:-10}
sudo apt-get install -y -qq elfutils >/dev/null 2>&1
drdir() { echo $HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' $1/cmake/FetchDynamoRIO.cmake); }
declare -A RUN=([native]="" [base]="/tmp/base/build/inst/bin/v3compat-run --" [fix]="build/inst/bin/v3compat-run --")
declare -A DBG=([native]=- [base]=$(drdir /tmp/base)/lib64/release/libdynamorio.so.debug [fix]=$(drdir .)/lib64/release/libdynamorio.so.debug)
echo "base ${DBG[base]} fix ${DBG[fix]}"
gcc -O2 -pthread lab/te_sig.c -o /tmp/te_sig && gcc -O2 -pthread lab/te_old.c -o /tmp/te_old || exit 1
CD=/tmp/exab-core; mkdir -p $CD lab-out/keep; sudo sysctl -q kernel.core_pattern="$CD/core.%p"
declare -A kept
one() {
  arm=$1 scen=$2 bin=$3; shift 3
  rm -f $CD/core.*
  timeout -s KILL 60 bash -c "ulimit -c unlimited; exec \"\$@\"" _ ${RUN[$arm]} $bin "$@" >/tmp/exab.out 2>/tmp/exab.err
  rc=$?
  c=$(ls $CD/core.* 2>/dev/null | head -1)
  if [ -n "$c" ]; then
    cls=$(python3 lab/texclass.py $c $bin ${DBG[$arm]} /tmp/exab.err /tmp/exab.out | head -1)
    k=${kept[$arm.$scen]:-0}
    if [ $k -lt 2 ] && [ $arm != native ]; then
      kept[$arm.$scen]=$((k + 1)); f=lab-out/keep/$arm.$scen.$((k + 1))
      echo "$cls" > $f.txt; cp /tmp/exab.err $f.err
      gdb -q -batch -c $c $bin -ex 'p $_siginfo' -ex 'info registers' -ex 'bt 12' -ex 'info threads' >> $f.txt 2>&1
    fi
    rm -f $c
  else
    cls="CLS none"
  fi
  echo "@@RUN $arm $scen rc=$rc $cls"
}
for r in $(seq $R); do
  for kind in segv abrt bus ill fpe ksegv; do for when in pre post; do
    for i in $(seq $N); do for arm in native base fix; do one $arm $kind.$when /tmp/te_sig 64 300 $when $kind; done; done
  done; done
  for kind in segv abrt ill ksegv; do for when in pre post; do
    for i in $(seq $N); do for arm in native base fix; do one $arm $kind.$when.vfork /tmp/te_sig 64 300 $when $kind vfork; done; done
  done; done
  for i in $(seq $((N * 8))); do for arm in native base fix; do one $arm old /tmp/te_old 64 300; done; done
  for i in $(seq $N); do for arm in base fix; do one $arm ctl build/test/thread_exit 64 300; done; done
done > lab-out/runs.txt
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
        w = d.get('where', '?')
        w = 'safe_read' if 'safe_read' in w else re.sub(r'\+0x[0-9a-f]+$', '', w)
        key = f"rc={rc} code={d.get('code', '?')} {w}"
    c[arm, scen, key] += 1
for (arm, scen), n in sorted(runs.items(), key=lambda x: (x[0][1], x[0][0])):
    for (a, s, k), v in sorted(c.items()):
        if (a, s) == (arm, scen): print(f'RESULT {scen} {arm}: {v} of {n} {k}')
EOF
