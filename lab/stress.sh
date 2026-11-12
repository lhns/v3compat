#!/bin/bash
# Revalidation of the fork chain rebased onto upstream master. Args: kind runs [extra]
. lab/loop.sh
grep -m1 "model name" /proc/cpuinfo
RUN="build/inst/bin/v3compat-run --"
DRRUN="$(ls -d ~/.cache/v3compat/dr-*/)bin64/drrun"
RS=build/test/regstress
mkdir -p lab-out/core; sudo sysctl -q kernel.core_pattern="$PWD/lab-out/core/core.%p"; ulimit -c unlimited
case "$1" in
# The stress set (docs/plan.md, "No change elsewhere").
sig) loop sig $2 60 $RUN $RS 8 10 sig jit ;;
regc) for k in "jit" "sig" "idle jit"; do loop regc "$2" 60 $RUN $RS 8 10 $k; done ;;
regd) for k in "jit" "sig" "sig jit"; do loop regd "$2" 60 $DRRUN -no_enable_reset -no_ignore_syscalls -- $RS 8 10 $k; done ;;
short) loop short $2 60 $RUN $RS 8 1 sig jit ;;
duck)
  img='duckdb/duckdb:2.0.0@sha256:453caf4f1c445ee8363acaa6cd383751396d2eae0d02f362ada64d87267a4221'
  docker pull -q "$img" >/dev/null; c=$(docker create "$img"); docker cp "$c:/duckdb" ./duckdb 2>/dev/null; docker rm "$c" >/dev/null
  sh test/apps/duckdb.sh "" ./duckdb > lab-out/native.txt
  sh test/apps/duckdb.sh "build/inst/bin/v3compat-run --" ./duckdb $2 > lab-out/client.txt 2>lab-out/client.err </dev/null; echo "duckdb.sh rc=$?"
  diff <(sed 's/ wall.*//' lab-out/native.txt | sort) <(sed 's/ wall.*//' lab-out/client.txt | sort -u) | head -20; echo "RESULT duck: diff rc=${PIPESTATUS[0]}, runs $2"; head -5 lab-out/client.err ;;
# Torn exit: 9V45 only.
segv)
  grep -m1 "model name" /proc/cpuinfo | grep -q 9V45 || { echo "RESULT skip: not 9V45"; exit 0; }
  loop segv $2 60 $RUN $RS 8 1 jit
  for c in lab-out/core/core.*; do [ -f "$c" ] || continue; gdb -q -batch -c "$c" $RS -ex 'info registers' -ex 'x/8i $pc' > "$c.txt" 2>&1; zstd -q -T0 --rm "$c"; done
  ls -d lab-out/core/*.zst 2>/dev/null | tail -n +5 | xargs -r rm -f ;;
# A process ending while a thread waits in vfork.
exit) loop exitnative $2 300 build/test/vfork_exit_static 20; loop exit $2 300 $RUN build/test/vfork_exit_dyn 20
      loop exitdr $2 300 $DRRUN -- build/test/vfork_exit_dyn 20; grep -hE "FAIL|PASS" lab-out/exit.out lab-out/exitdr.out | sort | uniq -c ;;
# Thread creation failing or churning at exit.
cap) for w in native client dr; do
       case $w in native) p="";; client) p="$RUN";; dr) p="$DRRUN --";; esac
       loop cap$w $2 120 $p build/test/thread_exit 64 300; loop nproc$w $2 120 $p build/test/thread_exit 64 300 nproc
       loop vfork$w $2 120 $p build/test/thread_exit 64 300 vfork
       for m in cap nproc vfork; do echo "$m$w: $(tail -1 lab-out/$m$w.out)"; done
     done ;;
bigclient) loop bigclient $2 120 $RUN build/test/thread_exit 1000 2000; echo "bigclient: $(tail -1 lab-out/bigclient.out)" ;;
# Signals sent with kill inside expansions.
xl)
  $RS 8 3 xlsig > lab-out/native.txt; echo "native xlsig: $(tail -1 lab-out/native.txt)"
  loop xlsig $2 60 $RUN $RS 8 3 xlsig; echo "last: $(tail -1 lab-out/xlsig.out)"
  loop xlsigjit $2 60 $RUN $RS 8 3 xlsig jit; echo "last: $(tail -1 lab-out/xlsigjit.out)"
  V3COMPAT_HELPERS_ONLY=1 loop xlhelp $2 60 $RUN $RS 8 3 xlsig; echo "last: $(tail -1 lab-out/xlhelp.out)"
  loop xldr $2 60 $DRRUN -no_enable_reset -no_ignore_syscalls -- $RS 8 3 xlsig; echo "last: $(tail -1 lab-out/xldr.out)"
  loop killed 20 30 $RUN build/test/killed; echo "killed: $(tail -1 lab-out/killed.out)" ;;
esac
for f in lab-out/*.fail1.err; do [ -f "$f" ] && { echo "== $f"; grep -a '@@' $f | head -20; }; done
ls lab-out/core | wc -l
[ "$1" = segv ] || rm -f lab-out/core/core.*
