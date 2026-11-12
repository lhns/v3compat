#!/bin/bash
# leanprobe.sh REPS: where a frame helper's time goes. base = main (DR clean call), new = lean call,
# p1 = DR clean call to an empty function, p2 = no call, p4 = lean without ymm saves (p1 p2 p4 compute wrong results).
set -u
REPS=${1:-5}
grep -m1 'model name' /proc/cpuinfo
for m in 1 2 4; do cmake -S . -B /tmp/p$m -DCMAKE_C_FLAGS=-DV3C_PROBE=$m >/dev/null && cmake --build /tmp/p$m -j >/dev/null || echo "BUILD p$m failed"; done
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run
  [p1]=/tmp/p1/inst/bin/v3compat-run [p2]=/tmp/p2/inst/bin/v3compat-run [p4]=/tmp/p4/inst/bin/v3compat-run)
BB=$PWD/build/test/bench_bmi
for r in $(seq $REPS); do
  for c in base new p1 p2 p4; do
    ${RUN[$c]} -- $BB | grep -E 'vperm|vpaddq|vpbroad' | sed "s/^/RESULT $c rep=$r /"
    V3COMPAT_HELPERS_ONLY=1 ${RUN[$c]} -- $BB | grep -E 'vpaddq' | sed "s/^/RESULT $c-helpers rep=$r /"
    ${RUN[$c]} -- /tmp/bench/fma | grep -E 'micro|kernel' | sed "s/^/RESULT $c rep=$r fma /"
  done
done
/tmp/bench/fma | grep kernel | sed 's/^/RESULT native fma /'
for lib in /tmp/base/build/inst/libexec/v3compat/libv3compat.so build/inst/libexec/v3compat/libv3compat.so; do
  echo "== $lib"
  objdump -d --no-show-raw-insn "$lib" | awk '/^[0-9a-f]+ <(vec_call|lean_entry|pending_operands)[^>]*>:/{p=1; print; next} /^[0-9a-f]+ </{p=0} p'
  echo "RESULT legacy-SSE in vec_call path ($lib): $(objdump -d --no-show-raw-insn "$lib" | awk '/^[0-9a-f]+ <(vec_call|lean_entry|pending_operands)[^>]*>:/{p=1; next} /^[0-9a-f]+ </{p=0} p && $2 !~ /^v/ && /%xmm/' | wc -l)"
done
