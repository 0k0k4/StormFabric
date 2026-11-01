"""Read-only, stdlib-only PE/contract comparison; prints JSON, never runs target.

This is an investigation aid, not a discovery profile or activation certificate.
No REA evidence is produced. All addresses are RVAs. .pdata entries are unwind
regions, not necessarily complete semantic functions. E8 searches are byte scans,
not instruction decoding; candidate calls require independent corroboration.
"""
import bisect
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PE:
    def __init__(self, path):
        self.path = str(path)
        self.data = Path(path).read_bytes()
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        nt = self.u32(0x3c)
        assert self.data[nt:nt + 4] == b'PE\0\0'
        count, optsize = self.u16(nt + 6), self.u16(nt + 20)
        opt = nt + 24
        assert self.u16(opt) == 0x20b
        self.image_base = struct.unpack_from('<Q', self.data, opt + 24)[0]
        self.sections = []
        for i in range(count):
            p = opt + optsize + 40 * i
            name = self.data[p:p + 8].rstrip(b'\0').decode()
            vs, va, rs, rp = struct.unpack_from('<IIII', self.data, p + 8)
            self.sections.append((name, va, vs, rp, rs, self.u32(p + 36)))
        pd, size = struct.unpack_from('<II', self.data, opt + 112 + 3 * 8)
        raw = self.read(pd, size)
        self.pdata = [struct.unpack_from('<III', raw, i) for i in range(0, size, 12)]
        self.begins = [p[0] for p in self.pdata]

    def u16(self, p):
        return struct.unpack_from('<H', self.data, p)[0]

    def u32(self, p):
        return struct.unpack_from('<I', self.data, p)[0]

    def read(self, rva, size):
        for _, va, _, rp, rs, _ in self.sections:
            if va <= rva and rva + size <= va + rs:
                return self.data[rp + rva - va:rp + rva - va + size]
        raise ValueError(f'RVA not file-backed: {rva:x}+{size:x}')

    def region(self, rva):
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and self.pdata[i][0] <= rva < self.pdata[i][1]:
            a, b, u = self.pdata[i]
            return {'begin': hex(a), 'end': hex(b), 'size': b - a, 'unwind': hex(u)}
        return None

    def executable(self):
        for _, va, _, rp, rs, flags in self.sections:
            if flags & 0x20000000:
                yield va, self.data[rp:rp + rs]

    def hits(self, pattern):
        result = []
        for va, raw in self.executable():
            at = raw.find(pattern)
            while at >= 0:
                result.append(va + at)
                at = raw.find(pattern, at + 1)
        return result

    def calls_to(self, target):
        result = []
        for va, raw in self.executable():
            at = raw.find(b'\xe8')
            while 0 <= at <= len(raw) - 5:
                if va + at + 5 + struct.unpack_from('<i', raw, at + 1)[0] == target:
                    result.append(va + at)
                at = raw.find(b'\xe8', at + 1)
        return result


def spans(indices):
    groups = []
    for i in indices:
        if groups and i == groups[-1][-1] + 1:
            groups[-1].append(i)
        else:
            groups.append([i])
    return groups


def compare(pe, c, start):
    old, mask = bytes.fromhex(c['bytes']), bytes.fromhex(c['mask'])
    new = pe.read(start, len(old))
    differences = [i for i, (a, b, m) in enumerate(zip(old, new, mask)) if (a & m) != (b & m)]
    groups = spans(differences)
    return {'candidate_start': hex(start), 'contract_size': len(old),
            'pdata': pe.region(start), 'fixed_bytes': sum(m == 255 for m in mask),
            'fixed_mismatches': len(differences), 'difference_span_count': len(groups),
            'difference_spans_first_24': [
                {'offset': hex(g[0]), 'old': old[g[0]:g[-1] + 1].hex(),
                 'new': new[g[0]:g[-1] + 1].hex()} for g in groups[:24]]}


def anchor_candidates(pe, c):
    raw, mask = bytes.fromhex(c['bytes']), bytes.fromhex(c['mask'])
    runs = sorted(spans([i for i, m in enumerate(mask) if m == 255]), key=len, reverse=True)
    candidates = set()
    for run in runs[:12]:
        if len(run) < 8:
            continue
        for hit in pe.hits(raw[run[0]:run[-1] + 1]):
            region = pe.region(hit)
            if region:
                candidates.add(int(region['begin'], 16))
    scored = [compare(pe, c, a) for a in candidates]
    return sorted(scored, key=lambda d: d['fixed_mismatches'])[:5]


