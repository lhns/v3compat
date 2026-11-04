#!/usr/bin/python3
import sys,bisect,subprocess,re,collections
f=sys.argv[1]; topn=int(sys.argv[2]) if len(sys.argv)>2 else 45
syms={}
def segs(path):
    r=[]
    for l in subprocess.run(['readelf','-lW',path],capture_output=True,text=True).stdout.splitlines():
        p=l.split()
        if p and p[0]=='LOAD': r.append((int(p[1],16),int(p[2],16),int(p[4],16)))
    return r
SEG={}
def f2v(path,off):
    if path not in SEG: SEG[path]=segs(path)
    for o,v,fs in SEG[path]:
        if o<=off<o+fs: return off-o+v
    return off
def load(path):
    if path in syms: return syms[path]
    out=subprocess.run(['nm','-n','--defined-only',path],capture_output=True,text=True).stdout
    a=[];n=[]
    for l in out.splitlines():
        p=l.split()
        if len(p)==3 and p[1] in 'tTwW': a.append(int(p[0],16)); n.append(p[2])
    syms[path]=(a,n); return syms[path]
SYM={'libdynamorio.so':'%s/lib64/release/libdynamorio.so.debug'%sys.argv[3],'libdrmgr.so':'/w/inst/libexec/v3compat/libdrmgr.so','libv3compat.so':'/w/inst/libexec/v3compat/libv3compat.so'}
maps=[];samples=[]
cur=None
for l in open(f,errors='replace'):
    if l[0]=='S' and l[1]==' ':
        p=l.split(); samples.append((int(p[1]),int(p[2]),int(p[3],16),int(p[4])))
    elif l[:2]=='M ':
        cur=[];maps.append((int(l.split()[1]),cur))
    elif l[0]=='E' and l.strip()=='E': pass
    else:
        m=re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) (\S+) \S+ \S+\s*(.*)',l)
        if m and cur is not None: cur.append((int(m[1],16),int(m[2],16),m[3],int(m[4],16),m[5].strip()))
base={}
times=[m[0] for m in maps]
def resolve(t,rip):
    i=max(0,min(bisect.bisect(times,t),len(maps)-1))
    for j in (i,i-1,i+1,0,len(maps)-1):
        if 0<=j<len(maps):
            for a,b,pr,off,name in maps[j][1]:
                if a<=rip<b: return (name or '[anon %s]'%pr, rip-a+off, a-off)
    return ('?',0,0)
fn=collections.Counter();mod=collections.Counter();sysc=collections.Counter();tid=collections.Counter()
for t,tn,rip,orig in samples:
    name,off,_=resolve(t,rip)
    base_=name.split('/')[-1]
    tid[tn]+=1
    if orig>=0: sysc[orig]+=1
    mod[base_ if not base_.startswith('[anon') else base_]+=1
    s=SYM.get(base_)
    if s:
        a,n=load(s); k=bisect.bisect(a,f2v(s.replace('.debug',''),off))-1
        fn[(base_[:8],n[k] if k>=0 else '?')]+=1
    else: fn[(base_,'')]+=1
N=len(samples) or 1
print('samples',N,'threads',len(tid),'top thread share %.1f%%'%(100*tid.most_common(1)[0][1]/N))
print('-- modules');[print('%6.1f%% %s'%(100*c/N,m)) for m,c in mod.most_common(12)]
print('-- stopped in syscall (nr)');[print('%6.1f%% %d'%(100*c/N,m)) for m,c in sysc.most_common(8)]
print('-- functions');[print('%6.1f%% %s %s'%(100*c/N,m[0],m[1])) for m,c in fn.most_common(topn)]
cats=[('decode',r'^(decode|read_|opnd_create|instr_decode|decode_)|^(get_|)decode'),('encode',r'encode|instr_encode|instr_length|copy_and_re_relativize|^opnd_.*size|size_ok|type_instr_uses|instr_info'),('heap',r'heap|alloc|vmm|special_|hashtable.*resize|stack_alloc'),('instr ir',r'^instr|^opnd|^reg_|instrlist'),('bb build',r'bb|build_|mangle|fragment|link|emit|fcache|htable|hashtable|vector|coarse|monitor|trace'),('sync/mem',r'mutex|spin|lock|synch|d_r_|memcpy|memset|signal|os_|dispatch|enter|exit')]
catc=collections.Counter()
for k,c in fn.items():
    if k[0]!='libdynam': continue
    for nm,rx in cats:
        if re.search(rx,k[1]): catc[nm]+=c;break
    else: catc['other DR']+=c
print('-- DR categories (rough regex)');[print('%6.1f%% %s'%(100*c/N,m)) for m,c in catc.most_common()]
