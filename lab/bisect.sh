#!/bin/bash
# bisect.sh N: `regstress 8 10 sig jit` under the client at four fork commits, interleaved: wall time and status.
set -u
grep -m1 'model name' /proc/cpuinfo
declare -A C=([base]=0bd5cd82939e36c213cb19c2ab2b051b1080ffb9 [main]=ebffb4a5e1f0486649bbae745a0542c448bf8ddb
              [fix]=f035adb1c4ccd61b93e2b10ade692691f84f5b7a [count]=76bda2e7b56b45e5522e0c60a82666078df76f57)
for v in base main fix count; do
  rm -rf /tmp/b-$v && mkdir /tmp/b-$v && git archive HEAD | tar -x -C /tmp/b-$v
  sed -i "s/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT ${C[$v]})/" /tmp/b-$v/cmake/FetchDynamoRIO.cmake
  (cmake -S /tmp/b-$v -B /tmp/b-$v/build >/dev/null && cmake --build /tmp/b-$v/build -j >/dev/null) || { echo "RESULT build $v failed"; exit 1; }
done
for i in $(seq $1); do for v in base main fix count; do
  s=$(date +%s.%N); timeout 120 /tmp/b-$v/build/inst/bin/v3compat-run -- /tmp/b-$v/build/test/regstress 8 10 sig jit >/tmp/o 2>/tmp/e; rc=$?
  t=$(echo "$(date +%s.%N) - $s" | bc); echo "RUN $v $i rc=$rc wall=$t $(grep V3LK /tmp/e | tail -1)"
done; done
for v in base main fix count; do grep "^RUN $v " lab-out/log.txt 2>/dev/null >/dev/null; done