def edge_target(pe, start, edge):
    return start + edge['end'] + struct.unpack('<i', pe.read(start + edge['offset'], 4))[0]


def main():
    old = PE(ROOT / 'Stormworks/stormworks64.exe')
    new = PE(Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('STORMWORKS_NEWER_EXE', r'..\Stormworks_Newer\stormworks64.exe')))
    assert old.sha256 == '55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3'
    assert new.sha256 == 'f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699'
    contracts_path = ROOT / 'StormworksModLoader/tools/discovery_contracts.json'
    data = json.loads(contracts_path.read_text())
    contracts = {e['rva']: e for e in data['entries'] + data['witnesses']}
    assert all(old.read(e['start'], len(e['bytes']) // 2) == bytes.fromhex(e['bytes'])
               for e in contracts.values())
    report = {'evidence': 'Offline file bytes and retained contracts only; no REA or runtime evidence.',
              'sources': [{'path': p.path, 'sha256': p.sha256} for p in (old, new)],
              'contracts_sha256': hashlib.sha256(contracts_path.read_bytes()).hexdigest()}
    call = contracts[0x74a538]
    call_start = 0x74adf8 - (call['rva'] - call['start'])
    target = edge_target(new, call_start, call['refs'][0])
    report['monitor_window'] = compare(new, call, call_start)
    report['monitor_window']['call_rva'] = hex(0x74adf8)
    report['monitor_window']['decoded_target'] = hex(target)
    report['monitor_draw'] = compare(new, contracts[0x74aa50], target)
    report['monitor_draw']['reference_pdata'] = old.region(0x74aa50)
    report['update_candidates'] = anchor_candidates(new, contracts[0x366e90])
    report['update_reference_pdata'] = old.region(0x366e90)
    report['update_pdata_partition_sizes'] = {
        label: [b - a for a, b, _ in pe.pdata if start <= a < start + 2793]
        for label, pe, start in [('reference', old, 0x366e90), ('new', new, 0x366130)]}
    gpu = contracts[0x6d87d0]
    report['gpu_calls'] = []
    for at in new.calls_to(0x689620):
        item = compare(new, gpu, at - (gpu['rva'] - gpu['start']))
        item['call_rva'] = hex(at)
        report['gpu_calls'].append(item)
    report['gpu_calls'].sort(key=lambda d: d['fixed_mismatches'])
    report['gpu_native'] = compare(new, contracts[0x688ec0], 0x689620)
    report['ui'] = []
    for key, candidate in ((0x6fbff0, 0x6fc8a0), (0x245990, 0x244a00)):
        c = contracts[key]
        item = {'name': c['name'], 'key': hex(key), 'body': compare(new, c, candidate), 'edges': []}
        for e in c['refs']:
            dependency = contracts.get(e['target'])
            if not dependency:
                continue
            dest = edge_target(new, candidate, e)
            d = compare(new, dependency, dest)
            d.update({'edge_offset': hex(e['offset']), 'dependency': dependency['name'],
                      'dependency_key': hex(e['target']), 'reference_pdata': old.region(e['target'])})
            item['edges'].append(d)
        item['checked_contract_edges'] = len(item['edges'])
        item['matching_contract_edges'] = sum(d['fixed_mismatches'] == 0 for d in item['edges'])
        item['edges'] = [d for d in item['edges'] if d['fixed_mismatches']]
        report['ui'].append(item)
    report['ui_witness_jump_tables'] = []
    for table, count in [(0xd68050, 16), (0xd68090, 9), (0xd680b4, 9), (0xd680d8, 9)]:
        previous = struct.unpack('<' + 'I' * count, old.read(table, 4 * count))
        current = struct.unpack('<' + 'I' * count, new.read(table + 0x2000, 4 * count))
        report['ui_witness_jump_tables'].append({
            'old_rva': hex(table), 'new_rva': hex(table + 0x2000), 'entry_count': count,
            'old_targets': [hex(x) for x in previous], 'new_targets': [hex(x) for x in current],
            'all_target_offsets_preserved': all(a - 0x99bc00 == b - 0x99d3b0
                                                for a, b in zip(previous, current))})
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
