#!/bin/bash
# bisect.sh N: `regstress 8 10 sig jit` under the client at four fork commits, interleaved: wall time and status.
set -u
grep -m1 'model name' /proc/cpuinfo
declare -A C=([base]=0bd5cd82939e36c213cb19c2ab2b051b1080ffb9 [ibl]=399c1a10b497805743a9d5f0ff2de6f9db01a5b4
              [emit]=9fe242500b686770e2dc7707f8cdbf360c5632a8 [both]=ebffb4a5e1f0486649bbae745a0542c448bf8ddb)
for v in base ibl emit both; do
  rm -rf /tmp/b-$v && mkdir /tmp/b-$v && git archive HEAD | tar -x -C /tmp/b-$v
  sed -i "s/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT ${C[$v]})/" /tmp/b-$v/cmake/FetchDynamoRIO.cmake
  (cmake -S /tmp/b-$v -B /tmp/b-$v/build >/dev/null && cmake --build /tmp/b-$v/build -j >/dev/null) || { echo "RESULT build $v failed"; exit 1; }
done
for i in $(seq $1); do for v in base ibl emit both; do
  s=$(date +%s.%N); timeout 120 /tmp/b-$v/build/inst/bin/v3compat-run -- /tmp/b-$v/build/test/regstress 8 10 sig jit >/tmp/o 2>/tmp/e; rc=$?
  t=$(echo "$(date +%s.%N) - $s" | bc); echo "RUN $v $i rc=$rc wall=$t"
done; done
for v in base ibl emit both; do grep "^RUN $v " lab-out/log.txt 2>/dev/null >/dev/null; done
