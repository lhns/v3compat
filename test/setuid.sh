#!/bin/sh
# setuid.sh INST BUILD: programs that would change privileges when exec'd are refused (status 87), and
# the ones that would not are run. Needs root or `sudo -n`; V3COMPAT_CI=1 makes a missing prerequisite a failure.
INST=$1; B=$2; RUN=$INST/bin/v3compat-run
skip() { echo "SKIP: $*"; [ "$V3COMPAT_CI" = 1 ] && exit 1; exit 77; }
if [ "$(id -u)" = 0 ]; then S=; elif sudo -n true 2>/dev/null; then S="sudo -n"; else skip "needs root or sudo -n"; fi
t=$(mktemp -d "$B/setuid.XXXXXX"); $S chmod 755 "$t"
trap '$S rm -rf "$t"' EXIT
bad=0
# part-skip MSG: one check cannot run here; that is a failure in CI.
part_skip() { echo "SKIPPED check: $*"; [ "$V3COMPAT_CI" = 1 ] && bad=1; }
fail() { echo "FAIL $*"; bad=1; }
ok() { echo "ok $*"; }
ids=$B/ids_static
[ -x "$ids" ] || skip "ids_static not found"
mk() { # mk NAME MODE OWNER:GROUP: a copy of ids
  cp "$ids" "$t/$1"; $S chown "$3" "$t/$1"; $S chmod "$2" "$t/$1"
}
# status MSG-REGEX NAME CMD...: runs as root; stdout in $out, stderr in $err
run() { out=$($S "$@" 2>"$t/err"); st=$?; err=$(cat "$t/err"); }
refused() { # refused NAME CMD...
  name=$1; shift; run "$@"
  if [ "$st" = 87 ] && echo "$err" | grep -q 'v3compat.*refusing to exec'; then ok "$name"; else fail "$name: status $st stdout '$out' stderr '$err'"; fi
}
ran() { # ran NAME WANT-REGEX CMD...
  name=$1; want=$2; shift 2; run "$@"
  if [ "$st" = 0 ] && echo "$out" | grep -q "$want"; then ok "$name"; else fail "$name: status $st stdout '$out' stderr '$err'"; fi
}

mk suid_nobody 4755 nobody:nogroup
# The case must be real: natively, root gains the owner's id.
run env V3COMPAT_NATIVE=1 "$RUN" "$t/suid_nobody"
case $out in *euid=65534*) ok "native setuid changes the user" ;; *) skip "setuid has no effect here (nosuid mount?): $out" ;; esac
refused "setuid file, root to nobody" "$RUN" "$t/suid_nobody"
mk sgid_nobody 2755 nobody:nogroup
run env V3COMPAT_NATIVE=1 "$RUN" "$t/sgid_nobody"
case $out in *egid=65534*) ok "native setgid changes the group" ;; *) skip "setgid has no effect here: $out" ;; esac
refused "setgid file, root to nogroup" "$RUN" "$t/sgid_nobody"
refused "setuid file, from execve in a program" "$RUN" "$B/execer_static" plain "$t/suid_nobody"
refused "setuid file, from a thread" "$RUN" "$B/execer_dyn" thread "$t/suid_nobody"
refused "setuid file, by execveat" "$RUN" "$B/execer_dyn" execveat-path "$t/suid_nobody"
# A vfork child is refused; its parent goes on and sees the status.
run "$RUN" "$B/execer_dyn" vfork "$t/suid_nobody"
if [ "$st" = 87 ] && echo "$out" | grep -q 'child exited 87' && echo "$err" | grep -q 'refusing to exec'; then ok "setuid file, from a vfork child"; else fail "vfork child: status $st stdout '$out' stderr '$err'"; fi
run "$RUN" "$B/execer_dyn" spawn "$t/suid_nobody"
if [ "$st" = 87 ] && echo "$out" | grep -q 'child exited 87'; then ok "setuid file, from posix_spawn"; else fail "spawn: status $st stdout '$out' stderr '$err'"; fi

# What natively changes nothing is run: root running a setuid-root file, no_new_privs, a script.
mk suid_root 4755 root:root
ran "setuid-root file, run by root" 'euid=0 .*translated=1' "$RUN" "$t/suid_root" translated
ran "setuid file under no_new_privs" 'euid=0 .*nnp=1' "$RUN" "$B/execer_dyn" nnp "$t/suid_nobody" translated
printf '#!%s translated\n' "$B/lz_dyn" > "$t/script"; $S chown nobody:nogroup "$t/script"; $S chmod 4755 "$t/script"
ran "setuid script is not refused (the kernel ignores the bit)" PASS "$RUN" "$t/script"
# The interpreter is what counts.
printf '#!%s\n' "$t/suid_nobody" > "$t/script2"; chmod 755 "$t/script2"
refused "script whose interpreter is setuid" "$RUN" "$t/script2"

# File capabilities: refused for a user they would change, fine for root.
mk cap_file 755 root:root
$S python3 - "$t/cap_file" <<'PY' && capok=1 || capok=0
import os, struct, sys
# VFS_CAP_REVISION_2, permitted CAP_NET_BIND_SERVICE
os.setxattr(sys.argv[1], 'security.capability', struct.pack('<IIIII', 0x02000000, 1 << 10, 0, 0, 0))
PY
if [ $capok = 1 ]; then
  ran "file capabilities, run by root" 'euid=0 .*translated=1' "$RUN" "$t/cap_file" translated
  if command -v setpriv >/dev/null; then
    refused "file capabilities, run by an ordinary user" setpriv --reuid=65534 --regid=65534 --clear-groups "$RUN" "$t/cap_file"
  else
    part_skip "setpriv not found"
  fi
else
  part_skip "cannot set security.capability here"
fi

# Executable but not readable: DynamoRIO could not load it, so it is refused rather than run untranslated.
mk unreadable 711 root:root; $S chmod 111 "$t/unreadable"
if command -v setpriv >/dev/null; then
  as_user="setpriv --reuid=65534 --regid=65534 --clear-groups"
  refused "executable but unreadable file" $as_user "$RUN" "$t/unreadable"
  refused "executable but unreadable file, exec'd by a program" $as_user "$RUN" "$B/execer_static" plain "$t/unreadable"
else
  part_skip "setpriv not found"
fi
exit $bad
