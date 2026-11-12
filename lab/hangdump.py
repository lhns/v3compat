#!/usr/bin/env python3
# hangdump.py PID OUT: a poor man's stack of each thread: words on its stack that point into libdynamorio or the client.
import re, subprocess, sys
pid, out = sys.argv[1], sys.argv[2]
maps = open(f'/proc/{pid}/maps').read().splitlines()
libs = {}
for l in maps:
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) (\S+) \S+ \S+\s*(.*)', l)
    if m and m[5] and ('dynamorio' in m[5] or 'v3compat' in m[5]):
        b, e, off = int(m[1], 16), int(m[2], 16), int(m[4], 16)
        libs.setdefault(m[5], []).append((b, e, off, m[3]))
g = subprocess.run(['gdb', '-q', '-batch', '-p', pid, '-ex', 'thread apply all -q -- info registers rip rsp',
                    '-ex', 'thread apply all -q -- x/300gx $rsp'], capture_output=True, text=True, timeout=120).stdout
def sym(a):
    for lib, rs in libs.items():
        for b, e, off, perm in rs:
            if b <= a < e and 'x' in perm:
                r = subprocess.run(['addr2line', '-f', '-e', lib, hex(a - b + off)], capture_output=True, text=True).stdout.split('\n')
                return f'{lib.split("/")[-1]}+{a-b+off:#x} {r[0] if r else "?"} {r[1].split("/")[-1] if len(r)>1 else ""}'
    return None
o = [f'pid {pid}']
for t in re.split(r'(?=Thread \d+ \()', g):
    head = t.split('\n', 1)[0][:60]
    rip = re.search(r'rip\s+(0x[0-9a-f]+)', t)
    if not rip: continue
    o.append(f'== {head} rip={rip[1]} {sym(int(rip[1],16)) or ""}')
    n = 0
    for w in re.findall(r'\s(0x[0-9a-f]{16})', t):
        s = sym(int(w, 16))
        if s and n < 25: o.append('   ' + s); n += 1
for tid in subprocess.run(['ls', f'/proc/{pid}/task'], capture_output=True, text=True).stdout.split():
    try:
        o.append(f'task {tid} wchan={open(f"/proc/{pid}/task/{tid}/wchan").read()} syscall={open(f"/proc/{pid}/task/{tid}/syscall").read().strip()}')
    except OSError: pass
open(out, 'a').write('\n'.join(o) + '\n')
