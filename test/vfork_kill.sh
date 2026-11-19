#!/bin/sh
# vfork_kill.sh RUN VFORK_KILL KILLS: vfork children SIGKILLed at random points, then at once. A child killed holding a
# lock of DynamoRIO's ends the process with status 89 and says so (docs/plan.md): KILLS-kill runs may end that way, and
# anything else but PASS fails. Most children die holding none, so a 3-kill run normally PASSes and shows the parent
# goes on after a kill; up to five are tried. All five refused is logged, not failed: on some CPUs whole script runs do
# that (about 1 in 19 on Xeon 6975P-C), and a refusal is no hang or crash. A hang is ctest's timeout.
RUN=$1; T=$2; K=$3; err=$(mktemp); bad=0
one() {  # one LABEL KILLS [zero]: sets r to pass, refused or bad
    out=$($RUN -- "$T" "$2" $3 2>"$err"); rc=$?
    if [ $rc = 0 ] && [ "$(echo "$out" | tail -1)" = PASS ]; then r=pass
    elif [ $rc = 89 ] && grep -q 'killed holding a DynamoRIO lock' "$err"; then r=refused
    else r=bad; echo "$1: rc=$rc"; echo "$out" | tail -3; head -c 2000 "$err"; fi
    echo "$1: $r"
}
passed=0
for i in 1 2 3 4 5; do
    one "3 kills, run $i" 3; [ $r = bad ] && bad=1; [ $r = pass ] && { passed=1; break; }
done
[ $passed = 1 ] || echo "no 3-kill run passed, all refused"
one "$K kills at random points" "$K"; [ $r = bad ] && bad=1
one "$K kills at once" "$K" zero; [ $r = bad ] && bad=1
rm -f "$err"
[ "$bad" = 0 ] && echo PASS
exit $bad
