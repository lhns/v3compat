#!/bin/bash
# LAB ONLY: v3compat's ctest as ci.yml runs it, on this branch's DynamoRIO pin.
set -u
grep -m1 'model name' /proc/cpuinfo
cmake -S . -B build-ci -DV3COMPAT_SEMANTIC=ON >/dev/null && cmake --build build-ci -j >/dev/null || { echo "RESULT ctest build failed"; exit 1; }
chmod o+x "$HOME"
V3COMPAT_CI=1 ctest --test-dir build-ci --output-on-failure > lab-out/ctest.txt 2>&1; rc=$?
tail -15 lab-out/ctest.txt
echo "RESULT ctest rc=$rc $(grep -E 'tests passed|tests failed' lab-out/ctest.txt)"
