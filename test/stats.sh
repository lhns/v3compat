#!/bin/sh
# stats.sh RUN AVX2PROG HELLO: the client's per-class totals for avx2prog must be exact.
RUN=$1; PROG=$2; HELLO=$3
err=$(V3COMPAT_STATS=1 "$RUN" -- "$PROG" 2>&1 >/dev/null)
echo "$err"
for want in avx2=3 fma=1 bmi=4 lzcnt=1 tzcnt=1 movbe=1; do
  echo "$err" | grep -q "v3compat: stats .* $want " || { echo "missing $want"; exit 1; }
done
# Off by default: nothing on stderr.
quiet=$("$RUN" -- "$HELLO" 2>&1 >/dev/null) || exit 1
[ -z "$quiet" ] || { echo "stats printed without V3COMPAT_STATS=1: $quiet"; exit 1; }
