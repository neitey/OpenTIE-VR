import re,sys
# usage: stubgen.py map.txt out.c out.ver arch api
src,outc,outv,arch,api=sys.argv[1:6]; api=int(api)
ARCHES={'arm','arm64','x86','x86_64','riscv64'}
blocks=[];cur=None
for raw in open(src):
    line=raw.rstrip('\n')
    code,_,cmt=line.partition('#')
    code=code.strip(); tags=cmt.split()
    m=re.match(r'^([A-Za-z0-9_]+)\s*\{',code)
    if m:
        cur=[m.group(1),[],tags]; blocks.append(cur); continue
    if code.startswith('}'):
        cur=None; continue
    if cur is None or not code or code in('global:','local:') or code.startswith('local:') or code=='*;': continue
    if code.endswith(':'): continue
    sym=code.rstrip(';').strip()
    if not re.match(r'^[A-Za-z_][A-Za-z0-9_]*$',sym): continue
    alltags=tags+cur[2]
    archtags=[t for t in alltags if t in ARCHES]
    if archtags and arch not in archtags: continue
    skip=False
    for t in alltags:
        if t.startswith('introduced=') or t.startswith('introduced-'+arch+'='):
            v=t.split('=')[1]
            try:
                if int(v)>api: skip=True
            except: pass
        if t in('platform-only','apex','systemapi','llndk','legacy') : pass
    if 'platform-only' in alltags or 'apex' in alltags or 'systemapi' in alltags: skip=True
    if skip: continue
    cur[1].append((sym,'var' in alltags or 'weak' in alltags and False))
with open(outc,'w') as c, open(outv,'w') as v:
    seen=set()
    for name,syms,tags in blocks:
        v.write(name+' {\n global:\n')
        for s,isvar in syms:
            v.write('  %s;\n'%s)
            if s in seen: continue
            seen.add(s)
            if isvar: c.write('char %s[64];\n'%s)
            else: c.write('void %s(void){}\n'%s)
        v.write('};\n')
print(len(seen),'symbols')
