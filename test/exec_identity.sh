#!/bin/sh
# exec_identity.sh RUN BUILD: what a program sees of how it was exec'd is what it sees natively, for each way in.
RUN=$1; B=$2
t=$(mktemp -d "$B/identity.XXXXXX"); trap 'rm -rf "$t"' EXIT
bad=0
ex=$B/execer_dyn
cp "$B/execer_dyn" "$t/prog"; ln -s prog "$t/link"
printf '#!%s whoami\n' "$t/prog" > "$t/script"; chmod 755 "$t/script"
printf '#!%s\n' "$t/script" > "$t/script2"; chmod 755 "$t/script2"
# same NAME ARGS...: execer's output and status, natively and under the client.
same() {
  name=$1; shift
  n=$(cd "$t" && V3COMPAT_NATIVE=1 "$RUN" -- "$ex" "$@" a1 'a 2' 2>&1); ns=$?
  c=$(cd "$t" && "$RUN" -- "$ex" "$@" a1 'a 2' 2>&1); cs=$?
  if [ "$n" = "$c" ] && [ "$ns" = "$cs" ]; then echo "ok $name"; else echo "FAIL $name: native $ns:"; echo "$n"; echo "client $cs:"; echo "$c"; bad=1; fi
}
same "absolute path" plain "$t/prog" whoami
same "symlink" plain "$t/link" whoami
same "relative path" plain ./prog whoami
same "bare argv0" argv0 "$t/prog" whoami
same "execveat, path" execveat-path "$t/prog" whoami
same "execveat, dirfd" execveat-dirfd "$t/prog" whoami
same "execveat of a symlink, no follow" execveat-nofollow "$t/link" whoami
same "fexecve, close-on-exec" fexecve-cloexec "$t/prog" whoami
same "fexecve" fexecve "$t/prog" whoami
same "O_PATH fexecve" fexecve-path "$t/prog" whoami
same "script" plain "$t/script"
same "script of a script" plain "$t/script2"
same "script, execveat dirfd" execveat-dirfd "$t/script"
same "script, fexecve" fexecve "$t/script"
same "script, fexecve close-on-exec: ENOENT" fexecve-cloexec "$t/script"
same "script, dirfd close-on-exec: ENOENT" execveat-dirfd-cloexec "$t/script"
mkfifo "$t/fifo"; chmod 755 "$t/fifo"
same "FIFO: EACCES, does not block" plain "$t/fifo"
exit $bad
