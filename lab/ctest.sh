#!/bin/bash
# ctest.sh: the whole suite with the semantic suite on (semantic-hw), as CI runs it. RESULT lines: summary, failed tests.
grep -m1 'model name' /proc/cpuinfo
chmod o+x "$HOME"
cmake -S . -B build -DV3COMPAT_SEMANTIC=ON > lab-out/cfg.log 2>&1 && cmake --build build -j"$(nproc)" > lab-out/build2.log 2>&1 || { tail -20 lab-out/build2.log; echo "RESULT ctest: build failed"; exit 1; }
V3COMPAT_CI=1 ctest --test-dir build --output-on-failure > lab-out/ctest.log 2>&1
echo "ctest rc=$?"
grep -E 'tests passed|tests failed' lab-out/ctest.log | sed 's/^/RESULT ctest: /'
grep -E 'Failed|\*\*\*|Not Run' lab-out/ctest.log | head -20 | sed 's/^/RESULT ctest failed: /'
grep -E 'semantic-hw' lab-out/ctest.log | sed 's/^/RESULT /'
