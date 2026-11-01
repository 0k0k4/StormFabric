"""Produce compiled bindings for TWO hash-pinned, offline-reviewed executables.
Unique full-body matching + RTTI + decoded references determine identities.
This never loads/runs either executable and does not loosen unknown-build gates.
"""
import hashlib
import json
import struct
from pathlib import Path
from compare_newer_offline import PE

ROOT = Path(__file__).resolve().parents[1]
old=PE(ROOT.parent/'Stormworks/stormworks64.exe')
new=PE(Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('STORMWORKS_NEWER_EXE', r'..\Stormworks_Newer\stormworks64.exe')))
assert old.sha256=='55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3'
assert new.sha256=='f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699'
data=json.loads((ROOT/'tools/discovery_contracts.json').read_text())
contracts={e['rva']:e for e in data['entries']+data['witnesses']}
primary=list(data['entries'])
# P0 Lua additions have their own hash-pinned retained instruction contracts.
p0=json.loads((ROOT/'tools/p0_lua_contracts.json').read_text())
p0_review=json.loads((ROOT/'tools/p0_lua_binding_evidence.json').read_text())
assert p0['reference_sha256']==old.sha256
assert p0_review['reference_sha256']==old.sha256 and p0_review['newer_sha256']==new.sha256
assert hashlib.sha256((ROOT/'tools/p0_lua_contracts.json').read_bytes()).hexdigest()==p0_review['contract_file_sha256']
for c in p0['entries']+p0['witnesses']:
    assert old.read(c['start'],len(c['bytes'])//2)==bytes.fromhex(c['bytes'])
    if c['rva'] not in contracts:contracts[c['rva']]=c
primary.extend(p0['entries'])
mapping={}
evidence={}
def bind(key,rva,why):
    if key in mapping:
        assert mapping[key]==rva,(hex(key),hex(mapping[key]),hex(rva),why)
    mapping[key]=rva;evidence.setdefault(key,why)
def match(c,rva):
    body=bytes.fromhex(c['bytes']);mask=bytes.fromhex(c['mask'])
    try: candidate=new.read(rva,len(body))
    except ValueError:return False
    return all(a&m==b&m for a,b,m in zip(body,candidate,mask))
def unique(c):
    body=bytes.fromhex(c['bytes']);mask=bytes.fromhex(c['mask'])
    best=(0,0);run=0
    for i,m in enumerate(mask):
        run=run+1 if m==255 else 0
        if run>best[1]:best=(i+1-run,run)
    offset,n=best
    hits=[h-offset for h in new.hits(body[offset:offset+n]) if match(c,h-offset)]
    return hits[0] if len(hits)==1 else None
for c in primary:
    candidate=unique(c)
    if candidate is not None:bind(c['rva'],candidate+c['rva']-c['start'],'unique complete masked body')
# Only these field-changing video bodies/windows are explicitly reviewed here.
for key,rva in [(0x74aa50,0x74b310),(0x6d87d0,0x6d9041),(0x366e90,0x366130)]:
    bind(key,rva,'reviewed field-changing video contract: newer_offline_comparison.json')
for t in data['tables']:
    candidates=[]
    for _,va,_,raw,n,flags in new.sections:
        if not(flags&0x40000000) or flags&(0xa0000000):continue
        blob=new.data[raw:raw+n]
        for at in range(0,n-8,8):
            col=struct.unpack_from('<Q',blob,at)[0]-new.image_base
            try:
                sig,offset,_,typ,_,selfrva=struct.unpack('<6I',new.read(col,24))
                if sig!=1 or offset or col!=selfrva:continue
                name=new.read(typ+16,len(t['type'])+1)
                if name!=t['type'].encode()+b'\0':continue
                table=va+at+8
                targets=[struct.unpack('<Q',new.read(table+s['slot']*8,8))[0]-new.image_base for s in t['slots']]
                if not all(match(contracts[s['target']],target) for s,target in zip(t['slots'],targets)):continue
                candidates.append((table,targets))
            except (ValueError,struct.error):continue
    assert len(candidates)==1,(t['type'],candidates)
    table,targets=candidates[0];bind(t['rva'],table,'exact RTTI class and all required method bodies')
    for s,target in zip(t['slots'],targets):bind(s['target'],target,'RTTI-identified native vtable slot')
changed=True
while changed:
    changed=False
    for key,c in contracts.items():
        if key not in mapping:continue
        start=mapping[key]-(c['rva']-c['start'])
        for ref in c['refs']:
            target=ref['target']
            if target not in contracts:continue
            destination=start+ref['end']+struct.unpack('<i',new.read(start+ref['offset'],4))[0]
            if target not in mapping:
                bind(target,destination,'decoded reference from '+hex(key));changed=True
            else:assert mapping[target]==destination,(hex(key),hex(target),'inconsistent reference graph')
# Input manifests are produced by independent subsystem reviews if needed.
for name in ('newer_lua_bindings.json','newer_video_bindings.json'):
    path=ROOT/'tools'/name
    if not path.exists():continue
    extra=json.loads(path.read_text(encoding='utf-8-sig'))
    rows=extra.get('bindings',{})
    if isinstance(rows,list):rows={entry['key']:entry['new_rva'] for entry in rows}
    if isinstance(rows,dict):
        for key,value in rows.items():
            if isinstance(value,dict):value=value.get('rva',value.get('new_rva'))
            bind(int(key,0),int(value,0) if isinstance(value,str) else value,'subsystem review '+name)
for row in p0_review['bindings']:
    key=int(row['key'],0);rva=int(row['new_rva'],0)
    assert mapping.get(key)==rva
    assert new.read(rva,row['span_size'])==bytes.fromhex(row['newer_bytes'])
missing=[hex(e['rva']) for e in primary if e['rva'] not in mapping]
assert not missing,missing
report={'reference_sha256':old.sha256,'newer_sha256':new.sha256,
        'bindings':{hex(k):{'rva':hex(v),'evidence':evidence[k]} for k,v in sorted(mapping.items())}}
(ROOT/'tools/reviewed_engine_bindings.json').write_text(json.dumps(report,indent=2)+'\n')
def lit(b):
    return '\n'.join('"'+''.join(f'\\x{x:02x}' for x in b[i:i+128])+'"' for i in range(0,len(b),128))
lines=['// Generated by tools/gen_engine_bindings.py; two hash-pinned reviewed builds.','#pragma once']
# Code fingerprints retain all new-build bytes, including relative call destinations.
# Absolute vtable pointers are validated by slots instead, to remain ASLR-safe.
for i,(key,c) in enumerate(contracts.items()):
    for label,pe,rva in [('Old',old,key),('New',new,mapping.get(key))]:
        if rva is None:continue
        start=rva-(c['rva']-c['start']);body=pe.read(start,len(c['bytes'])//2)
        lines.append(f'static const char kBinding{label}{i}[] =\n{lit(body)};')
lines.append('static const Binding kEngineBindings[] = {')
for i,(key,c) in enumerate(contracts.items()):
    if key not in mapping:continue
    displacement=c['rva']-c['start'];n=len(c['bytes'])//2
    lines.append(f'    {{0x{key:x},0x{key:x},0x{mapping[key]:x},{displacement},{n},kBindingOld{i},kBindingNew{i}}},')
lines.append('};')
for i,t in enumerate(data['tables']):
    lines.append(f'static const BindingSlot kBindingSlots{i}[] = {{'+','.join(f'{{{s["slot"]},0x{s["target"]:x}}}' for s in t['slots'])+'};')
lines.append('static const BindingTable kEngineBindingTables[] = {')
for i,t in enumerate(data['tables']):
    lines.append(f'    {{0x{t["rva"]:x},0x{mapping[t["rva"]]:x},kBindingSlots{i},{len(t["slots"])}}},')
lines.append('};')
video=json.loads((ROOT/'tools/newer_video_bindings.json').read_text())
windows=list(video['windows'])
ctor=video['monitor_constructor']
windows.append({'reference_rva':ctor['reference_rva'],'newer_rva':ctor['newer_rva'],'size':ctor['size']})
for i,window in enumerate(windows):
    for label,pe,key in [('Old',old,'reference_rva'),('New',new,'newer_rva')]:
        body=pe.read(int(window[key],0),window['size'])
        expected=window.get('reference_bytes' if label=='Old' else 'newer_bytes')
        if expected:assert body==bytes.fromhex(expected)
        lines.append(f'static const char kBindingLayout{label}{i}[] =\n{lit(body)};')
lines.append('static const BindingWindow kEngineLayoutWindows[] = {')
for i,window in enumerate(windows):
    lines.append(f'    {{{window["reference_rva"]},{window["newer_rva"]},{window["size"]},kBindingLayoutOld{i},kBindingLayoutNew{i}}},')
lines.append('};')
(ROOT/'src/engine_bindings.generated.h').write_text('\n'.join(lines)+'\n')
print('Generated',len(mapping),'reviewed bindings; no target execution')
