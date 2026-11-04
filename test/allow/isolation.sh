#!/bin/sh
# isolation.sh PROBE: a row writing to the code page must not reach the next row. Seed 3's repne cmpsq scans that
# page for the seed's pointer and faults at its end, unless a row left the pointer there, as this writer does.
set -eu
row() { printf '%s\t0\t-\t-\t-\t-\t-\n' "$1"; }
alone=$(row f248a7 | "$1" | tail -n 1)
after=$({ row 48894608; row f248a7; } | "$1" | tail -n 1)
echo "alone: $alone"; echo "after a writer: $after"
[ "$alone" = "$after" ] && [ "${alone%%	*}" = 8 ]
