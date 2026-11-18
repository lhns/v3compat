#!/bin/sh
# iforms.sh MANIFEST: emulated-iforms.txt as regen.sh writes it, from lhns/avxemu's test/spec/manifest.tsv.
echo "# From lhns/avxemu test/spec/manifest.tsv at the pinned commit: forms the expansion stage handles or refuses itself."
tail -n +2 "$1" | cut -f1 | cut -d@ -f1 | LC_ALL=C sort -u
