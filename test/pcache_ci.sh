#!/bin/bash
# pcache_ci.sh BUILD DUCKDB: the translation cache in CI (docs/translation-cache.md, Phase 2), ASLR on. Every
# hit is also translated fresh and compared (exit 86 on a difference) over ctest, a stress subset and duckdb's
# hashes; warm runs must hit, and two training runs at different layouts must write the same entries.
set -u
B=$1; DUCK=$2; R=$B/inst/bin/v3compat-run; T="python3 test/pcache_tool.py"
C=$(mktemp -d); trap 'rm -rf "$C"' EXIT
bad=0
fail() { echo "FAIL: $*"; bad=1; }
# setuid's checks run as another user, which must be able to write and read the cache
mkdir -p $C/ct && chmod 1777 $C $C/ct

echo "== ctest, training, then verify mode"
V3COMPAT_CACHE=$C/ct V3COMPAT_CACHE_WRITE=1 ctest --test-dir $B -j4 --output-on-failure > $C/train.txt 2>&1 ||
  { fail "ctest under training"; grep -F -A12 "***Failed" $C/train.txt | head -60; }
ls $C/ct | wc -l
V3COMPAT_CACHE=$C/ct V3COMPAT_CACHE_VERIFY=1 ctest --test-dir $B -j4 --output-on-failure > $C/verify.txt 2>&1 ||
  { fail "ctest in verify mode"; grep -E "\(Failed\)|\*\*\*|VERIFY MISMATCH" $C/verify.txt | head -40; }
grep -E "tests passed" $C/verify.txt

# floor DIR PERCENT NAME cmd...: a warm run hits at least PERCENT of its lookups, verify mode clean
floor() {
  local dir=$1 pct=$2 name=$3; shift 3
  V3COMPAT_CACHE=$dir V3COMPAT_CACHE_VERIFY=1 V3COMPAT_CACHE_STATS=1 "$@" > /dev/null 2> $C/err; local rc=$?
  local s; s=$(grep '^pcache: pid' $C/err | head -1)
  local h l; h=$(echo "$s" | sed -n 's/.* hits \([0-9]*\) .*/\1/p'); l=$(echo "$s" | sed -n 's/.* lookups \([0-9]*\) .*/\1/p')
  echo "$name: rc=$rc hits ${h:-?} of ${l:-?} lookups"
  [ "$rc" = 0 ] && [ -n "$h" ] && [ $((h * 100)) -ge $((pct * ${l:-1})) ] || { fail "$name: below $pct% or rc=$rc"; head -5 $C/err; }
}
echo "== hit rates, warm"
floor $C/ct 90 hello_dyn $R -- $B/test/hello_dyn
floor $C/ct 90 misc_forms $R -- $B/test/misc_forms
mkdir -p $C/d1 $C/d2
V3COMPAT_CACHE=$C/d1 V3COMPAT_CACHE_WRITE=1 $R -- $DUCK -csv -noheader -c 'select 42' > /dev/null
V3COMPAT_CACHE=$C/d2 V3COMPAT_CACHE_WRITE=1 $R -- $DUCK -csv -noheader -c 'select 42' > /dev/null
floor $C/d1 95 "duckdb select 42" $R -- $DUCK -csv -noheader -c 'select 42'

echo "== two layouts"
$T check $C/d1/duckdb.pcache || fail "the written file does not check"
$T diff $C/d1/duckdb.pcache $C/d2/duckdb.pcache || fail "entries differ between two layouts"

echo "== stress subset and duckdb's hashes, verify mode"
RS=$B/test/regstress
mkdir -p $C/rs
for k in "jit" "sig" "idle jit"; do V3COMPAT_CACHE=$C/rs V3COMPAT_CACHE_WRITE=1 $R -- $RS 8 10 $k > /dev/null; done
for k in "jit" "sig" "idle jit" "sig jit"; do
  n=0; for i in 1 2 3 4 5 6 7 8 9 10; do
    V3COMPAT_CACHE=$C/rs V3COMPAT_CACHE_VERIFY=1 timeout 120 $R -- $RS 8 10 $k > $C/out 2>&1 || { n=$((n + 1)); tail -3 $C/out; }
  done
  echo "regstress $k: $n of 10 bad"; [ $n = 0 ] || fail "regstress $k"
done
mkdir -p $C/dv
V3COMPAT_CACHE=$C/dv V3COMPAT_CACHE_WRITE=1 $R -- $DUCK -csv -noheader -f test/apps/duckdb-varied.sql > /dev/null
sh test/apps/duckdb.sh "" $DUCK > $C/native.txt
V3COMPAT_CACHE=$C/dv V3COMPAT_CACHE_VERIFY=1 sh test/apps/duckdb.sh "$R --" $DUCK 5 > $C/warm.txt || fail "duckdb.sh in verify mode"
diff <(sed 's/ wall.*//' $C/native.txt | sort) <(sed 's/ wall.*//' $C/warm.txt | sort -u) || fail "duckdb hashes differ from native"

[ $bad = 0 ] && echo PASS
exit $bad
