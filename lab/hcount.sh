#!/bin/bash
# LAB ONLY. hcount.sh: which instruction forms reach a helper call. Untimed. Sums the client's HCOUNT lines
# (src/expand.c lab_report) over every pid of a run; also one V3COMPAT_STATS=1 run per workload for executed counts.
set -u
B=/tmp/bench
RUN=$PWD/build/inst/bin/v3compat-run
SQL=$PWD/test/apps/duckdb-varied.sql
OUT=lab-out; mkdir -p $OUT /tmp/hc
grep -m1 'model name' /proc/cpuinfo; nproc
# Output digest as in bench/ab.sh: stdout plus the stderr that is not the client's.
digest() { { grep -vE '^(round|total|micro) ' "$1"; grep -vE '^(HCOUNT|v3compat: )' "$2"; } | sed -E 's/[0-9]+\.[0-9]+ (s|ms)//g' | sha256sum | cut -c1-12; }
W=()
w() { W+=("$1"); eval "CMD_${1//-/_}=\$2"; }   # name, command line
w duckdb-varied "./duckdb -csv -noheader -f $SQL"
w duckdb-compute "./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'"
w node "node bench/jit.js 8 1"
w java "java -XX:UseAVX=2 -cp $B/classes Bench 8 1"
w java-version "java -XX:UseAVX=2 -version"
w javac "javac -J-XX:UseAVX=2 -d /tmp/jo bench/Bench.java"
jdig() { [ -d /tmp/jo ] && (cd /tmp/jo && find . -name '*.class' | sort | xargs sha256sum | sha256sum | cut -c1-12); }
for name in "${W[@]}"; do
  v=CMD_${name//-/_}; cmd=${!v}
  rm -rf /tmp/jo; mkdir -p /tmp/jo
  eval "timeout -k 10 1500 $cmd" >/tmp/hc/n.out 2>/tmp/hc/n.err; nrc=$?
  nd=$(digest /tmp/hc/n.out /tmp/hc/n.err); njd=$(jdig)
  rm -rf /tmp/jo; mkdir -p /tmp/jo
  eval "timeout -k 10 3000 $RUN -- $cmd" >/tmp/hc/c.out 2>/tmp/hc/c.err; crc=$?
  cd_=$(digest /tmp/hc/c.out /tmp/hc/c.err); cjd=$(jdig)
  ok=MATCH; [ "$nd" = "$cd_" ] && [ "$njd" = "$cjd" ] || ok=MISMATCH
  echo "RESULT $name status native_rc=$nrc client_rc=$crc digest_native=$nd digest_client=$cd_ classes_native=$njd classes_client=$cjd $ok"
  cp /tmp/hc/c.err $OUT/stderr-$name.txt; grep -h '^HCOUNT' /tmp/hc/c.err > $OUT/hcount-$name.txt
  echo "RESULT $name pids $(awk '{print $2}' $OUT/hcount-$name.txt | sort -u | wc -l)"
  awk -v w=$name '
    /^HCOUNT/ { kind=""; op=""; type=""; imm=""; fl=""; opc=""; wide=""; mem=""; c=0
      for (i = 2; i <= NF; i++) { split($i, a, "="); if (a[1]=="kind") kind=a[2]; else if (a[1]=="op") op=a[2]; else if (a[1]=="type") type=a[2];
        else if (a[1]=="imm") imm=a[2]; else if (a[1]=="flags") fl=a[2]; else if (a[1]=="opc") opc=a[2]; else if (a[1]=="wide") wide=a[2];
        else if (a[1]=="mem") mem=a[2]; else if (a[1]=="count") c=a[2] }
      if (kind=="") next
      sz = (kind=="frame") ? (wide ? "ymm" : "xmm") : (wide ? "64" : "32")
      form = opc "/" sz "/" (mem ? "mem" : "reg") "/op=" op "/type=" type "/imm=" imm "/flags=" fl
      n[kind " " form] += c; tot[kind] += c }
    END { for (k in n) print "RESULT " w " " k " " n[k]; for (k in tot) print "RESULT " w " " k " TOTAL " tot[k] }' $OUT/hcount-$name.txt | sort -k5,5nr -t' ' > /tmp/hc/res.txt
  sort -t' ' -k5,5nr /tmp/hc/res.txt | tee $OUT/result-$name.txt
  eval "V3COMPAT_STATS=1 timeout -k 10 3000 $RUN -- $cmd" >/dev/null 2>/tmp/hc/s.err
  echo "RESULT $name statsrun rc=$?"
  grep -hE '^v3compat: (stats|op) ' /tmp/hc/s.err > $OUT/stats-$name.txt
done
