#!/bin/bash
# perfdiff.sh N: regstress sig jit profiled on EPYC 9V74, the old base against the same code with 16 bytes of padding.
set -u
grep -m1 'model name' /proc/cpuinfo; grep -q 9V74 /proc/cpuinfo || { echo 'RESULT skip: not 9V74'; exit 0; }
sudo sysctl -q kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0
which perf >/dev/null 2>&1 || sudo apt-get install -y -qq linux-tools-common linux-tools-$(uname -r) >/dev/null 2>&1
declare -A C=([base]=0bd5cd82939e36c213cb19c2ab2b051b1080ffb9 [p16]=471ff6e18d6a7105db4e9bc4da88a72c0bd0985f)
for v in base p16; do
  rm -rf /tmp/b-$v && mkdir /tmp/b-$v && git archive HEAD | tar -x -C /tmp/b-$v
  sed -i "s/^set(V3COMPAT_DR_COMMIT .*/set(V3COMPAT_DR_COMMIT ${C[$v]})/" /tmp/b-$v/cmake/FetchDynamoRIO.cmake
  (cmake -S /tmp/b-$v -B /tmp/b-$v/build >/dev/null && cmake --build /tmp/b-$v/build -j >/dev/null) || { echo "RESULT build $v failed"; exit 1; }
  perf buildid-cache -a $HOME/.cache/v3compat/dr-${C[$v]}/lib64/release/libdynamorio.so.debug 2>/dev/null
done
for i in $(seq $1); do for v in base p16; do
  s=$(date +%s.%N); perf record -q -e cpu-clock -F 499 -o lab-out/perf.$v.$i -- /tmp/b-$v/build/inst/bin/v3compat-run -- /tmp/b-$v/build/test/regstress 8 10 sig jit >/dev/null 2>&1
  echo "RUN $v $i wall=$(echo "$(date +%s.%N) - $s" | bc)"
done; done
for v in base p16; do
  echo "PROFILE $v"; perf report -q -i lab-out/perf.$v.1 --sort dso,sym --percent-limit 0.7 2>/dev/null | head -40 | sed "s/^/PROF $v /"
done
