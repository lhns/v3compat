#!/bin/bash
# fmaab.sh REPS: bench/fma under base (/tmp/base build, origin/main) and new (./build), interleaved per rep
# (the order alternates), on one runner. RESULT lines: median ns/op per micro row, median seconds per kernel,
# and whether every kernel checksum equals a native run's. Needs /tmp/bench/fma (the lab workflow builds it).
set -u
REPS=${1:-8}
F=/tmp/bench/fma
declare -A RUN=([base]=/tmp/base/build/inst/bin/v3compat-run [new]=$PWD/build/inst/bin/v3compat-run)
O=/tmp/fmaab; rm -rf $O; mkdir -p $O
echo "model name $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"
nproc
$F 1 kernels > $O/native.txt 2>&1; echo "native rc=$?"
for r in $(seq "$REPS"); do
  if [ $((r % 2)) = 1 ]; then order="base new"; else order="new base"; fi
  for c in $order; do
    timeout -k 10 600 ${RUN[$c]} -- $F 1 > $O/$c.$r.txt 2> $O/$c.$r.err; echo "run $c rep=$r rc=$?"
  done
done
python3 - "$O" "$REPS" <<'PY'
import re, statistics, sys
o, reps = sys.argv[1], int(sys.argv[2])
rows = {}
sums = {}
def parse(path):
    micro, kern = {}, {}
    try: lines = open(path).read().splitlines()
    except OSError: return micro, kern
    for l in lines:
        m = re.match(r"micro (\S+)\s+([\d.]+) ns/op", l)
        if m: micro[m.group(1)] = float(m.group(2))
        m = re.match(r"kernel (\S+)\s+([\d.]+) s\s+sum ([0-9a-f]+)", l)
        if m: kern[m.group(1)] = (float(m.group(2)), m.group(3))
    return micro, kern
_, nat = parse(o + "/native.txt")
data = {"base": {}, "new": {}}
okc = {"base": 0, "new": 0}
for c in data:
    for r in range(1, reps + 1):
        mi, ke = parse(f"{o}/{c}.{r}.txt")
        if not mi or len(ke) < 3: continue
        okc[c] += 1
        for k, v in mi.items(): data[c].setdefault(("micro", k), []).append(v)
        for k, (t, s) in ke.items():
            data[c].setdefault(("kernel", k), []).append(t)
            sums.setdefault((c, k), []).append(s == nat.get(k, (0, None))[1])
for key in sorted(data["base"]):
    b = data["base"][key]; n = data["new"].get(key, [])
    if not n: continue
    bm, nm = statistics.median(b), statistics.median(n)
    extra = ""
    if key[0] == "kernel":
        extra = " sums_equal_native base=%d/%d new=%d/%d" % (sum(sums[("base", key[1])]), len(sums[("base", key[1])]),
                                                           sum(sums[("new", key[1])]), len(sums[("new", key[1])]))
    unit = "ns/op" if key[0] == "micro" else "s"
    print(f"RESULT {key[0]} {key[1]} base={bm:.3f} new={nm:.3f} {unit} new/base={nm / bm:.3f} n={len(b)}/{len(n)}{extra}")
print(f"RESULT runs ok base={okc['base']}/{reps} new={okc['new']}/{reps}")
PY
