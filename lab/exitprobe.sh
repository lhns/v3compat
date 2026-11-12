#!/bin/bash
# exitprobe.sh: a frame helper that exits the process (on its own stack) must end with 86, threads or not.
grep -m1 'model name' /proc/cpuinfo
cmake -S . -B /tmp/p5 -DCMAKE_C_FLAGS=-DV3C_PROBE=5 >/dev/null && cmake --build /tmp/p5 -j >/dev/null || echo "RESULT build failed"
R=/tmp/p5/inst/bin/v3compat-run
for i in $(seq 20); do $R -- build/test/bench_bmi >/dev/null 2>/tmp/e; echo "bench rc=$?"; done | sort | uniq -c | sed 's/^/RESULT /'
for i in $(seq 20); do $R -- build/test/regstress 8 1 >/dev/null 2>>/tmp/e2; echo "regstress rc=$?"; done | sort | uniq -c | sed 's/^/RESULT /'
head -3 /tmp/e /tmp/e2
