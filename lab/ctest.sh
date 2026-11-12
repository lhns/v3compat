#!/bin/bash
# ctest with the semantic suite, for a branch without a PR yet.
cmake -S . -B build -DV3COMPAT_SEMANTIC=ON >/dev/null && cmake --build build -j >/dev/null || { echo "RESULT build failed"; exit 1; }
ctest --test-dir build --output-on-failure -j4 > lab-out/ctest.txt 2>&1; rc=$?
grep -E "tests passed|Failed|\*\*\*" lab-out/ctest.txt | sed 's/^/RESULT /'; echo "RESULT ctest rc=$rc"
grep -A30 -E "FAIL " lab-out/ctest.txt | head -80
