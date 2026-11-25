#!/bin/bash
# ivy-ab.sh: the Ivy Bridge traces A/B, run in a pod on the target node from a clone of this repo (no AVX2, so no
# native arm). Decides whether DynamoRIO traces ship: every section measures under each option set, interleaved.
# Option sets (V3COMPAT_DR_OPTS): d = default, nt = -disable_traces, th = -trace_threshold 1000. th is an internal
# DynamoRIO option; it is probed first (probe.trace_threshold) and dropped everywhere when rejected.
# Builds build/ if missing, fetches duckdb, then runs, each skippable:
#   1. P3=1: per set a training run into /tmp/ivy-cache-<set> and a cache-hits run, then 10 rounds of `select 42`,
#      cold and warm per set. LADDER=1 adds lab/startup.sh's ladder.
#   2. OPS=1: per-op cost (test/bench_bmi, bench/fma micro rows), inline against V3COMPAT_HELPERS_ONLY=1, OPS_ROUNDS
#      rounds per set in OPS_OPTSETS.
#   3. COMPUTE_ROUNDS>0: duckdb compute cold, `select 42` of the same round, their difference, and compute warm, per set.
#      A round starts only when the node's 1-minute load is below QUIET_LOAD; a round that ran is never dropped.
#   4. LONG_REPS>0: duckdb varied, javac of bench/Bench.java and `java Bench 8 1` per set; output digests recorded.
#   5. SIG_RUNS>0: signal injection (regstress sig jit / xlsig) under each of SIG_OPTSETS; failures keep their output.
# Variables: OPTSETS="d nt th", P3=1, OPS=1, OPS_ROUNDS=3, OPS_OPTSETS="d nt", COMPUTE_ROUNDS=20, COMPUTE_MIN=120
# (minutes section 3 may wait for quiet and run), QUIET_LOAD=6.0, LONG_REPS=3, SIG_RUNS=1000, SIG_OPTSETS=nt, LADDER=0.
# Needs python3, and java + javac for the Java part of section 4. Raw lines go to lab-out/ivy-raw.txt; a measured
# line is `KEY WALL rc=RC cpu=USER+SYS rss=KB` (seconds; the sum of user and system time of the process tree).
# The summary (median, min, max per key, CPU model, load before and after) is printed as JSON at the end.
set -u
cd "$(dirname "$0")/.." || exit 1
OPTSETS=${OPTSETS:-d nt th} P3=${P3:-1} OPS=${OPS:-1} OPS_ROUNDS=${OPS_ROUNDS:-3} OPS_OPTSETS=${OPS_OPTSETS:-d nt}
COMPUTE_ROUNDS=${COMPUTE_ROUNDS:-20} COMPUTE_MIN=${COMPUTE_MIN:-120} QUIET_LOAD=${QUIET_LOAD:-6.0}
LONG_REPS=${LONG_REPS:-3} SIG_RUNS=${SIG_RUNS:-1000} SIG_OPTSETS=${SIG_OPTSETS:-nt} LADDER=${LADDER:-0}
mkdir -p lab-out
RAW=lab-out/ivy-raw.txt; : > $RAW
LOAD0=$(cat /proc/loadavg)

if [ ! -x build/inst/bin/v3compat-run ]; then
  if ! { cmake -S . -B build >lab-out/build.log 2>&1 && cmake --build build -j"$(nproc)" >>lab-out/build.log 2>&1; }; then
    tail -30 lab-out/build.log; exit 1
  fi
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

RUN=(build/inst/bin/v3compat-run --)
Q42=(./duckdb -csv -noheader -c 'select 42')
QC=(./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)')
declare -A OPT=([d]="" [nt]="-disable_traces" [th]="-trace_threshold 1000")
OUT=/tmp/ivy-out ERR=/tmp/ivy-err
rec() { echo "$*" >> $RAW; echo "$*"; }
loadnow() { cut -d' ' -f1 /proc/loadavg; }
secbegin() { SEC0=$(loadnow); }
secend() { rec "section.load $1 $SEC0 $(loadnow)"; }

