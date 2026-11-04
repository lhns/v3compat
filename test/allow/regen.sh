#!/bin/sh
# regen.sh XED_INSTALL BUILD_DIR [AVX2_PROBE.tsv]: rebuild test/allow/evidence on Ivy Bridge, then the list.
#   XED_INSTALL  an install of Intel XED, the same one lhns/avxemu's manifest is generated with.
#   BUILD_DIR    a built v3compat tree, for tools/allow_dkey.
#   AVX2_PROBE   the `allow-probe` artifact of the CI job for these rows; omit to keep the committed file.
# Run on the target CPU: results are evidence about the machine they ran on. Refuses to record anything else.
set -eu
here=$(cd "$(dirname "$0")" && pwd); ev=$here/evidence; xed=$1; b=$2; t=$(mktemp -d)
mkdir -p "$ev"
grep -q 'model\s*: 58$' /proc/cpuinfo || { echo "not Ivy Bridge (model 58): results would be mislabelled" >&2; exit 1; }
gcc -O2 -I"$xed/include/xed" -I"$xed/include" -I"$here" "$here/sweep.c" "$xed/lib/libxed.a" -o "$t/sweep"
gcc -O1 -static -fno-stack-protector -I"$here" "$here/probe.c" "$here/probe_stub.S" -o "$t/probe"
# Every form avxemu's suite accounts for, tested or refused: the expansion stage handles or refuses these itself.
{ echo "# From lhns/avxemu test/spec/manifest.tsv at the pinned commit: forms the expansion stage handles or refuses itself."
  tail -n +2 "$b/_deps/avxemu-src/test/spec/manifest.tsv" | cut -f1 | cut -d@ -f1 | LC_ALL=C sort -u; } > "$here/emulated-iforms.txt"
"$t/sweep" > "$t/rows.tsv"
"$t/sweep" iforms > "$t/iforms.tsv"
"$b/tools/allow_dkey" < "$t/rows.tsv" > "$t/keys.tsv"
"$t/probe" < "$t/rows.tsv" > "$t/ivb.tsv"
gzip -9nc "$t/rows.tsv" > "$ev/rows.tsv.gz"
gzip -9nc "$t/iforms.tsv" > "$ev/iforms.tsv.gz"
gzip -9nc "$t/keys.tsv" > "$ev/keys.tsv.gz"
gzip -9nc "$t/ivb.tsv" > "$ev/ivybridge.tsv.gz"
[ -n "${3:-}" ] && gzip -9nc "$3" > "$ev/avx2.tsv.gz"
python3 "$here/gen.py" build
rm -rf "$t"
