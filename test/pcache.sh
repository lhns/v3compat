#!/bin/sh
# pcache.sh RUN MAIN LIB LIB_B TOOL: the translation cache fails closed (docs/translation-cache.md, Phase 2).
# Every damaged, stale or foreign cache gives the native output, exit 0 and no hits, each rejected by the check
# meant for it; the same setup undamaged hits, also at another load base. LIB_B is another build of LIB.
RUN=$1; MAIN=$2; LIB=$3; LIB_B=$4; TOOL=$5
unset V3COMPAT_CACHE V3COMPAT_CACHE_WRITE V3COMPAT_CACHE_VERIFY V3COMPAT_CACHE_STATS V3COMPAT_DR_OPTS
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
bad=0
fail() { echo "FAIL: $*"; sed 's/^/  stderr: /' "$d/err" | head -8; bad=1; }
cp "$LIB" "$d/lib.so"; cp "$LIB_B" "$d/libb.so"
native() { "$MAIN" "$@" 2>/dev/null; }
# run DIR [VAR=value...]: one run reading the cache in DIR, with its stats
run() {
  dir=$1; shift
  env V3COMPAT_CACHE="$dir" V3COMPAT_CACHE_STATS=1 "$@" $RUN -- "$MAIN" "$d/lib.so" > "$d/out" 2> "$d/err"; rc=$?
}
train() { # DIR [prefix...]
  dir=$1; shift; rm -rf "$dir"; mkdir -p "$dir"
  V3COMPAT_CACHE="$dir" V3COMPAT_CACHE_WRITE=1 "$@" $RUN -- "$MAIN" "$d/lib.so" > /dev/null 2> "$d/train.err" ||
    { echo "FAIL: training run"; cat "$d/train.err"; exit 1; }
  [ -f "$dir/pcache_main.pcache" ] || { echo "FAIL: no cache written"; exit 1; }
}
st() { sed -n "s/^pcache: pid .* $1 \([0-9]*\) .*/\1/p" "$d/err" | head -1; }
base() { sed -n 's/^base //p' "$1" | head -1; }
# expect WHAT: the run gave the native output and exit 0
same() {
  want=$(native "$d/lib.so")
  [ "$rc" = 0 ] && [ "$(cat "$d/out")" = "$want" ] || fail "$1: rc=$rc output [$(cat "$d/out")] native [$want]"
}

train "$d/c"
python3 "$TOOL" check "$d/c/pcache_main.pcache" || fail "the written file does not check"
cp "$d/train.err" "$d/train0.err"

# Positive control: hits, at least half the lookups, and verify mode clean.
run "$d/c"; same control
h=$(st hits); l=$(st lookups)
[ "${h:-0}" -gt 0 ] && [ $((h * 2)) -ge "${l:-1}" ] || fail "control: hits ${h:-none} of ${l:-none} lookups"
echo "control: hits $h of $l lookups"
run "$d/c" V3COMPAT_CACHE_VERIFY=1; same "control, verify"
[ "$(st verify_bad)" = 0 ] && [ "$(st verify_ok)" -gt 0 ] || fail "control, verify: ok $(st verify_ok) bad $(st verify_bad)"