# meas KEY cmd...: one run under `timeout -k 10 $MEAS_T` (default 600), stdout to $OUT, stderr to $ERR. The raw line
# is `KEY WALL rc=RC cpu=USER+SYS rss=KB`. The launcher execs DynamoRIO in place and timeout waits for its child, so
# wait4's rusage covers the whole tree: user + system seconds, and the largest peak RSS in KB.
meas() {
  local key=$1; shift
  LAST=$(python3 - "$key" "${MEAS_T:-600}" "$OUT" "$ERR" "$@" <<'PY'
import os, sys, time
key, t, out, err, *cmd = sys.argv[1:]
t0 = time.monotonic()
pid = os.fork()
if pid == 0:
    try:
        os.dup2(os.open(os.devnull, os.O_RDONLY), 0)
        os.dup2(os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC), 1)
        os.dup2(os.open(err, os.O_WRONLY | os.O_CREAT | os.O_TRUNC), 2)
        os.execvp("timeout", ["timeout", "-k", "10", t] + cmd)
    finally:
        os._exit(127)
_, st, ru = os.wait4(pid, 0)
wall = time.monotonic() - t0
print("%s %.3f rc=%d cpu=%.3f rss=%d" % (key, wall, os.waitstatus_to_exitcode(st), ru.ru_utime + ru.ru_stime, ru.ru_maxrss))
PY
  )
  rec "$LAST"
}

# The option sets that apply: th only when the probe accepts it.
probe_th() {
  case " $OPTSETS $OPS_OPTSETS $SIG_OPTSETS " in *" th "*) ;; *) return ;; esac
  local base rc err
  V3COMPAT_DR_OPTS="" "${RUN[@]}" build/test/hello_dyn >/dev/null 2>$ERR; base=$(head -1 $ERR)
  V3COMPAT_DR_OPTS=${OPT[th]} "${RUN[@]}" build/test/hello_dyn >/dev/null 2>$ERR; rc=$?
  err=$(head -1 $ERR)
  if [ $rc = 0 ] && [ "$err" = "$base" ]; then rec "probe.trace_threshold ok"; return; fi
  rec "probe.trace_threshold rejected rc=$rc ${err:-no stderr}"
  OPTSETS=${OPTSETS//th/}; OPS_OPTSETS=${OPS_OPTSETS//th/}; SIG_OPTSETS=${SIG_OPTSETS//th/}
}
probe_th
rec "optsets $OPTSETS"

# Training run and cache-hits run per set, once (sections 1 and 3 need the caches).
TRAINED=
train() {
  local s
  TRAINED=1
  for s in $OPTSETS; do
    rm -rf "/tmp/ivy-cache-$s"; mkdir -p "/tmp/ivy-cache-$s"
    V3COMPAT_DR_OPTS=${OPT[$s]} V3COMPAT_CACHE=/tmp/ivy-cache-$s V3COMPAT_CACHE_WRITE=1 timeout -k 10 600 "${RUN[@]}" "${Q42[@]}" >/dev/null 2>&1
    V3COMPAT_DR_OPTS=${OPT[$s]} V3COMPAT_CACHE=/tmp/ivy-cache-$s V3COMPAT_CACHE_STATS=1 "${RUN[@]}" "${Q42[@]}" 2>&1 >/dev/null |
      grep '^pcache: pid' | while read -r l; do rec "cache.hits.$s $l"; done
  done
}

