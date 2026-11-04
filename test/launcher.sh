#!/bin/sh
# launcher.sh INST HELLO BUILD: how the launcher fails. Status, nothing on stdout, a one-line message on stderr.
INST=$1; HELLO=$2; B=$3
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
bad=0
# check NAME WANT-STATUS [stderr-regex] -- cmd...: stdout must stay empty.
check() {
  name=$1; want=$2; re=$3; shift 4
  out=$("$@" 2>"$t/err" </dev/null); st=$?
  err=$(cat "$t/err")
  ok=1
  [ "$st" = "$want" ] || ok=0
  [ -z "$out" ] || ok=0
  if [ -n "$re" ]; then echo "$err" | grep -q "$re" || ok=0; fi
  [ "$(echo "$err" | wc -l)" -le 1 ] || ok=0
  [ "$ok" = 1 ] && echo "ok $name" || { echo "FAIL $name: status $st (want $want) stdout '$out' stderr '$err'"; bad=1; }
}

# An install without libdynamorio, and one under a path with a space.
cp -r "$INST" "$t/broken"
rm "$t/broken/libexec/v3compat/dynamorio/lib64/release/libdynamorio.so"
check no-dynamorio 125 'v3compat-run: ' -- "$t/broken/bin/v3compat-run" -- "$HELLO"
cp -r "$INST" "$t/broken2"; rm "$t/broken2/libexec/v3compat/libv3compat.so"
check no-client 125 'v3compat-run: ' -- "$t/broken2/bin/v3compat-run" -- "$HELLO"
mkdir "$t/with space"; cp -r "$INST" "$t/with space/inst"
check space-in-path 125 'space' -- "$t/with space/inst/bin/v3compat-run" -- "$HELLO"
check usage 125 'usage' -- "$INST/bin/v3compat-run"
check missing 127 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t/nonexistent"
check missing-path 127 'not found' -- "$INST/bin/v3compat-run" nonexistent-prog-xyz
printf 'text\n' > "$t/plain"; chmod 644 "$t/plain"
check not-executable 126 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t/plain"
chmod 755 "$t/plain"
check garbage 126 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t/plain"
check directory 126 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t"
printf '#!%s/nonexistent-interp\n' "$t" > "$t/bad-interp"; chmod 755 "$t/bad-interp"
check missing-interpreter 126 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t/bad-interp"
# A dynamic program whose ELF interpreter is gone must fail up front, not abort inside DynamoRIO.
cp "$HELLO" "$t/nointerp"; chmod 755 "$t/nointerp"
python3 - "$t/nointerp" <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
i = d.index(b'/lib64/ld-linux-x86-64.so.2')
d[i:i + 11] = b'/lib64/nope'
open(p, 'wb').write(d)
PY
check elf-interpreter-gone 126 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t/nointerp"

# 32-bit and foreign ELF headers: refused, or failed up front, never an abort inside DynamoRIO.
cp "$HELLO" "$t/elf32"; printf '\001' | dd of="$t/elf32" bs=1 seek=4 conv=notrunc 2>/dev/null; chmod 755 "$t/elf32"
check elf32-refused 87 'refusing to exec.*32-bit' -- "$INST/bin/v3compat-run" -- "$t/elf32"
cp "$HELLO" "$t/arm"; printf '\267' | dd of="$t/arm" bs=1 seek=18 conv=notrunc 2>/dev/null; chmod 755 "$t/arm"
check foreign-machine 126 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t/arm"
head -c 100 "$HELLO" > "$t/trunc"; chmod 755 "$t/trunc"
check truncated-elf 126 'v3compat-run: ' -- "$INST/bin/v3compat-run" -- "$t/trunc"
# The same files exec'd by a program under the client: refused, or the exec fails and the program goes on.
out=$("$INST/bin/v3compat-run" -- "$B/execer_static" plain "$t/elf32" 2>&1); st=$?
[ "$st" = 1 ] && echo "$out" | grep -q 'refusing to exec.*32-bit' && echo "$out" | grep -q '^FAIL exec plain: Exec format error' && echo "ok exec of 32-bit fails with ENOEXEC and a message" || { echo "FAIL exec of 32-bit: $st '$out'"; bad=1; }
out=$("$INST/bin/v3compat-run" -- "$B/execer_static" regs "$t/elf32" 2>/dev/null); st=$?
[ "$st" = 0 ] && [ "$out" = PASS ] && echo "ok a refused exec leaves the registers as a failed one" || { echo "FAIL refused exec registers: $st '$out'"; bad=1; }
for f in arm trunc nointerp; do
  out=$("$INST/bin/v3compat-run" -- "$B/execer_static" plain "$t/$f" 2>&1); st=$?
  [ "$st" = 1 ] && echo "$out" | grep -q '^FAIL exec plain: ' && echo "ok exec of $f fails, the program goes on" || { echo "FAIL exec of $f: $st '$out'"; bad=1; }
done

# ENOEXEC sends glibc's execvp to /bin/sh, which must fail on the file rather than run it.
mkdir "$t/pe"; cp "$t/elf32" "$t/pe/elf32"
out=$(PATH="$t/pe:$PATH" "$INST/bin/v3compat-run" -- "$B/execer_dyn" execvp elf32 2>&1); st=$?
[ "$st" != 0 ] && echo "$out" | grep -q 'refusing to exec.*32-bit' && ! echo "$out" | grep -q '^hello' && echo "ok execvp of 32-bit: the shell fallback fails too" || { echo "FAIL execvp of 32-bit: $st '$out'"; bad=1; }

# V3COMPAT_NATIVE=1 skips every check, also in an install that is broken.
out=$(V3COMPAT_NATIVE=1 "$t/broken/bin/v3compat-run" -- "$HELLO" 2>&1); st=$?
[ "$st" = 0 ] && [ "$out" = "hello" ] && echo "ok native-skips-checks" || { echo "FAIL native-skips-checks: $st '$out'"; bad=1; }
V3COMPAT_NATIVE=1 "$INST/bin/v3compat-run" nonexistent-prog-xyz 2>/dev/null; st=$?
[ "$st" = 127 ] && echo "ok native-missing" || { echo "FAIL native-missing: $st"; bad=1; }

# The app's environment: ours replace any DYNAMORIO_ variables the caller had, and the rest is kept.
out=$(DYNAMORIO_OPTIONS=-bogus DYNAMORIO_RUNUNDER=0 KEEP=me "$INST/bin/v3compat-run" -- /usr/bin/env 2>&1)
echo "$out" | grep -q '^KEEP=me$' && ! echo "$out" | grep -q bogus && echo "ok env-replaced" || { echo "FAIL env-replaced"; bad=1; }
# An environment far bigger than a page.
big=$(head -c 100000 /dev/zero | tr '\0' x)
out=$(BIG=$big "$INST/bin/v3compat-run" -- /usr/bin/env 2>&1 | grep -c '^BIG=')
[ "$out" = 1 ] && echo "ok big-env" || { echo "FAIL big-env"; bad=1; }
# Too many options for DynamoRIO.
check options-too-long 125 'too long' -- env "V3COMPAT_DR_OPTS=$(head -c 3000 /dev/zero | tr '\0' x)" "$INST/bin/v3compat-run" -- "$HELLO"
check options-way-too-long 125 'too long' -- env "V3COMPAT_DR_OPTS=$(head -c 20000 /dev/zero | tr '\0' x)" "$INST/bin/v3compat-run" -- "$HELLO"
exit $bad
