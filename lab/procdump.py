#!/usr/bin/env python3
# procdump.py PID: each thread's syscall, pc and the words on its stack that point into DynamoRIO, drmgr or the
# client, without stopping the process (gdb cannot attach to a group with a thread in vfork). Needs root.
import re, struct, subprocess, sys
pid = sys.argv[1]
segs = {}
def load_segs(lib):
    if lib not in segs:
        r = subprocess.run(['readelf', '-lW', lib], capture_output=True, text=True).stdout
        segs[lib] = [(int(m[1], 16), int(m[2], 16), int(m[3], 16))
                     for m in re.finditer(r'LOAD\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+\S+\s+(0x[0-9a-f]+)', r)]
    return segs[lib]
libs = []
for l in open(f'/proc/{pid}/maps'):
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) (\S+) \S+ \S+\s*(.*)', l)
    if m and m[5].startswith('/') and 'x' in m[3]:
        libs.append((int(m[1], 16), int(m[2], 16), int(m[4], 16), m[5].strip()))
def sym(a):
    for b, e, off, lib in libs:
        if b <= a < e:
            fo = a - b + off
            va = next((fo - o + v for o, v, fs in load_segs(lib) if o <= fo < o + fs), fo)
            dbg = lib + '.debug' if __import__('os').path.exists(lib + '.debug') else lib
            r = subprocess.run(['addr2line', '-f', '-e', dbg, hex(va)], capture_output=True, text=True).stdout.split('\n')
            return f'{lib.split("/")[-1]}+{va:#x} {r[0] if r else "?"}'
    return None
mem = open(f'/proc/{pid}/mem', 'rb')
for tid in sorted(subprocess.run(['ls', f'/proc/{pid}/task'], capture_output=True, text=True).stdout.split(), key=int):
    t = f'/proc/{pid}/task/{tid}'
    try:
        sc = open(f'{t}/syscall').read().split()
        print(f'== task {tid} state={open(f"{t}/stat").read().split(")")[1].split()[0]} wchan={open(f"{t}/wchan").read()} syscall={" ".join(sc)}')
    except OSError as e:
        print(f'== task {tid}: {e}'); continue
    if len(sc) < 3: continue
    sp, pc = int(sc[-2], 16), int(sc[-1], 16)
    print(f'   pc {sym(pc) or hex(pc)}')
    try:
        mem.seek(sp); data = mem.read(8 * 400)
    except OSError as e:
        print(f'   stack: {e}'); continue
    n = 0
    for (w,) in struct.iter_unpack('<Q', data):
        s = sym(w)
        if s and n < 30 and ('dynamorio' in s or 'drmgr' in s or 'v3compat' in s): print('   ' + s); n += 1
