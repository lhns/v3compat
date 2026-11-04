#!/bin/sh
# docker.sh INST BUILD: the launcher in images with no shell, no /tmp, no HOME and a read-only root,
# as a non-root user under Docker's default seccomp profile. INST is bind-mounted read-only.
# Skips (77) without docker; V3COMPAT_CI=1 makes that a failure.
INST=$1; B=$2
# Where the install appears in the container; a named volume instead of a path works on a remote daemon.
MOUNT=${V3COMPAT_TEST_MOUNT:-$INST:/v3compat:ro}
LAUNCHER=${V3COMPAT_TEST_LAUNCHER:-/v3compat/bin/v3compat-run}
skip() { echo "SKIP: $*"; [ "$V3COMPAT_CI" = 1 ] && exit 1; exit 77; }
command -v docker >/dev/null && docker info >/dev/null 2>&1 || skip "no docker"
id=v3compat-test-$$
t=$(mktemp -d)
cleanup() { docker rmi -f "$id-static" "$id-dyn" >/dev/null 2>&1; rm -rf "$t"; }
trap cleanup EXIT
bad=0

# Static programs only, then the same plus the dynamic loader and libc. The modes are set in the tar, so
# this also works where chmod does not (the setuid-root file is appended with its own mode).
mkdir -p "$t/s/t" "$t/u/t" "$t/d/t"
for f in lz_static execer_static ids_static; do cp "$B/$f" "$t/s/t/"; done
cp "$B/ids_static" "$t/u/t/ids_suid"
cp "$B/lz_dyn" "$t/d/t/"
if [ -n "$V3COMPAT_TEST_LIBROOT" ]; then # a prepared tree of the loader and libc, when ldd is not here
  cp -r "$V3COMPAT_TEST_LIBROOT/." "$t/d/"
else
  for lib in $(ldd "$B/lz_dyn" | sed -n 's|.*=> \(/[^ ]*\) .*|\1|p; s|^[[:space:]]*\(/[^ ]*\) (.*|\1|p'); do
    mkdir -p "$t/d$(dirname "$lib")"; cp -L "$lib" "$t/d$lib"
  done
fi
cp -r "$t/s/t/." "$t/d/t/"
own="--owner=0 --group=0 --numeric-owner"
for k in s d; do
  tar -C "$t/$k" $own --mode=755 -cf "$t/$k.tar" . || skip "tar"
  tar -C "$t/u" $own --mode=4755 -rf "$t/$k.tar" t/ids_suid || skip "tar"
done
docker import - "$id-static" < "$t/s.tar" >/dev/null || skip "docker import failed"
docker import - "$id-dyn" < "$t/d.tar" >/dev/null || skip "docker import failed"

expect() { # expect NAME STATUS REGEX
  if [ "$st" = "$2" ] && echo "$out" | grep -q "$3"; then echo "ok $1"; else echo "FAIL $1: status $st stdout '$out' stderr '$err'"; bad=1; fi
}

# r IMAGE [docker args...] -- PROG ARGS: stdout in $out, stderr in $err, status in $st
r() {
  image=$1; shift
  dargs=
  while [ "$1" != -- ]; do dargs="$dargs $1"; shift; done; shift
  out=$(docker run --rm --read-only --user 65534 --entrypoint "$LAUNCHER" \
        -v "$MOUNT" $dargs "$image" -- "$@" 2>"$t/err" </dev/null); st=$?
  err=$(cat "$t/err")
}

r "$id-static" -- /t/lz_static translated
expect "static program, translated" 0 PASS
r "$id-static" -- /t/execer_static plain /t/lz_static
expect "static execs static, translated" 0 PASS
r "$id-static" -- /t/execer_static execveat-path /t/lz_static
expect "static execveat static, translated" 0 PASS
r "$id-static" -- /t/execer_static vfork /t/lz_static
expect "static vforks static, translated" 0 PASS
r "$id-static" -e V3COMPAT_NATIVE=1 -- /t/ids_suid
expect "setuid-root file natively gives euid 0 (the case is real)" 0 'euid=0'
r "$id-static" -- /t/ids_suid
if [ "$st" = 87 ] && [ -z "$out" ] && echo "$err" | grep -q 'refusing to exec'; then echo "ok setuid-root file as uid 65534 is refused"; else echo "FAIL setuid: status $st stdout '$out' stderr '$err'"; bad=1; fi
r "$id-static" -- /t/execer_static plain /t/ids_suid
if [ "$st" = 87 ] && echo "$err" | grep -q 'refusing to exec'; then echo "ok setuid-root file exec'd by a program is refused"; else echo "FAIL setuid exec: status $st stdout '$out' stderr '$err'"; bad=1; fi
r "$id-static" -e V3COMPAT_NATIVE=1 -- /t/ids_static translated
expect "native bypass runs without DynamoRIO" 0 "euid=65534 .*translated=0"
r "$id-dyn" -- /t/lz_dyn translated
expect "dynamic glibc program, translated" 0 PASS
r "$id-dyn" -- /t/execer_static plain /t/lz_dyn
expect "static execs dynamic, translated" 0 PASS
exit $bad
