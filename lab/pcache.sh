#!/bin/bash
# pcache.sh MODE [N]: the translation cache (docs/translation-cache.md, Phase 2), ASLR on, default DR options.
#   time N      train a cache per workload and option set, verify every hit, then cold against warm, N interleaved reps
#   twolayout   two training runs per workload (two ASLR layouts): entries for the same block must be identical
#   stress K N  the stress set kind K (duck, sig, regc, short) with N runs in verify mode, and N/10 without a cache
#   srab N      -stable_reach alone against default, no cache, long duckdb runs
#   diag        layouts and predicate misses of reads; xlsig N: regstress xlsig plain, warm and verify
#   ctest       ctest plain, then training, then verify mode (status 86 on a mismatch fails a test)
set -u
. lab/loop.sh
grep -m1 'model name' /proc/cpuinfo; nproc
R=$PWD/build/inst/bin/v3compat-run
[ -x $R ] || { echo "RESULT no v3compat-run: the build failed"; exit 1; }
T="python3 test/pcache_tool.py"
mkdir -p lab-out
declare -A OPTS=([tr]="" [nt]="-disable_traces")
digest() { grep -vE '^(round|total|micro) ' "$1" | sed -E 's/[0-9]+\.[0-9]+ (s|ms)//g' | sha256sum | cut -c1-12; }
wl() { # workload, then the command prefix
  local w=$1; shift
  case $w in
    s42) "$@" ./duckdb -csv -noheader -c 'select 42' ;;
    comp) "$@" ./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)' ;;
    node0) "$@" node -e 0 ;;
    jit) "$@" node bench/jit.js 8 1 ;;
    javav) "$@" java -XX:UseAVX=2 -version ;;
    regs) "$@" build/test/regstress 8 2 jit ;;
    misc) "$@" build/test/misc_forms ;;
    bigc) "$@" ./duckdb -csv -noheader -c 'select sum(range*range) from range(300000000)' ;;
    varied) "$@" ./duckdb -csv -noheader -f test/apps/duckdb-varied.sql ;;
  esac
}
one() { # label workload [env...]: one timed run under the launcher
  local l=$1 w=$2; shift 2
  /usr/bin/time -f '%e' -o /tmp/p-time env "$@" timeout 900 bash -c "$(declare -f wl); wl $w $R --" \
    >/tmp/p-out 2>/tmp/p-err; local rc=$?
  echo "RESULT $w $l rc=$rc wall=$(tail -1 /tmp/p-time) digest=$(digest /tmp/p-out)"
  grep -h '^pcache: pid' /tmp/p-err | sed "s/^/STATS $w $l /"
  grep -h 'rejected' /tmp/p-err | sed "s/^/REJECT $w $l /"
  grep -A2 -h 'VERIFY MISMATCH' /tmp/p-err | head -12 | sed "s/^/MISMATCH $w $l /"
  [ $rc != 0 ] && { echo "STDERR-HEAD $w $l"; grep -v '^pcache' /tmp/p-err | head -c 600 | head -6; }
  return 0
}
train() { # dir option-set workload
  rm -rf $1; mkdir -p $1
  one train-$2 $3 V3COMPAT_DR_OPTS="${OPTS[$2]}" V3COMPAT_CACHE=$1 V3COMPAT_CACHE_WRITE=1 V3COMPAT_CACHE_STATS=1
}
W="s42 comp node0 jit javav"
case "$1" in
time)
  for w in $W; do
    timeout 900 bash -c "$(declare -f wl); wl $w" >/tmp/p-out 2>/dev/null; echo "RESULT $w native rc=$? digest=$(digest /tmp/p-out)"
  done
  for c in tr nt; do
    for w in $W; do train /tmp/pc-$c-$w $c $w; ls -la /tmp/pc-$c-$w | grep pcache; done
    for w in $W; do
      one verify-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}" V3COMPAT_CACHE=/tmp/pc-$c-$w V3COMPAT_CACHE_VERIFY=2 V3COMPAT_CACHE_STATS=1
    done
  done
  for r in $(seq $2); do
    for w in $W; do
      for c in tr nt; do
        one cold-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}"
        one coldsr-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]} -stable_reach"
        one warm-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}" V3COMPAT_CACHE=/tmp/pc-$c-$w
      done
    done
  done
  # hit counts of a warm run without verify
  for c in tr nt; do for w in $W; do
    one stats-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}" V3COMPAT_CACHE=/tmp/pc-$c-$w V3COMPAT_CACHE_STATS=1
  done; done ;;
