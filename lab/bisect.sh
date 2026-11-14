#!/bin/bash
# bisect.sh N: `regstress 8 10 sig jit` under the client at four fork commits, interleaved: wall time and status.
set -u
grep -m1 'model name' /proc/cpuinfo; grep -q 9V74 /proc/cpuinfo || { echo 'RESULT skip: not 9V74'; exit 0; }
declare -A C=([base]=0bd5cd82939e36c213cb19c2ab2b051b1080ffb9 [main]=ebffb4a5e1f0486649bbae745a0542c448bf8ddb
              [align]=298795a411c8298416a0e735479d61a394321b74 [p16]=471ff6e18d6a7105db4e9bc4da88a72c0bd0985f
              [p16a]=e84c4313e6d22a96bd3b99bee98ee7acb3b7ec3e)
for v in base main align p16 p16a; do
  rm -rf /tmp/b-$v && mkdir /tmp/b-$v && git archive HEAD | tar -x -C /tmp/b-$v
  sed -i "s/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT ${C[$v]})/" /tmp/b-$v/cmake/FetchDynamoRIO.cmake
  (cmake -S /tmp/b-$v -B /tmp/b-$v/build >/dev/null && cmake --build /tmp/b-$v/build -j >/dev/null) || { echo "RESULT build $v failed"; exit 1; }
done
for i in $(seq $1); do for v in base main align p16 p16a; do
  s=$(date +%s.%N); timeout 120 /tmp/b-$v/build/inst/bin/v3compat-run -- /tmp/b-$v/build/test/regstress 8 10 sig jit >/tmp/o 2>/tmp/e; rc=$?
  t=$(echo "$(date +%s.%N) - $s" | bc); echo "RUN $v $i rc=$rc wall=$t $(grep V3LK /tmp/e | tail -1)"
done; done
for v in base main align p16 p16a; do grep "^RUN $v " lab-out/log.txt 2>/dev/null >/dev/null; done
