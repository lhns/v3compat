#!/bin/sh
# duckdb.sh RUN DUCKDB [RUNS]: print output hashes and wall times of duckdb's benchmark queries under RUN,
# the varied one RUNS times. RUN is a command prefix, empty for native. Hashes must match native on an AVX2 host.
# Exits non-zero if any run did: a crash can match on a hash of partial output only by luck, but must not pass by it.
RUN=$1; D=$2; RUNS=${3:-1}; here=$(dirname "$0"); out=$(mktemp); fail=0
t() { name=$1; shift; s=$(date +%s.%N); timeout 600 "$@" >"$out" 2>&1; rc=$?; h=$(sha256sum <"$out" | cut -c1-16); e=$(date +%s.%N)
      printf '%s hash=%s wall=%.2f\n' "$name" "$h" "$(echo "$e - $s" | bc)"
      [ "$rc" = 0 ] || { echo "$name exited $rc" >&2; fail=1; }; }
t select42 $RUN "$D" -csv -noheader -c "select 42"
t compute $RUN "$D" -csv -noheader -c "select sum(range*range) from range(30000000)"
i=0; while [ "$i" -lt "$RUNS" ]; do i=$((i+1)); t varied $RUN "$D" -csv -noheader -f "$here/duckdb-varied.sql"; done
rm -f "$out"; exit $fail
