#!/bin/sh
# duckdb-reset.sh RUN DUCKDB RUNS: the query that retires shared trace heads, under RUN and any V3COMPAT_DR_OPTS or DR
# options it brings. Output must match native and exit 0 every time.
RUN=$1; D=$2; RUNS=$3; here=$(dirname "$0"); dir=$(mktemp -d)
sed -n '1p;7p;8p' "$here/duckdb-varied.sql" > "$dir/q.sql"
"$D" -csv -noheader -f "$dir/q.sql" > "$dir/native" 2>&1 || exit 2
bad=0; i=0
while [ "$i" -lt "$RUNS" ]; do i=$((i+1))
    timeout 300 $RUN "$D" -csv -noheader -f "$dir/q.sql" > "$dir/out" 2>"$dir/err"; rc=$?
    if [ "$rc" != 0 ] || ! cmp -s "$dir/native" "$dir/out"; then bad=$((bad+1)); echo "run $i: exit $rc" >&2; diff "$dir/native" "$dir/out" | head -4 >&2; fi
done
rm -rf "$dir"; echo "$bad of $RUNS bad"; [ "$bad" = 0 ]
