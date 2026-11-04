#!/bin/sh
# audit.sh RUN DUCKDB: how many forms off the audited list real programs translate, per program. Must be 0.
RUN=$1; D=$2; here=$(cd "$(dirname "$0")" && pwd); t=$(mktemp -d); bad=0
head -c 3000000 "$D" > "$t/in"
count() { name=$1; shift
  V3COMPAT_AUDIT=1 "$RUN" -- "$@" > /dev/null 2> "$t/err"
  grep 'v3compat: audit' "$t/err" | sed 's/ at 0x.*//' | sort -u > "$t/forms"
  n=$(wc -l < "$t/forms"); echo "$name: $n forms off the list"; cat "$t/forms"
  [ "$n" = 0 ] || bad=1; }
count hello sh -c 'echo hello'
count zstd zstd -q -f "$t/in" -o "$t/out"
count zstd-d zstd -q -d -f "$t/out" -o "$t/back"
count duckdb-select42 "$D" -csv -noheader -c "select 42"
count duckdb-compute "$D" -csv -noheader -c "select sum(range*range) from range(30000000)"
count duckdb-varied "$D" -csv -noheader -f "$here/duckdb-varied.sql"
cmp "$t/in" "$t/back" || bad=1
rm -rf "$t"
exit "$bad"
