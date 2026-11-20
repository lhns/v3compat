#!/bin/bash
# startup.sh REPS [WORKLOADS]: where startup goes (translation cache design). A ladder native / DynamoRIO alone /
# empty client / the client / the client without traces, interleaved per rep; then block counts and untimed perf
# profiles.
set -u
cd "$(dirname "$0")/.." || exit 1  # the repo root, where build/ and ./duckdb are
REPS=${1:-10}
grep -m1 'model name' /proc/cpuinfo; nproc
DR=$HOME/.cache/v3compat/dr-$(sed -n 's/^set(V3COMPAT_DR_COMMIT \([0-9a-f]*\))/\1/p' cmake/FetchDynamoRIO.cmake)/
for c in nullclient countclient; do
  cmake -S lab/$c -B /tmp/$c-b -DDynamoRIO_DIR=${DR}cmake >/dev/null && cmake --build /tmp/$c-b >/dev/null || exit 1
done
RUN=$PWD/build/inst/bin/v3compat-run
DRO="-no_enable_reset -no_ignore_syscalls"
declare -A W=([hello-static]="build/test/hello_static" [hello-dyn]="build/test/hello_dyn"
              [duck42]="./duckdb -csv -noheader -c 'select 42'"
              [duckcomp]="./duckdb -csv -noheader -c 'select sum(range*range) from range(30000000)'"
              [node0]="node -e 0")
declare -A C=([native]="" [dr]="${DR}bin64/drrun $DRO --" [null]="${DR}bin64/drrun $DRO -c /tmp/nullclient-b/libnull.so --"
              [client]="$RUN --" [client-nt]="env V3COMPAT_DR_OPTS=-disable_traces $RUN --"
              [dr-nt]="${DR}bin64/drrun $DRO -disable_traces --" [null-nt]="${DR}bin64/drrun $DRO -disable_traces -c /tmp/nullclient-b/libnull.so --")
order=${2:-"hello-static hello-dyn duck42 duckcomp node0"}
for r in $(seq $REPS); do
  for w in $order; do for c in native dr dr-nt null null-nt client client-nt; do
    eval "/usr/bin/time -f '%e %U %S %F %R' -o /tmp/t timeout -k 10 300 ${C[$c]} ${W[$w]}" >/dev/null 2>/tmp/e; rc=$?
    echo "RESULT $w $c rep=$r rc=$rc wall,user,sys,majflt,minflt=$(tail -1 /tmp/t)"
  done; done
done
# Counts (untimed): blocks built in file-backed modules against other memory, traces.
for w in $order; do
  eval "timeout -k 10 300 ${DR}bin64/drrun $DRO -c /tmp/countclient-b/libcount.so -- ${W[$w]}" >/dev/null 2>/tmp/e
  echo "RESULT count $w $(grep COUNT /tmp/e)"
  eval "timeout -k 10 300 ${DR}bin64/drrun $DRO -disable_traces -c /tmp/countclient-b/libcount.so -- ${W[$w]}" >/dev/null 2>/tmp/e
  echo "RESULT count-nt $w $(grep COUNT /tmp/e)"
done
# Profiles (untimed); PROFILE=0 skips them (no perf or sudo, as in a pod).
[ "${PROFILE:-1}" = 0 ] && exit 0
sudo apt-get install -y -qq linux-tools-common linux-tools-$(uname -r) >/dev/null 2>&1 || sudo apt-get install -y -qq linux-tools-generic >/dev/null 2>&1
sudo sysctl -q kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0
PERF=$(ls /usr/lib/linux-tools/*/perf 2>/dev/null | tail -1); PERF=${PERF:-perf}
$PERF buildid-cache -a ${DR}lib64/release/libdynamorio.so.debug 2>/dev/null
mkdir -p lab-out/perf
for w in duck42 node0 hello-dyn; do for c in dr client; do
  eval "$PERF record -q -F 20000 -e cpu-clock -o /tmp/p.data -- ${C[$c]} ${W[$w]}" >/dev/null 2>/dev/null
  $PERF report -i /tmp/p.data --stdio --no-children --sort dso,sym 2>/dev/null > lab-out/perf/$w-$c.txt
  $PERF report -i /tmp/p.data --stdio --no-children --sort dso 2>/dev/null | grep -E '^ +[0-9]' | head -15 | sed "s/^/RESULT perf-dso $w $c /"
done; done
# Inclusive time (untimed, call graphs): block build, trace build, dispatch.
for w in duck42 node0; do for c in dr client client-nt; do
  eval "$PERF record -q -F 2000 -e cpu-clock --call-graph dwarf,16384 -o /tmp/g.data -- ${C[$c]} ${W[$w]}" >/dev/null 2>/dev/null
  $PERF report -i /tmp/g.data --stdio --children --sort sym -g none 2>/dev/null | grep -E '^ +[0-9]' | head -80 > lab-out/perf/$w-$c-children.txt
  grep -E ' (build_bb_ilist|build_basic_block_fragment|internal_extend_trace|monitor_cache_exit|end_and_emit_trace|d_r_dispatch|dispatch_enter_dynamorio|emit_fragment_ex|instrument_basic_block|mangle_bb_ilist|d_r_mangle|fragment_add|link_new_fragment|recreate_app_state|vm_area_add_to_list|check_thread_vm_area)$' lab-out/perf/$w-$c-children.txt | sed "s/^/RESULT incl $w $c /"
done; done
