#!/bin/sh
# vfork_exit_load.sh RUN VFORK_EXIT SECONDS: each way of ending vfork_exit, 8 copies at once for SECONDS in all.
# Load widens two races in the exit's handling of a thread in vfork: a thread entering vfork after the exit
# looked for it, and the child's SIGCHLD reaching that thread once abandoned (docs/plan.md).
RUN=$1; T=$2; SECS=$3; dir=$(mktemp -d); bad=0
end=$(( $(date +%s) + SECS ))
for mode in 0 1 3 4 2; do
    [ "$(date +%s)" -lt "$end" ] || break
    stop=$(( $(date +%s) + SECS / 5 ))
    k=0
    while [ "$k" -lt 8 ]; do k=$((k+1))
        ( n=0; while [ "$(date +%s)" -lt "$stop" ]; do n=$((n+1))
              VFORK_EXIT_MODE=$mode $RUN -- "$T" 5 > "$dir/$mode.$k.out" 2>&1 || { echo "mode $mode copy $k run $n:"; cat "$dir/$mode.$k.out"; }
          done ) > "$dir/fail.$mode.$k" &
    done
    wait
done
cat "$dir"/fail.* > "$dir/all"; [ -s "$dir/all" ] && { head -40 "$dir/all"; bad=1; }
rm -rf "$dir"
[ "$bad" = 0 ] && echo PASS
exit $bad
