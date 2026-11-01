"""Read-only PE/Ghidra review for P0's three extra private bindings. No target execution."""
import hashlib
import json
from pathlib import Path

from compare_newer_offline import PE, ROOT
from review_newer_lua import OLD_HASH, NEW_HASH, matches


def main():
    folder = Path(__file__).parent
    old = PE(ROOT / 'Stormworks/stormworks64.exe')
    new = PE(Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('STORMWORKS_NEWER_EXE', r'..\Stormworks_Newer\stormworks64.exe')))
    assert old.sha256 == OLD_HASH and new.sha256 == NEW_HASH
    contracts_path = folder / 'p0_lua_contracts.json'
    contracts = json.loads(contracts_path.read_text())
    assert contracts['reference_sha256'] == old.sha256
    result = {
        'evidence_kind': 'Retained read-only Ghidra full instruction bodies + offline PE byte match; no runtime evidence',
        'reference_sha256': old.sha256, 'newer_sha256': new.sha256,
        'contract_file': 'tools/p0_lua_contracts.json',
        'contract_file_sha256': hashlib.sha256(contracts_path.read_bytes()).hexdigest(),
        'bindings': [],
        'limitations': ['No REA provider; direct retained Ghidra export only.',
                        'No game launch, attach, install or runtime proof.',
                        'raiseError is intentionally unsupported; plugin frames are never in the protected native leaf.',
                        'Only microcontroller VMs observed by loader init/destroy hooks; addon VMs unknown.']
    }
    for entry in contracts['entries']:
        body = bytes.fromhex(entry['bytes'])
        assert old.read(entry['start'], len(body)) == body
        candidates = matches(new, entry)
        assert len(candidates) == 1, (entry['name'], candidates)
        rva = candidates[0]
        newer_body = new.read(rva, len(body))
        result['bindings'].append({
            'key': hex(entry['rva']), 'reference_rva': hex(entry['start']),
            'new_rva': hex(rva), 'name': entry['name'], 'span_size': len(body),
            'fixed_bytes': bytes.fromhex(entry['mask']).count(255),
            'candidate_count': len(candidates), 'full_masked_body_match': True,
            'reference_instruction_sha256': hashlib.sha256(body).hexdigest(),
            'newer_instruction_sha256': hashlib.sha256(newer_body).hexdigest(),
            'reference_bytes': entry['bytes'], 'newer_bytes': newer_body.hex(),
            'mask': entry['mask'], 'refs': entry['refs']})
    # Also authenticate the retained pseudocode/assembly, including longjmp containment.
    result['ghidra_evidence'] = [
        {'path': 'tools/ghidra/' + p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
        for p in sorted((folder / 'ghidra').glob('p0_lua_*.txt'))
        if not p.name.endswith('.commands.txt')]
    output = folder / 'p0_lua_binding_evidence.json'
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'Reviewed {len(result["bindings"])} bindings for both authenticated PEs: {output}')


if __name__ == '__main__':
    main()
