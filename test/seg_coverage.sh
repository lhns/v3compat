#!/bin/sh
# seg_coverage.sh: every emulated form with a memory operand (test/allow/emulated-iforms.txt) needs a row in
# seg_operand.c, F(X, CAT, mnemonic[_variant]) or an FMA3/FMAS(F, X, op) line.
here=$(dirname "$0")
bad=0
for m in $(grep -E '_MEM' "$here/allow/emulated-iforms.txt" | grep -v '^#' | cut -d_ -f1 | sort -u | tr 'A-Z' 'a-z'); do
  op=$(echo "$m" | sed -n -E 's/^vf([a-z]+)(132|213|231)(ps|pd)$/FMA3 \1/p; s/^vf([a-z]+)(132|213|231)(ss|sd)$/FMAS \1/p')
  if grep -Eq "F\(X, [A-Z0-9_]+, $m(_[a-z0-9]+)?\)" "$here/seg_operand.c" ||
     { [ -n "$op" ] && grep -q "${op% *}(F, X, ${op#* })" "$here/seg_operand.c"; }; then :
  else echo "no fs/gs row for $m"; bad=1; fi
done
[ $bad = 0 ] && echo PASS
exit $bad