# 1. select 42, cold and warm per set, interleaved.
if [ "$P3" = 1 ]; then
  secbegin; train
  for r in $(seq 10); do
    for s in $OPTSETS; do
      V3COMPAT_DR_OPTS=${OPT[$s]} meas "select42.cold.$s" "${RUN[@]}" "${Q42[@]}"
      V3COMPAT_DR_OPTS=${OPT[$s]} V3COMPAT_CACHE=/tmp/ivy-cache-$s meas "select42.warm.$s" "${RUN[@]}" "${Q42[@]}"
    done
  done
  [ "$LADDER" = 1 ] && PROFILE=0 bash lab/startup.sh 3 "hello-static hello-dyn duck42 duckcomp" 2>&1 |
    grep '^RESULT' | sed 's/^RESULT /ladder /' | tee -a $RAW
  secend 1
fi

# 2. Per-op: bench_bmi rows and fma micro rows, inline against helpers only.
if [ "$OPS" = 1 ]; then
  secbegin
  for r in $(seq "$OPS_ROUNDS"); do
    for s in $OPS_OPTSETS; do
      for arm in inline helpers; do
        h=""; [ $arm = helpers ] && h=1
        V3COMPAT_DR_OPTS=${OPT[$s]} V3COMPAT_HELPERS_ONLY=$h timeout -k 10 600 "${RUN[@]}" build/test/bench_bmi 2>/dev/null |
          awk -v a=$arm -v s="$s" '$1 == "bench" { print "op." $2 "." a "." s, $3 }' | while read -r l; do rec "$l"; done
        V3COMPAT_DR_OPTS=${OPT[$s]} V3COMPAT_HELPERS_ONLY=$h timeout -k 10 600 "${RUN[@]}" /tmp/bench/fma 2>/dev/null |
          awk -v a=$arm -v s="$s" '$1 == "micro" { print "op." $2 "." a "." s, $3 }' | while read -r l; do rec "$l"; done
      done
    done
  done
  secend 2
fi

# 3. duckdb compute cold, whole-process; less select 42 of the same round; and warm. Each round waits for a quiet node.
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
if [ "$COMPUTE_ROUNDS" -gt 0 ]; then
  secbegin; [ -n "$TRAINED" ] || train
  CEND=$(( $(date +%s) + COMPUTE_MIN * 60 ))
  rounds=0
  for r in $(seq "$COMPUTE_ROUNDS"); do
    quiet || { rec "compute.gaveup round=$r"; break; }
    l0=$(loadnow); t0=$(throttled)
    for s in $OPTSETS; do
      V3COMPAT_DR_OPTS=${OPT[$s]} meas "compute.cold.$s" "${RUN[@]}" "${QC[@]}"; read -r _ c rc1 _ <<<"$LAST"
      V3COMPAT_DR_OPTS=${OPT[$s]} meas "select42.cold.pair.$s" "${RUN[@]}" "${Q42[@]}"; read -r _ w rc2 _ <<<"$LAST"
      [ "$rc1 $rc2" = "rc=0 rc=0" ] && rec "compute.less_startup.$s $(echo "$c - $w" | bc) rc=0"
      V3COMPAT_DR_OPTS=${OPT[$s]} V3COMPAT_CACHE=/tmp/ivy-cache-$s meas "compute.warm.$s" "${RUN[@]}" "${QC[@]}"
    done
    l1=$(loadnow); t1=$(throttled); d=na
    [ "$t0" != na ] && [ "$t1" != na ] && d=$(( t1 - t0 ))
    rec "round.load $l0 $l1 throttled=$d"
    rounds=$((rounds + 1))
  done
  rec "compute.rounds $rounds"
  secend 3
fi

