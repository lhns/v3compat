#!/bin/bash
# ivy-ab.sh: the Ivy Bridge A/B, run in a pod on the target node from a clone of this repo (no AVX2, so no native
# arm). Builds build/ if missing, fetches duckdb, then runs these sections in order:
#   1. Phase 3, 10 rounds of `select 42`, four arms interleaved: cold, warm (translation cache), and both with
#      `-disable_traces` (.nt; its own cache, as DR options are part of the cache key). Untimed: cache hits of each warm
#      arm and peak RSS of each arm. LADDER=1 adds lab/startup.sh's ladder.
#   2. OPS=1: per-op cost (test/bench_bmi, bench/fma micro rows), inline against V3COMPAT_HELPERS_ONLY=1.
#   3. Phase 2 compute: duckdb compute cold, less `select 42` of the same round, and warm. A round starts only when
#      the node's 1-minute load is below QUIET_LOAD; a round that ran is never dropped.
#   4. Phase 2 signal injection: SIG_RUNS runs of regstress sig jit / xlsig, whatever the load; failures keep their output.
# Variables: LADDER=0, OPS=0, COMPUTE_ROUNDS=20, COMPUTE_MIN=180 (minutes section 3 may wait for quiet and run),
# SIG_RUNS=1000, QUIET_LOAD=6.0. Raw lines go to lab-out/ivy-raw.txt; the summary (median, min, max per key, CPU
# model, load before and after) is printed as JSON at the end.
set -u
cd "$(dirname "$0")/.." || exit 1
LADDER=${LADDER:-0} OPS=${OPS:-0} COMPUTE_ROUNDS=${COMPUTE_ROUNDS:-20} COMPUTE_MIN=${COMPUTE_MIN:-180}
SIG_RUNS=${SIG_RUNS:-1000} QUIET_LOAD=${QUIET_LOAD:-6.0}
mkdir -p lab-out
RAW=lab-out/ivy-raw.txt; : > $RAW
LOAD0=$(cat /proc/loadavg)

if [ ! -x build/inst/bin/v3compat-run ]; then
  cmake -S . -B build >lab-out/build.log 2>&1 && cmake --build build -j"$(nproc)" >>lab-out/build.log 2>&1 ||
    { tail -30 lab-out/build.log; exit 1; }
