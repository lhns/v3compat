#!/bin/bash
# vfx.sh N: vfork_exit_dyn 20 under the client with no cache, in training mode, warm and in verify mode, interleaved,
# N runs each. The warm and verify cache comes from one training run of the same binary.
grep -m1 'model name' /proc/cpuinfo
R=build/inst/bin/v3compat-run; T=build/test/vfork_exit_dyn
rm -rf /tmp/vc /tmp/vt; mkdir -p /tmp/vc /tmp/vt
V3COMPAT_CACHE=/tmp/vc V3COMPAT_CACHE_WRITE=1 $R -- $T 20 > /dev/null 2>&1
ls -la /tmp/vc
declare -A bad=([plain]=0 [train]=0 [warm]=0 [verify]=0)
for i in $(seq $1); do for a in plain train warm verify; do
  case $a in plain) e="" ;; train) e="V3COMPAT_CACHE=/tmp/vt V3COMPAT_CACHE_WRITE=1" ;;
    warm) e="V3COMPAT_CACHE=/tmp/vc" ;; verify) e="V3COMPAT_CACHE=/tmp/vc V3COMPAT_CACHE_VERIFY=1" ;; esac
  timeout 300 env $e $R -- $T 20 > /tmp/o 2>/tmp/e; rc=$?
  [ $rc != 0 ] && { bad[$a]=$((bad[$a]+1)); echo "FAIL $a $i rc=$rc $(grep -m1 -ohE 'multiple native handlers|VERIFY MISMATCH|status 0x[0-9a-f]+' /tmp/e /tmp/o | head -1)"; }
done; done
for a in plain train warm verify; do echo "RESULT vfork_exit_dyn $a: ${bad[$a]} of $1 bad"; done
V3COMPAT_CACHE=/tmp/vc V3COMPAT_CACHE_STATS=1 $R -- $T 20 2>&1 >/dev/null | grep '^pcache: pid' | head -2
