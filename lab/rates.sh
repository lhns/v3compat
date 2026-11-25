#!/bin/bash
# rates.sh: avxemu's fma-fast test output (hit counts, per-kind lanes) at the pinned commit.
set -e
git clone -q https://github.com/lhns/avxemu /tmp/avxemu && cd /tmp/avxemu && git checkout -q f01ad505e4a0507420bb56ebdd06989358c6fd2a
cmake -S . -B /tmp/avxb > /dev/null && cmake --build /tmp/avxb -j"$(nproc)" --target fma-fast > /dev/null
/tmp/avxb/test/fma-fast ref 2000000 2>&1 | sed 's/^/RESULT /'
/tmp/avxb/test/fma-fast silicon 4000000 2>&1 | sed 's/^/RESULT /'