twolayout)
  for w in $W; do
    train /tmp/pa-$w tr $w; train /tmp/pb-$w tr $w
    for f in /tmp/pa-$w/*.pcache; do
      $T check $f
      echo "RESULT twolayout $w $(basename $f): $($T diff $f /tmp/pb-$w/$(basename $f) | tail -1)"
      $T diff $f /tmp/pb-$w/$(basename $f) | grep DIFF | head -8
      [ $w = node0 ] && cp $f lab-out/a-$(basename $f) && cp /tmp/pb-$w/$(basename $f) lab-out/b-$(basename $f)
    done
  done ;;
diag)
  # Where predicate misses come from: the layout of a training run and of reads.
  for w in s42 node0 regs misc; do
    rm -rf /tmp/pd; mkdir -p /tmp/pd
    V3COMPAT_CACHE=/tmp/pd V3COMPAT_CACHE_WRITE=1 V3COMPAT_CACHE_STATS=2 bash -c "$(declare -f wl); wl $w $R --" 2>&1 >/dev/null | grep -E "layout" | sed "s/^/TRAIN $w /"
    for i in $(seq 10); do
      V3COMPAT_CACHE=/tmp/pd V3COMPAT_CACHE_STATS=2 bash -c "$(declare -f wl); wl $w $R --" 2>&1 >/dev/null | grep -E "layout|predicate|pcache: pid" | cut -c1-330 | head -4 | sed "s/^/READ $w $i /"
    done
  done ;;
srab)
  # Steady-state cost of -stable_reach alone, no cache: long duckdb runs (a non-PIE binary), interleaved.
  for r in $(seq $2); do
    for a in base sr; do
      o=""; [ $a = sr ] && o="-stable_reach"
      one srab-$a bigc V3COMPAT_DR_OPTS="$o"
      one srab-$a varied V3COMPAT_DR_OPTS="$o"
    done
  done ;;
ctestrace)
  # Whether the cache changes how often a ctest fails with DynamoRIO's signal at thread start: plain, training, warm
  # and verify interleaved, each a whole ctest -j4 as in CI's cache job, N passes.
  rm -rf /tmp/ctr; mkdir -p /tmp/ctr/c && chmod 1777 /tmp/ctr /tmp/ctr/c
  V3COMPAT_CACHE=/tmp/ctr/c V3COMPAT_CACHE_WRITE=1 ctest --test-dir build -j4 > /dev/null 2>&1
  for i in $(seq $2); do for a in plain train warm verify; do
    case $a in plain) e="V3COMPAT_CACHE=" ;; train) rm -rf /tmp/ctr/t; mkdir -p /tmp/ctr/t; chmod 1777 /tmp/ctr/t; e="V3COMPAT_CACHE=/tmp/ctr/t V3COMPAT_CACHE_WRITE=1" ;;
      warm) e="V3COMPAT_CACHE=/tmp/ctr/c" ;; verify) e="V3COMPAT_CACHE=/tmp/ctr/c V3COMPAT_CACHE_VERIFY=1" ;; esac
    env $e ctest --test-dir build -j4 --output-on-failure > lab-out/ct-$a-$i.txt 2>&1
    f=$(grep -F "***Failed" lab-out/ct-$a-$i.txt | grep -v "setuid" | sed -E 's/.*Test +#[0-9]+: ([^ ]+) .*/\1/' | tr '\n' ' ')
    k=$(grep -c "native thread" lab-out/ct-$a-$i.txt)
    echo "RESULT ctestrace $a pass $i failed [$f] native-thread-lines $k"
  done; done ;;
xlsig)
  # ctest's regstress-xlsig (SIGXCPU at emulated instructions), arms interleaved in blocks of 10
  rm -rf /tmp/pcx; mkdir -p /tmp/pcx
  V3COMPAT_CACHE=/tmp/pcx V3COMPAT_CACHE_WRITE=1 $R -- build/test/regstress 8 10 xlsig jit > /dev/null
  for b in $(seq $(($2 / 10))); do
    bash -c ". lab/loop.sh; loop xl-plain 10 60 $R -- build/test/regstress 8 10 xlsig jit" | grep RESULT
    V3COMPAT_CACHE=/tmp/pcx bash -c ". lab/loop.sh; loop xl-warm 10 60 $R -- build/test/regstress 8 10 xlsig jit" | grep RESULT
    V3COMPAT_CACHE=/tmp/pcx V3COMPAT_CACHE_VERIFY=1 bash -c ". lab/loop.sh; loop xl-verify 10 60 $R -- build/test/regstress 8 10 xlsig jit" | grep RESULT
  done ;;
