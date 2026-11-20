#!/bin/bash
# ivy-ab.sh [MINUTES]: the Ivy Bridge A/B, run in a pod on the target node from a clone of this repo (no AVX2, so
# no native arm). Builds build/ if missing, fetches duckdb, then, in priority order within MINUTES (default 90):
#   1. duckdb `select 42` cold against a warm translation cache, and lab/startup.sh's ladder;
#   2. per-op cost (test/bench_bmi, bench/fma micro rows): inline against V3COMPAT_HELPERS_ONLY=1;
#   3. duckdb compute cold, whole-process and less `select 42` of the same round (the Phase 1 method);
#   4. signal injection (regstress sig, xlsig) until the time is up.
# Arms alternate within each round. Raw lines go to lab-out/ivy-raw.txt; the summary (median, min, max per key, CPU
# model, load before and after) is printed as JSON at the end.
set -u
cd "$(dirname "$0")/.." || exit 1
MIN=${1:-90}
left() { echo $(( END - $(date +%s) )); }
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

END=$(( $(date +%s) + MIN * 60 ))   # the time box starts after the build
RUN="build/inst/bin/v3compat-run --"
Q42=(./duckdb -csv -noheader -c 'select 42')
QC=(./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)')
rec() { echo "$*" >> $RAW; echo "$*"; }
wall() { # KEY cmd...: one timed run, cold page cache not forced (as before)
  local key=$1; shift; local s e rc
  s=$(date +%s.%N); timeout -k 10 600 "$@" >/tmp/ivy-out 2>/tmp/ivy-err; rc=$?; e=$(date +%s.%N)
  rec "$key $(echo "$e - $s" | bc) rc=$rc"
}

# 1. Cache cold against warm, select 42. One training run first (untimed for the result).
C=/tmp/ivy-cache; rm -rf $C; mkdir -p $C
V3COMPAT_CACHE=$C V3COMPAT_CACHE_WRITE=1 timeout -k 10 600 $RUN "${Q42[@]}" >/dev/null 2>&1
for r in $(seq 10); do
  wall select42.cold $RUN "${Q42[@]}"
  V3COMPAT_CACHE=$C wall select42.warm $RUN "${Q42[@]}"
done
[ "$(left)" -gt 1800 ] && PROFILE=0 bash lab/startup.sh 3 "hello-static hello-dyn duck42 duckcomp" 2>&1 |
  grep '^RESULT' | sed 's/^RESULT /ladder /' | tee -a $RAW

# 2. Per-op: bench_bmi rows and fma micro rows, inline against helpers only, 10 rounds.
for r in $(seq 10); do
  [ "$(left)" -gt 900 ] || break
  for arm in inline helpers; do
    h=""; [ $arm = helpers ] && h=1
    V3COMPAT_HELPERS_ONLY=$h timeout -k 10 600 $RUN build/test/bench_bmi 2>/dev/null |
      awk -v a=$arm '$1 == "bench" { print "op." $2 "." a, $3 }' | while read -r l; do rec "$l"; done
    V3COMPAT_HELPERS_ONLY=$h timeout -k 10 600 $RUN /tmp/bench/fma 2>/dev/null |
      awk -v a=$arm '$1 == "micro" { print "op." $2 "." a, $3 }' | while read -r l; do rec "$l"; done
  done
done

# 3. duckdb compute cold, whole-process; less select 42 of the same round; and warm.
for r in $(seq 10); do
  [ "$(left)" -gt 600 ] || break
  wall compute.cold $RUN "${QC[@]}"; c=$(tail -1 $RAW | cut -d' ' -f2)
  wall select42.cold.pair $RUN "${Q42[@]}"; s=$(tail -1 $RAW | cut -d' ' -f2)
  rec "compute.less_startup $(echo "$c - $s" | bc) rc=0"
  V3COMPAT_CACHE=$C wall compute.warm $RUN "${QC[@]}"
done

# 4. Signal injection until the time is up: every run must exit 0 ("bad" counts the rest).
n=0; bad=0
while [ "$(left)" -gt 60 ]; do
  for k in "sig jit" "xlsig"; do
    timeout -k 10 120 $RUN build/test/regstress 8 3 $k >/tmp/ivy-out 2>&1; rc=$?
    n=$((n + 1)); [ $rc = 0 ] || { bad=$((bad + 1)); rec "signal.fail $k rc=$rc $(tail -1 /tmp/ivy-out | cut -c1-120)"; }
  done
done
rec "signal.runs $n"; rec "signal.bad $bad"

LOAD1=$(cat /proc/loadavg)
python3 - "$RAW" "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)" "$LOAD0" "$LOAD1" "$(nproc) $(cat /sys/fs/cgroup/cpu.max 2>/dev/null)" <<'PY'
import json, statistics, sys
raw, model, l0, l1, cpus = sys.argv[1:6]
vals, other = {}, []
for line in open(raw):
    p = line.split()
    if len(p) >= 2 and not line.startswith(("ladder", "signal.fail")):
        try:
            vals.setdefault(p[0], []).append(float(p[1]))
            if len(p) > 2 and p[2] != "rc=0": other.append(line.strip())
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
