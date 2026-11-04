#!/bin/sh
# duckdb.sh RUN DUCKDB: print output hashes and wall times of duckdb's benchmark queries under RUN.
# RUN is a command prefix, empty for native. Hashes must match native on an AVX2 host.
RUN=$1; D=$2; here=$(dirname "$0")
t() { name=$1; shift; s=$(date +%s.%N); h=$("$@" 2>&1 | sha256sum | cut -c1-16); e=$(date +%s.%N)
      printf '%s hash=%s wall=%.2f\n' "$name" "$h" "$(echo "$e - $s" | bc)"; }
t select42 $RUN "$D" -csv -noheader -c "select 42"
t compute $RUN "$D" -csv -noheader -c "select sum(range*range) from range(30000000)"
t varied $RUN "$D" -csv -noheader -f "$here/duckdb-varied.sql"
