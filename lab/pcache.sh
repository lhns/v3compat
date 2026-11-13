#!/bin/bash
# pcache.sh MODE [N]: the lab-only translation cache prototype (docs/translation-cache.md, Phase 1), ASLR off.
#   time N    train a cache per workload and option set, verify every hit, then cold against warm, N interleaved reps
#   stress N  the stress subset and duckdb's hashes under a warm cache in verify mode, plus the remap test
#   ctest     ctest plain, then training, then verify mode (status 86 on a mismatch fails a test)
set -u
. lab/loop.sh
grep -m1 'model name' /proc/cpuinfo; nproc
R=$PWD/build/inst/bin/v3compat-run
B=/tmp/bench
mkdir -p lab-out
declare -A OPTS=([tr]="-vm_max_offset 0x0" [nt]="-vm_max_offset 0x0 -disable_traces")
digest() { grep -vE '^(round|total|micro) ' "$1" | sed -E 's/[0-9]+\.[0-9]+ (s|ms)//g' | sha256sum | cut -c1-12; }
wl() { # workload, then the command prefix
  local w=$1; shift
  case $w in
    s42) "$@" ./duckdb -csv -noheader -c 'select 42' ;;
    comp) "$@" ./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)' ;;
    node0) "$@" node -e 0 ;;
    jit) "$@" node bench/jit.js 8 1 ;;
    javav) "$@" java -XX:UseAVX=2 -version ;;
  esac
}
one() { # label workload [env...]: one timed run under the launcher with ASLR off
  local l=$1 w=$2; shift 2
  /usr/bin/time -f '%e' -o /tmp/p-time env "$@" timeout 900 bash -c "$(declare -f wl); wl $w setarch -R $R --" \
    >/tmp/p-out 2>/tmp/p-err; local rc=$?
  echo "RESULT $w $l rc=$rc wall=$(tail -1 /tmp/p-time) digest=$(digest /tmp/p-out)"
  grep -h '^pcache: pid' /tmp/p-err | sed "s/^/STATS $w $l /"
  grep -A2 -h 'VERIFY MISMATCH' /tmp/p-err | head -12 | sed "s/^/MISMATCH $w $l /"
  [ $rc != 0 ] && { echo "STDERR-HEAD $w $l"; grep -v '^pcache' /tmp/p-err | head -c 600 | head -6; }
  return 0
}
W="s42 comp node0 jit javav"
case "$1" in
time)
  for w in $W; do
    timeout 900 bash -c "$(declare -f wl); wl $w" >/tmp/p-out 2>&1; echo "RESULT $w native rc=$? digest=$(digest /tmp/p-out)"
  done
  for c in tr nt; do
    rm -rf /tmp/pc-$c; mkdir -p /tmp/pc-$c
    for w in $W; do
      one train-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}" V3COMPAT_CACHE=/tmp/pc-$c V3COMPAT_CACHE_WRITE=1 V3COMPAT_CACHE_STATS=1
      # each workload's own cache: duckdb's two share a file name
      mkdir -p /tmp/pc-$c-$w; cp /tmp/pc-$c/*.pcache /tmp/pc-$c-$w/
    done
    ls -la /tmp/pc-$c-*/ | grep pcache
    for w in $W; do
      one verify-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}" V3COMPAT_CACHE=/tmp/pc-$c-$w V3COMPAT_CACHE_VERIFY=2 V3COMPAT_CACHE_STATS=1
    done
  done
  for r in $(seq $2); do
    for w in $W; do
      for c in tr nt; do
        one cold-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}"
        one warm-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}" V3COMPAT_CACHE=/tmp/pc-$c-$w
      done
    done
  done
  # hit counts of a warm run without verify
  for c in tr nt; do for w in $W; do
    one stats-$c $w V3COMPAT_DR_OPTS="${OPTS[$c]}" V3COMPAT_CACHE=/tmp/pc-$c-$w V3COMPAT_CACHE_STATS=1
  done; done ;;