# The file as a whole: each rejected at open, by name.
for m in "flip-body:body hash" "truncate:size" "empty:size" "garbage:magic" "version:version" "key:key"; do
  mode=${m%%:*}; why=${m#*:}
  mkdir -p "$d/x-$mode"; python3 "$TOOL" corrupt "$d/c/pcache_main.pcache" "$mode" "$d/x-$mode/pcache_main.pcache"
  run "$d/x-$mode"; same "$mode"
  grep -q "rejected ($why)" "$d/err" || fail "$mode: not rejected as '$why'"
  [ -z "$(st hits)" ] || [ "$(st hits)" = 0 ] || fail "$mode: hits $(st hits)"
done

# Inside a file whose checksums were fixed up again: each entry rejected by the check meant for it.
for m in entry-hash:corrupt entry-len:corrupt index-off:corrupt record-kind:build value-class:build predicate:pre; do
  mode=${m%%:*}; counter=${m#*:}
  mkdir -p "$d/x-$mode"; python3 "$TOOL" corrupt "$d/c/pcache_main.pcache" "$mode" "$d/x-$mode/pcache_main.pcache"
  run "$d/x-$mode"; same "$mode"
  [ "$(st hits)" = 0 ] && [ "$(st "$counter")" -gt 0 ] || fail "$mode: hits $(st hits), $counter $(st "$counter")"
done

# Another client option is another key.
run "$d/c" V3COMPAT_HELPERS_ONLY=1; same "other options"
grep -q "rejected (key)" "$d/err" || fail "other options: not rejected as 'key'"

# Another build of the library at the same path: its blocks find no entry.
cp "$d/lib.so" "$d/liba.so"; cp "$d/libb.so" "$d/lib.so"
run "$d/c"; same "other build"
[ "$(st noentry)" -gt 0 ] || fail "other build: noentry $(st noentry)"
cp "$d/liba.so" "$d/lib.so"

# The library's code changed in place, build-id kept: the byte compare misses, the new code runs.
python3 - "$d/lib.so" <<'EOF' || fail "could not patch the library"
import sys
p = sys.argv[1]; b = open(p, 'rb').read(); k = bytes.fromhex('44332211')
assert b.count(k) == 1, b.count(k)
open(p, 'wb').write(b.replace(k, bytes.fromhex('88776655')))
EOF
run "$d/c"; same "patched code"
grep -q "mark 55667788" "$d/out" && [ "$(st bytes)" -gt 0 ] || fail "patched code: bytes $(st bytes), [$(cat "$d/out")]"
cp "$d/liba.so" "$d/lib.so"

# A library unloaded and another one loaded where it was, in both orders.
for order in "liba.so libb.so" "libb.so liba.so"; do
  set -- $order
  rm -rf "$d/r"; mkdir -p "$d/r"
  V3COMPAT_CACHE="$d/r" V3COMPAT_CACHE_WRITE=1 $RUN -- "$MAIN" "$d/$1" "$d/$2" > /dev/null 2>&1
  V3COMPAT_CACHE="$d/r" V3COMPAT_CACHE_VERIFY=1 $RUN -- "$MAIN" "$d/$2" "$d/$1" > "$d/out" 2> "$d/err"; rc=$?
  want=$(native "$d/$2" "$d/$1")
  [ "$rc" = 0 ] && [ "$(cat "$d/out")" = "$want" ] || fail "remap $order: rc=$rc [$(cat "$d/out")] native [$want]"
done

# Another load base, both ways round: trained with ASLR off, read with it on, and the reverse.
if setarch -R true 2>/dev/null; then
  for way in off-on on-off; do
    if [ $way = off-on ]; then train "$d/b" setarch -R; else train "$d/b"; fi
    if [ $way = off-on ]; then run "$d/b" V3COMPAT_CACHE_VERIFY=1; else run "$d/b" V3COMPAT_CACHE_VERIFY=1 setarch -R; fi
    same "base $way"
    tb=$(base "$d/train.err"); rb=$(base "$d/err"); h=$(st hits); l=$(st lookups)
    [ "$tb" != "$rb" ] || fail "base $way: the library loaded at $tb both times"
    [ "${h:-0}" -gt 0 ] && [ $((h * 2)) -ge "${l:-1}" ] && [ "$(st verify_bad)" = 0 ] ||
      fail "base $way: hits ${h:-none} of ${l:-none}, verify_bad $(st verify_bad)"
    echo "base $way: library at $tb, then $rb; hits $h of $l"
  done
else
  echo "setarch -R unavailable: the fixed-base half skipped"
  [ "$V3COMPAT_CI" = 1 ] && bad=1
fi

[ $bad = 0 ] && echo PASS
exit $bad