stress)
  # Trained once per kind, then N runs reading it in verify mode (exit 86 on a mismatch), N/10 with no cache.
  K=$2 N=$3 RS=build/test/regstress
  rm -rf /tmp/pcs; mkdir -p /tmp/pcs
  case $K in
  duck)
    V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_WRITE=1 $R -- ./duckdb -csv -noheader -f test/apps/duckdb-varied.sql > /dev/null
    V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_STATS=1 $R -- ./duckdb -csv -noheader -f test/apps/duckdb-varied.sql 2>&1 >/dev/null | grep '^pcache: pid' | sed 's/^/STATS duck /'
    sh test/apps/duckdb.sh "" ./duckdb > lab-out/native.txt
    for a in plain verify; do
      n=$N; [ $a = plain ] && n=$((N / 10)); [ $n -lt 1 ] && n=1
      e=""; [ $a = verify ] && e="V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_VERIFY=1"
      env $e sh test/apps/duckdb.sh "$R --" ./duckdb $n > lab-out/$a.txt 2>lab-out/$a.err </dev/null; rc=$?
      d=$(diff <(sed 's/ wall.*//' lab-out/native.txt | sort) <(sed 's/ wall.*//' lab-out/$a.txt | sort -u) | wc -l)
      echo "RESULT duck $a runs $n rc=$rc difflines=$d"; [ $rc != 0 ] && head -3 lab-out/$a.err
    done ;;
  *)
    case $K in sig) KS=("sig jit") S=10 ;; regc) KS=("jit" "sig" "idle jit") S=10 ;; short) KS=("sig jit") S=1 ;; esac
    for k in "${KS[@]}"; do V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_WRITE=1 $R -- $RS 8 $S $k > /dev/null; done
    V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_STATS=1 $R -- $RS 8 $S ${KS[0]} 2>&1 >/dev/null | grep '^pcache: pid' | sed "s/^/STATS $K /"
    for k in "${KS[@]}"; do
      V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_VERIFY=1 bash -c ". lab/loop.sh; loop $K-verify $N 60 $R -- $RS 8 $S $k" | grep RESULT
      bash -c ". lab/loop.sh; loop $K-plain $((N / 10)) 60 $R -- $RS 8 $S $k" | grep RESULT
    done ;;
  esac ;;
ctest)
  rm -rf /tmp/pct; mkdir -p /tmp/pct
  ctest --test-dir build -j4 > lab-out/ctest-plain.txt 2>&1; echo "RESULT ctest plain rc=$?"
  V3COMPAT_CACHE=/tmp/pct V3COMPAT_CACHE_WRITE=1 ctest --test-dir build -j4 > lab-out/ctest-train.txt 2>&1; echo "RESULT ctest train rc=$?"
  ls /tmp/pct | wc -l
  V3COMPAT_CACHE=/tmp/pct V3COMPAT_CACHE_VERIFY=1 ctest --test-dir build -j4 --output-on-failure > lab-out/ctest-verify.txt 2>&1; echo "RESULT ctest verify rc=$?"
  for f in plain train verify; do echo "== $f"; grep -E "tests passed|\*\*\*Failed|Not Run|\(Failed\)|Timeout" lab-out/ctest-$f.txt | head -30; done
  grep -h "VERIFY MISMATCH" lab-out/ctest-verify.txt | head -10
  ctest --test-dir build -R pcache-fail-closed -V 2>&1 | grep -E "control|base|FAIL|PASS|skipped" | head -20
  for t in hello_dyn misc_forms vec_inline; do
    [ -x build/test/$t ] && V3COMPAT_CACHE=/tmp/pct V3COMPAT_CACHE_STATS=1 $R -- build/test/$t 2>&1 >/dev/null | grep '^pcache: pid' | sed "s/^/STATS $t /"
  done ;;
esac