stress)
  N=${2:-60}
  export V3COMPAT_DR_OPTS="-vm_max_offset 0x0"
  RS=build/test/regstress
  rm -rf /tmp/pcs; mkdir -p /tmp/pcs
  # the remap test: a cached block of one library, unmapped, another library's code at the same address
  printf 'int f(void) { return 1; }\n' > /tmp/liba.c; printf 'int f(void) { return 2; }\n' > /tmp/libb.c
  gcc -O1 -shared -fPIC /tmp/liba.c -o /tmp/liba.so; gcc -O1 -shared -fPIC /tmp/libb.c -o /tmp/libb.so
  printf '%s\n' '#include <dlfcn.h>' '#include <stdio.h>' \
    'int main(int c, char **v) { for (int i = 1; i < c; i++) { void *h = dlopen(v[i], RTLD_NOW); int (*f)(void) = (int (*)(void))dlsym(h, "f");' \
    '  printf("%s %p %d\n", v[i], (void *)f, f()); dlclose(h); } return 0; }' > /tmp/remap.c
  gcc -O1 /tmp/remap.c -o /tmp/remap -ldl
  for order in "/tmp/liba.so /tmp/libb.so" "/tmp/libb.so /tmp/liba.so"; do
    rm -rf /tmp/pcr; mkdir -p /tmp/pcr
    V3COMPAT_CACHE=/tmp/pcr V3COMPAT_CACHE_WRITE=1 setarch -R $R -- /tmp/remap $order > /tmp/r-train.txt
    for o2 in "/tmp/liba.so /tmp/libb.so" "/tmp/libb.so /tmp/liba.so"; do
      V3COMPAT_CACHE=/tmp/pcr V3COMPAT_CACHE_VERIFY=1 V3COMPAT_CACHE_STATS=1 setarch -R $R -- /tmp/remap $o2 > /tmp/r-out.txt 2>/tmp/r-err.txt; rc=$?
      setarch -R /tmp/remap $o2 > /tmp/r-native.txt
      echo "RESULT remap train=[$order] run=[$o2] rc=$rc same-as-native=$(cmp -s <(cut -d' ' -f1,3 /tmp/r-out.txt) <(cut -d' ' -f1,3 /tmp/r-native.txt) && echo yes || echo NO) $(tr '\n' ' ' < /tmp/r-out.txt)"
      grep '^pcache: pid' /tmp/r-err.txt | sed 's/^/STATS remap /'
    done
  done
  # training runs for the stress programs, then verify mode on every run
  V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_WRITE=1 setarch -R $R -- $RS 8 10 sig jit > /dev/null
  img='duckdb/duckdb:2.0.0@sha256:453caf4f1c445ee8363acaa6cd383751396d2eae0d02f362ada64d87267a4221'
  [ -x ./duckdb ] || { docker pull -q "$img" >/dev/null; c=$(docker create "$img"); docker cp "$c:/duckdb" ./duckdb; docker rm "$c" >/dev/null; }
  V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_WRITE=1 setarch -R $R -- ./duckdb -csv -noheader -f test/apps/duckdb-varied.sql > /dev/null
  ls -la /tmp/pcs
  export V3COMPAT_CACHE=/tmp/pcs V3COMPAT_CACHE_VERIFY=1
  for k in "jit" "sig" "idle jit"; do loop pcregc "$N" 60 setarch -R $R -- $RS 8 10 $k; done
  loop pcsig "$N" 60 setarch -R $R -- $RS 8 10 sig jit
  sh test/apps/duckdb.sh "" ./duckdb > lab-out/native.txt
  sh test/apps/duckdb.sh "setarch -R $R --" ./duckdb 10 > lab-out/client.txt 2>lab-out/client.err </dev/null; echo "duckdb.sh rc=$?"
  diff <(sed 's/ wall.*//' lab-out/native.txt | sort) <(sed 's/ wall.*//' lab-out/client.txt | sort -u) | head -20
  echo "RESULT pcduck: diff rc=${PIPESTATUS[0]}, runs 10"; head -5 lab-out/client.err
  V3COMPAT_CACHE_STATS=1 setarch -R $R -- ./duckdb -csv -noheader -f test/apps/duckdb-varied.sql 2>&1 >/dev/null | grep '^pcache' | sed 's/^/STATS duck /' ;;
ctest)
  export V3COMPAT_DR_OPTS="-vm_max_offset 0x0"
  rm -rf /tmp/pct; mkdir -p /tmp/pct
  setarch -R ctest --test-dir build -j4 > lab-out/ctest-plain.txt 2>&1; echo "RESULT ctest plain rc=$?"
  V3COMPAT_CACHE=/tmp/pct V3COMPAT_CACHE_WRITE=1 setarch -R ctest --test-dir build -j4 > lab-out/ctest-train.txt 2>&1; echo "RESULT ctest train rc=$?"
  ls /tmp/pct | wc -l
  V3COMPAT_CACHE=/tmp/pct V3COMPAT_CACHE_VERIFY=1 setarch -R ctest --test-dir build -j4 --output-on-failure > lab-out/ctest-verify.txt 2>&1; echo "RESULT ctest verify rc=$?"
  for f in plain train verify; do echo "== $f"; grep -E "tests passed|\*\*\*Failed|Not Run|\(Failed\)|Timeout" lab-out/ctest-$f.txt | head -30; done
  grep -h "VERIFY MISMATCH" lab-out/ctest-verify.txt | head -10 ;;
esac
