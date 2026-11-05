#!/bin/sh
# opcounts.sh RUN DUCKDB: executed count per x86-64-v3 opcode in real programs, from the client's -stats.
# Prints "program opcode count", largest first per program. For reading, not a test.
RUN=$1; D=$2; here=$(cd "$(dirname "$0")" && pwd); t=$(mktemp -d)
head -c 3000000 "$D" > "$t/in"
count() { name=$1; shift
  V3COMPAT_STATS=1 "$RUN" -- "$@" > /dev/null 2> "$t/err" || echo "$name exited $?" >&2
  sed -n 's/^v3compat: op \([^ ]*\) \([0-9]*\)$/\1 \2/p' "$t/err" | awk -v n="$name" '{c[$1]+=$2} END {for (o in c) print n, o, c[o]}' | sort -k3,3nr; }
count compute "$D" -csv -noheader -c "select sum(range*range) from range(30000000)"
count varied "$D" -csv -noheader -f "$here/duckdb-varied.sql"
count zstd zstd -q -f "$t/in" -o "$t/out"
count zstd-d zstd -q -d -f "$t/out" -o "$t/back"
rm -rf "$t"
