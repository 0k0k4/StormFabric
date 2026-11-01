"""Offline, read-only Lua binding review. Never loads or executes the target.

Prints JSON; redirect stdout to newer_lua_bindings.json to retain the review.
Uses retained Ghidra instruction masks, full bodies, rel32 edges and PE tables.
No original contract is modified. All addresses in output are RVAs.
"""
import json
import re
import struct
from pathlib import Path

from compare_newer_offline import PE, ROOT, edge_target

OLD_HASH = '55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3'
NEW_HASH = 'f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699'


def matches(pe, c):
    raw, mask = bytes.fromhex(c['bytes']), bytes.fromhex(c['mask'])
    assert set(mask) <= {0, 255}
    pattern = re.compile(b''.join(re.escape(bytes([b])) if m else b'.'
                                  for b, m in zip(raw, mask)), re.DOTALL)
    return sorted({va + m.start() for va, data in pe.executable()
                   for m in pattern.finditer(data)})


def main():
    old = PE(ROOT / 'Stormworks/stormworks64.exe')
    new = PE(Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('STORMWORKS_NEWER_EXE', r'..\Stormworks_Newer\stormworks64.exe')))
    assert old.sha256 == OLD_HASH and new.sha256 == NEW_HASH
    source = (ROOT / 'StormworksModLoader/src/native_lua.cpp').read_text(encoding='utf-8')
    keys = sorted({int(x, 16) for x in re.findall(
        r'(?:g_base\s*\+\s*|EngineRva\(g_base,\s*)(0x[0-9a-fA-F]+)', source)})
    data = json.loads((Path(__file__).parent / 'discovery_contracts.json').read_text())
    contracts = {c['rva']: c for c in data['entries'] + data['witnesses']}
    for c in contracts.values():
        assert old.read(c['start'], len(c['bytes']) // 2) == bytes.fromhex(c['bytes'])
    candidates = {k: matches(new, c) for k, c in contracts.items()}
    resolved = {k: a[0] for k, a in candidates.items() if len(a) == 1}
    proofs = {}
    # A caller's exact retained full body fixes each typed rel32 operand location.
    # Only accept a target if its complete retained masked body also matches.
    changed = True
    while changed:
        changed = False
        for caller, start in list(resolved.items()):
            for edge in contracts[caller]['refs']:
                target = edge['target']
                if target not in candidates or target in resolved:
                    continue
                dest = edge_target(new, start, edge)
                if dest in candidates[target]:
                    resolved[target] = dest
                    proofs[target] = {'caller_key': hex(caller), 'caller_new': hex(start),
                                      'operand_offset': hex(edge['offset']), 'flow': edge['flow']}
                    changed = True
    result = {'evidence_kind': 'Offline retained Ghidra contracts and PE bytes; no runtime evidence',
              'reference_sha256': old.sha256, 'newer_sha256': new.sha256,
              'bindings': [], 'unresolved': []}
    for key in keys:
        c = contracts.get(key)
        if key in resolved:
            result['bindings'].append({'key': hex(key), 'new_rva': hex(resolved[key]),
                'name': c['name'], 'span_size': len(c['bytes']) // 2,
                'fixed_bytes': bytes.fromhex(c['mask']).count(255),
                'full_masked_body_match': True, 'candidate_count': len(candidates[key]),
                'rel32_identity_proof': proofs.get(key)})
        else:
            result['unresolved'].append({'key': hex(key), 'contract': c is not None,
                                       'candidates': [hex(x) for x in candidates.get(key, [])]})
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
