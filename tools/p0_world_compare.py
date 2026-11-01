"""Offline candidates only. No target execution; no activation certification."""
import json
import re
import struct
from pathlib import Path
from compare_newer_offline import PE, ROOT

outdir = Path(__file__).parent / 'ghidra'
old = PE(ROOT / 'Stormworks/stormworks64.exe')
new = PE(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('STORMWORKS_NEWER_EXE', r'..\Stormworks_Newer\stormworks64.exe'))
text = '\n'.join((outdir / name).read_text() for name in
                 ['p0_world_contract.txt', 'p0_world_vehicle.txt'])
entries = []
for match in re.finditer(r'SIG (140[0-9a-f]+) len=\d+ matches=\d+ first=\w+\s*\n([0-9A-F? ]+)', text):
    rva = int(match[1], 16) - old.image_base
    tokens = match[2].split()
    pattern = bytes(0 if t == '??' else int(t, 16) for t in tokens)
    mask = bytes(0 if t == '??' else 255 for t in tokens)
    fixed = [(i, b) for i, (b, m) in enumerate(zip(pattern, mask)) if m]
    first, val = fixed[0]
    hits = []
    for va, data in new.executable():
        at = data.find(bytes([val]), first)
        while at >= 0:
            start = at - first
            if start + len(pattern) <= len(data) and all(data[start+i] == b for i, b in fixed):
                hits.append(va+start)
            at = data.find(bytes([val]), at+1)
    entries.append({'reference_rva': hex(rva), 'bytes': pattern.hex(), 'mask': mask.hex(),
                    'new_candidate_rvas': [hex(h) for h in hits],
                    'new_regions': [new.region(h) for h in hits]})
report = {'kind': 'offline masked-window candidate search; not validated ABI',
          'old_sha256': old.sha256, 'new_sha256': new.sha256, 'entries': entries}
report['new_string_witnesses'] = {
    hex(rva): new.read(rva, 100).split(b'\0')[0].decode('ascii') for rva in
    [0xb596b4, 0xb90eb0, 0xb91e68, 0xb911f0, 0xb960e8, 0xb96270]}
report['script_operators_vehicle_position_slot'] = {
    label: {'vtable_rva': hex(vt), 'slot_offset': '0xc8',
            'target_rva': hex(struct.unpack('<Q', pe.read(vt+0xc8, 8))[0]-pe.image_base)}
    for label, pe, vt in [('reference', old, 0xb0f0e0), ('new', new, 0xb11a28)]}
(outdir / 'p0_world_candidates.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
