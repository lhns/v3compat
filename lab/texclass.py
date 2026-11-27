#!/usr/bin/env python3
# texclass.py CORE BIN DRDEBUG ERR OUT: one line classifying a run's core: whose fault the dumping thread took
# (si_code, its pc's object and function) and the @@TEX prints of a lab-te-exit-diag fork build.
import re, subprocess, sys
core, binary, dbg, err, out = sys.argv[1:6]

def a2l(f, a):
    r = subprocess.run(['addr2line', '-f', '-e', f, hex(a)], capture_output=True, text=True).stdout.split('\n')
    return r[0] if r and r[0] else '?'

g = subprocess.run(['gdb', '-q', '-batch', '-c', core, binary, '-ex', 'p $_siginfo.si_code',
                    '-ex', 'p $_siginfo._sifields._sigfault.si_addr', '-ex', 'p/x $pc', '-ex', 'info proc mappings'],
                   capture_output=True, text=True, timeout=120).stdout
vals = re.findall(r'^\$\d+ = (.*)$', g, re.M)
code = vals[0] if len(vals) > 0 else '?'
addr = re.search(r'0x[0-9a-f]+', vals[1]).group(0) if len(vals) > 1 and '0x' in vals[1] else (vals[1] if len(vals) > 1 else '?')
pc = int(vals[2], 16) if len(vals) > 2 and vals[2].startswith('0x') else None
maps = []
for m in re.finditer(r'^\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(\S+)', g, re.M):
    maps.append((int(m[1], 16), int(m[2], 16), int(m[3], 16), m[4]))
def base(obj):
    return next((b for b, e, off, o in maps if o == obj and off == 0), None)
where = 'anon'
if pc is not None:
    for b, e, off, o in maps:
        if b <= pc < e:
            name = o.split('/')[-1]
            bb = base(o)
            if name == 'libdynamorio.so':
                where = 'dr:' + a2l(dbg, pc - bb + 0x71000000)
            elif o == binary or name == binary.split('/')[-1]:
                where = 'app:' + a2l(o, pc - bb)
            else:
                where = name + '+' + hex(pc - bb)
            break
tex = {}
first = []
try:
    for l in open(err, errors='replace'):
        m = re.match(r'@@TEX (\w+(?: \w+)?)', l)
        if m:
            k = m.group(1); tex[k] = tex.get(k, 0) + 1
            c = re.search(r'caller=(0x[0-9a-f]+) dr=(0x[0-9a-f]+)', l)
            if c:
                l = l.strip() + ' <' + a2l(dbg, int(c[1], 16) - int(c[2], 16) + 0x71000000) + '>'
            if len(first) < 6: first.append(l.strip()[6:200])
except OSError:
    pass
printed = int('created' in open(out, errors='replace').read()) if out != '-' else 0
print(f'CLS code={code} addr={addr} where={where} printed={printed} tex={",".join(f"{k}:{v}" for k, v in sorted(tex.items())) or "-"}')
for f in first:
    print('  TEX ' + f)