fi
mkdir -p /tmp/bench && gcc -O3 -march=x86-64-v3 bench/fma.c -o /tmp/bench/fma -lm || exit 1
if [ ! -x ./duckdb ]; then
  # duckdb 2.0.0's binary is the image's last layer (the image has no shell to copy it out of).
  layer=sha256:a565173728242afe7101852a9750ad0b0b0d1ac944dc2c18e6cddc65fd0ae697
  tok=$(curl -fsS "https://auth.docker.io/token?service=registry.docker.io&scope=repository:duckdb/duckdb:pull" |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')
  curl -fsSL -H "Authorization: Bearer $tok" "https://registry-1.docker.io/v2/duckdb/duckdb/blobs/$layer" -o /tmp/duck.tgz &&
    echo "${layer#sha256:}  /tmp/duck.tgz" | sha256sum -c --quiet && tar xzf /tmp/duck.tgz -C . duckdb || exit 1
fi

RUN="build/inst/bin/v3compat-run --"
Q42=(./duckdb -csv -noheader -c 'select 42')
QC=(./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)')
NT=-disable_traces
rec() { echo "$*" >> $RAW; echo "$*"; }
wall() { # KEY cmd...: one timed run, cold page cache not forced (as before)
  local key=$1; shift; local s e rc
  s=$(date +%s.%N); timeout -k 10 600 "$@" >/tmp/ivy-out 2>/tmp/ivy-err; rc=$?; e=$(date +%s.%N)
  rec "$key $(echo "$e - $s" | bc) rc=$rc"
}
rss() { # KEY cmd...: one untimed run; peak RSS in KB of the child (the launcher execs DynamoRIO in place)
  local key=$1; shift
  rec "rss.$key $(python3 -c '
import os, sys
pid = os.fork()
if pid == 0:
    try:
        fd = os.open(os.devnull, os.O_RDWR)
        os.dup2(fd, 0); os.dup2(fd, 1); os.dup2(fd, 2)
        os.execvp(sys.argv[1], sys.argv[1:])
    finally:
        os._exit(127)
_, st, ru = os.wait4(pid, 0)
print(ru.ru_maxrss, "rc=%d" % os.waitstatus_to_exitcode(st))' timeout -k 10 600 "$@")"
}

# 1. select 42, four arms. One untimed training run per cache, under the options of its warm arm.
C=/tmp/ivy-cache; CN=/tmp/ivy-cache-nt; rm -rf $C $CN; mkdir -p $C $CN
V3COMPAT_CACHE=$C V3COMPAT_CACHE_WRITE=1 timeout -k 10 600 $RUN "${Q42[@]}" >/dev/null 2>&1
V3COMPAT_DR_OPTS=$NT V3COMPAT_CACHE=$CN V3COMPAT_CACHE_WRITE=1 timeout -k 10 600 $RUN "${Q42[@]}" >/dev/null 2>&1
V3COMPAT_CACHE=$C V3COMPAT_CACHE_STATS=1 $RUN "${Q42[@]}" 2>&1 >/dev/null | grep '^pcache: pid' |
  while read -r l; do rec "cache.hits.warm $l"; done
V3COMPAT_DR_OPTS=$NT V3COMPAT_CACHE=$CN V3COMPAT_CACHE_STATS=1 $RUN "${Q42[@]}" 2>&1 >/dev/null | grep '^pcache: pid' |
  while read -r l; do rec "cache.hits.warm.nt $l"; done
rss select42.cold $RUN "${Q42[@]}"
V3COMPAT_CACHE=$C rss select42.warm $RUN "${Q42[@]}"
V3COMPAT_DR_OPTS=$NT rss select42.cold.nt $RUN "${Q42[@]}"
V3COMPAT_DR_OPTS=$NT V3COMPAT_CACHE=$CN rss select42.warm.nt $RUN "${Q42[@]}"
for r in $(seq 10); do
  wall select42.cold $RUN "${Q42[@]}"
  V3COMPAT_CACHE=$C wall select42.warm $RUN "${Q42[@]}"
  V3COMPAT_DR_OPTS=$NT wall select42.cold.nt $RUN "${Q42[@]}"
  V3COMPAT_DR_OPTS=$NT V3COMPAT_CACHE=$CN wall select42.warm.nt $RUN "${Q42[@]}"
done
[ "$LADDER" = 1 ] && PROFILE=0 bash lab/startup.sh 3 "hello-static hello-dyn duck42 duckcomp" 2>&1 |
  grep '^RESULT' | sed 's/^RESULT /ladder /' | tee -a $RAW

# 2. Per-op: bench_bmi rows and fma micro rows, inline against helpers only, 10 rounds.
if [ "$OPS" = 1 ]; then
  for r in $(seq 10); do
    for arm in inline helpers; do
      h=""; [ $arm = helpers ] && h=1
      V3COMPAT_HELPERS_ONLY=$h timeout -k 10 600 $RUN build/test/bench_bmi 2>/dev/null |
        awk -v a=$arm '$1 == "bench" { print "op." $2 "." a, $3 }' | while read -r l; do rec "$l"; done
      V3COMPAT_HELPERS_ONLY=$h timeout -k 10 600 $RUN /tmp/bench/fma 2>/dev/null |
        awk -v a=$arm '$1 == "micro" { print "op." $2 "." a, $3 }' | while read -r l; do rec "$l"; done
    done
  done
fi

# 3. duckdb compute cold, whole-process; less select 42 of the same round; and warm. Each round waits for a quiet node.
CEND=$(( $(date +%s) + COMPUTE_MIN * 60 ))
loadnow() { cut -d' ' -f1 /proc/loadavg; }
throttled() { { [ -r /sys/fs/cgroup/cpu.stat ] && awk '$1 == "nr_throttled" { print $2; f = 1 } END { if (!f) print "na" }' /sys/fs/cgroup/cpu.stat; } || echo na; }
quiet() { # wait until load < QUIET_LOAD; fail when the time box ends first
  local l
  while :; do
    l=$(loadnow)
    awk -v l="$l" -v q="$QUIET_LOAD" 'BEGIN { exit !(l < q) }' && return 0
    [ $(( $(date +%s) + 15 )) -ge $CEND ] && return 1
    echo "waiting for quiet: load $l >= $QUIET_LOAD"; sleep 15
  done
}
rounds=0
for r in $(seq "$COMPUTE_ROUNDS"); do
  quiet || { rec "compute.gaveup round=$r"; break; }
  l0=$(loadnow); t0=$(throttled)
  wall compute.cold $RUN "${QC[@]}"; read -r _ c rc1 < <(tail -1 $RAW)
  wall select42.cold.pair $RUN "${Q42[@]}"; read -r _ s rc2 < <(tail -1 $RAW)
  [ "$rc1 $rc2" = "rc=0 rc=0" ] && rec "compute.less_startup $(echo "$c - $s" | bc) rc=0"
  V3COMPAT_CACHE=$C wall compute.warm $RUN "${QC[@]}"
  l1=$(loadnow); t1=$(throttled); d=na
  [ "$t0" != na ] && [ "$t1" != na ] && d=$(( t1 - t0 ))
  rec "round.load $l0 $l1 throttled=$d"
  rounds=$((rounds + 1))
done
rec "compute.rounds $rounds"

# 4. Signal injection, exactly SIG_RUNS runs, whatever the load: every run must exit 0.
SIGK=("sig jit" "xlsig"); SIGN=(sig_jit xlsig); nk=(0 0); n=0; bad=0
for i in $(seq 0 $((SIG_RUNS - 1))); do
  j=$((i % 2))
  timeout -k 10 120 $RUN build/test/regstress 8 3 ${SIGK[$j]} >/tmp/ivy-out 2>&1; rc=$?
  n=$((n + 1)); nk[$j]=$((nk[j] + 1))
  if [ $rc != 0 ]; then
    bad=$((bad + 1)); cp /tmp/ivy-out lab-out/signal-fail-$n.txt
    rec "signal.fail ${SIGN[$j]} rc=$rc file=signal-fail-$n.txt"
  fi
  [ $((n % 50)) = 0 ] && echo "signal $n/$SIG_RUNS bad=$bad"
done
rec "signal.runs $n"; rec "signal.bad $bad"
rec "signal.runs.${SIGN[0]} ${nk[0]}"; rec "signal.runs.${SIGN[1]} ${nk[1]}"

LOAD1=$(cat /proc/loadavg)
python3 - "$RAW" "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)" "$LOAD0" "$LOAD1" "$(nproc) $(cat /sys/fs/cgroup/cpu.max 2>/dev/null)" <<'PY'
import json, statistics, sys
raw, model, l0, l1, cpus = sys.argv[1:6]
# lines with text or several numbers stay raw; counts are not timings
SKIP = ("ladder", "signal.", "round.load", "cache.hits", "compute.rounds", "compute.gaveup")
vals, other = {}, []
for line in open(raw):
    p = line.split()
    if len(p) >= 2 and not line.startswith(SKIP):
        try:
            v = float(p[1])
            if len(p) > 2 and p[2] != "rc=0":  # a failed run's time is not a measurement
                other.append(line.strip())
            else:
                vals.setdefault(p[0], []).append(v)
            continue
        except ValueError:
            pass
    other.append(line.strip())
out = {"cpu": model.strip(), "nproc_cpu_max": cpus, "loadavg_before": l0, "loadavg_after": l1,
       "stats": {k: {"n": len(v), "median": statistics.median(v), "min": min(v), "max": max(v)}
                 for k, v in sorted(vals.items())},
       "other": other}
print("IVY-AB-JSON " + json.dumps(out))
PY
