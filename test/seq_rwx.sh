#!/bin/sh
# seq_rwx.sh RUN PROG SEED COUNT [DR options]: test/seq_rwx.c's digests under the client against native.
RUN=$1; PROG=$2; SEED=$3; COUNT=$4; shift 4
"$PROG" "$SEED" "$COUNT" > native.$$ || { rm -f native.$$; echo "native run failed"; exit 1; }
V3COMPAT_DR_OPTS="$*" "$RUN" -- "$PROG" "$SEED" "$COUNT" > client.$$; rc=$?
d=$(diff native.$$ client.$$ | head -5); rm -f native.$$ client.$$
[ $rc = 0 ] || { echo "client run: status $rc"; exit 1; }
[ -z "$d" ] || { echo "client differs from native:"; echo "$d"; exit 1; }
echo PASS