# 4. Long workloads: what traces off costs on duckdb varied, javac and Java. Digests show every arm agrees.
if [ "$LONG_REPS" -gt 0 ]; then
  secbegin
  java=1
  if ! command -v java >/dev/null || ! command -v javac >/dev/null; then java=; rec "long.skip java/javac missing"; fi
  if [ -n "$java" ]; then
    # No native arm on this CPU: compile Bench once under the client, untimed, and reuse the classes.
    rm -rf /tmp/bench/classes; mkdir -p /tmp/bench/classes
    timeout -k 10 1800 "${RUN[@]}" javac -d /tmp/bench/classes bench/Bench.java >/dev/null 2>&1 ||
      { java=; rec "long.skip Bench compile failed"; }
  fi
  export MEAS_T=1800
  for r in $(seq "$LONG_REPS"); do
    for s in $OPTSETS; do
      V3COMPAT_DR_OPTS=${OPT[$s]} meas "varied.$s" "${RUN[@]}" ./duckdb -csv -noheader -f test/apps/duckdb-varied.sql
      rec "digest.varied.$s $(sha256sum $OUT | cut -c1-16)"
      [ -n "$java" ] || continue
      jd=$(mktemp -d /tmp/ivy-javac.XXXXXX)
      V3COMPAT_DR_OPTS=${OPT[$s]} meas "javac.$s" "${RUN[@]}" javac -d "$jd" bench/Bench.java
      rm -rf "$jd"
      V3COMPAT_DR_OPTS=${OPT[$s]} meas "java.$s" "${RUN[@]}" java -XX:UseAVX=2 -cp /tmp/bench/classes Bench 8 1
      rec "digest.java.$s $(grep checksum $OUT | tr '\n' ' ')"
    done
  done
  unset MEAS_T
  secend 4
fi

# 5. Signal injection, exactly SIG_RUNS runs per set, whatever the load: every run must exit 0.
if [ "$SIG_RUNS" -gt 0 ]; then
  secbegin
  SIGK=("sig jit" "xlsig"); SIGN=(sig_jit xlsig)
  for s in $SIG_OPTSETS; do
    nk=(0 0); n=0; bad=0
    for i in $(seq 0 $((SIG_RUNS - 1))); do
      j=$((i % 2))
      V3COMPAT_DR_OPTS=${OPT[$s]} timeout -k 10 120 "${RUN[@]}" build/test/regstress 8 3 ${SIGK[$j]} >$OUT 2>&1; rc=$?
      n=$((n + 1)); nk[j]=$((nk[j] + 1))
      if [ $rc != 0 ]; then
        bad=$((bad + 1)); cp $OUT "lab-out/signal-fail-$s-$n.txt"
        rec "signal.fail.$s ${SIGN[$j]} rc=$rc file=signal-fail-$s-$n.txt"
      fi
      [ $((n % 50)) = 0 ] && echo "signal.$s $n/$SIG_RUNS bad=$bad"
    done
    rec "signal.runs.$s $n"; rec "signal.bad.$s $bad"
    rec "signal.runs.${SIGN[0]}.$s ${nk[0]}"; rec "signal.runs.${SIGN[1]}.$s ${nk[1]}"
  done
  secend 5
fi

LOAD1=$(cat /proc/loadavg)
python3 - "$RAW" "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)" "$LOAD0" "$LOAD1" "$(nproc) $(cat /sys/fs/cgroup/cpu.max 2>/dev/null)" <<'PY'
import json, statistics, sys
raw, model, l0, l1, cpus = sys.argv[1:6]
# lines with text, digests or several numbers stay raw; counts are not timings
SKIP = ("ladder", "signal.", "round.load", "cache.hits", "compute.rounds", "compute.gaveup", "probe.", "digest.",
        "section.load", "optsets", "long.skip")
vals, other = {}, []
for line in open(raw):
    p = line.split()
    if len(p) >= 2 and not line.startswith(SKIP):
        try:
            v = float(p[1])
            kv = dict(t.split("=", 1) for t in p[2:] if "=" in t)
            if kv.get("rc", "0") != "0":  # a failed run's time is not a measurement
                other.append(line.strip())
            else:
                vals.setdefault(p[0], []).append(v)
                for f in ("cpu", "rss"):
                    if f in kv:
                        vals.setdefault(p[0] + "." + f, []).append(float(kv[f]))
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
