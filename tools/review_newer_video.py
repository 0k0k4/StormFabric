"""Offline video binding/layout review. Reads PE files; never loads/runs the game.

Uses retained Ghidra operand contracts plus explicit constructor operand windows.
The constructor mask below is hand-reviewed, not an E8 byte-scan disassembler.
Prints reproducible evidence JSON, or writes it with --output (new evidence only).
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from compare_newer_offline import PE, ROOT, compare


def relative(pe, operand, end):
    return end + struct.unpack('<i', pe.read(operand, 4))[0]


def import_slot(pe, dll, name):
    nt = pe.u32(0x3c)
    table, _ = struct.unpack_from('<II', pe.data, nt + 24 + 112 + 8)

    def string(rva):
        result = bytearray()
        while pe.read(rva, 1) != b'\0':
            result.extend(pe.read(rva, 1))
            rva += 1
        return result.decode('ascii')

    while True:
        lookup, _, _, dll_rva, iat = struct.unpack('<IIIII', pe.read(table, 20))
        if not dll_rva:
            break
        if string(dll_rva).lower() == dll.lower():
            for i in range(10000):
                entry = struct.unpack('<Q', pe.read((lookup or iat) + 8*i, 8))[0]
                if not entry:
                    break
                if not entry & (1 << 63) and string(entry + 2) == name:
                    return iat + 8*i
        table += 20
    raise AssertionError(f'import missing: {dll}!{name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    old = PE(ROOT / 'Stormworks/stormworks64.exe')
    new = PE(Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('STORMWORKS_NEWER_EXE', r'..\Stormworks_Newer\stormworks64.exe')))
    assert old.sha256 == '55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3'
    assert new.sha256 == 'f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699'
    path = Path(__file__).with_name('discovery_contracts.json')
    contracts = json.loads(path.read_text())
    contracts = {c['rva']: c for c in contracts['entries'] + contracts['witnesses']}
    mappings = [(0x74a538, 0x74adf8), (0x6d87d0, 0x6d9041),
                (0x74aa50, 0x74b310), (0x688ec0, 0x689620),
                (0x367c80, 0x366f20), (0xb70ea0, 0xb73178),
                (0xa925f0, import_slot(new, 'OPENGL32.DLL', 'glDrawElements'))]
    assert import_slot(old, 'OPENGL32.DLL', 'glDrawElements') == 0xa925f0
    observations = []

    def window(label, previous, current, size, meaning):
        a, b = old.read(previous, size), new.read(current, size)
        observations.append({'label': label, 'reference_rva': hex(previous),
                             'newer_rva': hex(current), 'size': size,
                             'reference_bytes': a.hex(' '), 'newer_bytes': b.hex(' '),
                             'identical': a == b, 'meaning': meaning})
        return a, b

    bodies = []
    for key, destination in mappings[:5] + [(0x20cf60, 0x20b480)]:
        c = contracts[key]
        assert old.read(c['start'], len(c['bytes']) // 2) == bytes.fromhex(c['bytes'])
        start = destination - (key - c['start'])
        result = compare(new, c, start)
        result.update({'key': hex(key), 'newer_rva': hex(destination)})
        expected = {0x74aa50: [(0x6f, 0x68)], 0x6d87d0: [(7, 0x98)]}.get(key, [])
        a, b, mask = bytes.fromhex(c['bytes']), new.read(start, len(c['bytes']) // 2), bytes.fromhex(c['mask'])
        assert [(i, y) for i, (x, y, m) in enumerate(zip(a, b, mask)) if (x & m) != (y & m)] == expected
        result['reviewed_fixed_changes_only'] = True
        bodies.append(result)
        window('binding_signature_' + hex(key), key, destination, 32,
               'Exact per-build start bytes; body/window comparison is recorded separately.')

    for (call, target), (newcall, newtarget) in [((0x74a538, 0x74aa50), (0x74adf8, 0x74b310)),
                                               ((0x6d87d0, 0x688ec0), (0x6d9041, 0x689620))]:
        for pe, site, dest in [(old, call, target), (new, newcall, newtarget)]:
            assert pe.read(site, 1) == b'\xe8'
            assert relative(pe, site + 1, site + 5) == dest

    # Full native monitor constructor: only these decoded rel32/RIP operands vary.
    # Each tuple is instruction offset, expected opcode, displacement offset, end.
    typed = [(0x17, 'e8', 0x18, 0x1c), (0x1d, '488d05', 0x20, 0x24),
             (0x36, 'e8', 0x37, 0x3b), (0x4b, '488d05', 0x4e, 0x52),
             (0x96, 'e8', 0x97, 0x9b), (0xa2, 'e8', 0xa3, 0xa7),
             (0xbc, '488d05', 0xbf, 0xc3), (0x130, 'e8', 0x131, 0x135),
             (0x13f, '488d15', 0x142, 0x146), (0x14d, 'e8', 0x14e, 0x152),
             (0x161, 'e8', 0x162, 0x166)]
    ctor = (0x365b80, 0x364e20)
    raw = [pe.read(rva, 378) for pe, rva in zip((old, new), ctor)]
    mask = bytearray(b'\xff' * 378)
    operands = []
    for instruction, opcode, displacement, end in typed:
        opcode = bytes.fromhex(opcode)
        assert all(r[instruction:instruction + len(opcode)] == opcode for r in raw)
        mask[displacement:displacement + 4] = b'\0' * 4
        operands.append({'instruction_offset': hex(instruction), 'opcode': opcode.hex(' '),
                         'displacement_offset': hex(displacement),
                         'reference_target': hex(relative(old, ctor[0] + displacement, ctor[0] + end)),
                         'newer_target': hex(relative(new, ctor[1] + displacement, ctor[1] + end))})
    assert all((a & m) == (b & m) for a, b, m in zip(*raw, mask))
    for pe, rva, table, destructor in [(old, ctor[0], 0xb70ea0, 0x367c80),
                                      (new, ctor[1], 0xb73178, 0x366f20)]:
        assert pe.region(rva)['size'] == 378
        assert relative(pe, rva + 0x20, rva + 0x24) == table
        assert struct.unpack('<Q', pe.read(table, 8))[0] == pe.image_base + destructor
    window('monitor_constructor_signal_member', 0x365b80 + 0x8f, 0x364e20 + 0x8f, 12,
           'LEA RCX,[RSI+0x1a8]; CALL native signal constructor; only rel32 changes.')
    a, b = window('monitor_call_signal_and_stack_arguments', 0x74a4c7, 0x74ad87, 0x74a538 - 0x74a4c7,
                  'LEA text +0x4f8, error +0x4f4; ADD R10,0x1a8; stack +0x40 is argument 9; integer argument 8 at +0x38; R9 is handle storage pointer.')
    assert a == b
    a, b = window('gpu_call_resource_unwrap_and_arguments', 0x6d878d, 0x6d8ffe, 0x43,
                  'R8=[RBX], R9=[RBX+8], then R8=[R8+8], R9=[R9+8]; fifth world pointer, sixth/seventh float, eighth packed color on stack. Only engine-owned batch LEA +0x2830 becomes +0x2898.')
    assert [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y] == [(7, 0x30, 0x98)]
    a, b = window('deferred_resource_object_store', 0x20cfd5, 0x20b4f5, 8,
                  'MOV RCX,[RDI+8]; MOV [RCX+8],RAX stores native target object in deferred wrapper +8.')
    assert a == b == bytes.fromhex('488b4f0848894108')
    # Explicit MOV EDX,[RSI+48h] and MOV EDX,[RDI+48h] operand windows.
    # The full GPU contract also keeps every non-relocation byte here fixed.
    for at, opcode in [(0xdb, '8b5648'), (0x127, '8b5748')]:
        a, b = window('gpu_texture_id_operand', 0x688ec0 + at, 0x689620 + at, 3,
                      'Native target texture GLuint load uses +0x48; retained video_fx_gpu.txt identifies glBindTexture consumer.')
        assert a == b == bytes.fromhex(opcode)
    a, b = window('script_draw_permission_set', 0x74ad67, 0x74b627, 9,
                  'MOV word [RBX+0x304],1 sets drawing permission around native onDraw dispatch.')
    assert a == b == bytes.fromhex('66c783040300000100')
    a, b = window('script_draw_permission_clear', 0x74aea9, 0x74b769, 7,
                  'MOV byte [RBX+0x304],0 clears drawing permission after native onDraw dispatch.')
    assert a == b == bytes.fromhex('c6830403000000')
    a, b = window('engine_owned_draw_queue', 0x74aabc, 0x74b37c, 7,
                  'LEA R14,[RDX+0xe50] becomes LEA R14,[RDX+0xe68]; loader delegates this access to native body.')
    assert a == bytes.fromhex('4c8db2500e0000') and b == bytes.fromhex('4c8db2680e0000')
    report = {
        'evidence_kind': 'Offline PE bytes + retained Ghidra contracts/traces, not REA evidence or runtime observation',
        'reproduce': 'python -B StormworksModLoader/tools/review_newer_video.py --output StormworksModLoader/tools/newer_video_bindings.json',
        'sources': [{'path': p.path, 'sha256': p.sha256} for p in (old, new)],
        'contracts_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'bindings': {hex(k): hex(v) for k, v in mappings},
        'body_comparisons': bodies,
        'monitor_constructor': {'reference_rva': hex(ctor[0]), 'newer_rva': hex(ctor[1]),
                                'size': 378, 'fixed_bytes': sum(m == 255 for m in mask),
                                'fixed_mismatches': 0, 'typed_operands': operands,
                                'vtable_write_and_first_destructor_slot_verified': True},
        'windows': observations,
        'direct_layout': {'kMonitorSignal': '0x1a8', 'kResourceObject': '0x8', 'kTargetTexture': '0x48',
                          'unchanged_in_both_reviewed_builds': True,
                          'script_draw_permission': '0x304 (unchanged fixed bytes in full monitor draw contract)',
                          'lua_state_owner_link': 'state-8 -> self; self+8 -> state; parent native Lua layout review owns this binding'},
        'abi': {
            'platform': 'Windows x64',
            'monitor': '11 arguments: pointer,pointer,pointer,pointer,int*,float,float,int,signal*,int*,text*. Argument 8 is integer, not float; wrapper corrected.',
            'gpu': '8 arguments: shader*,batch*,video target*,overlay target*,world*,float,float,uint32 packed color; unchanged.',
            'destroy': 'scalar deleting destructor: RCX=self, EDX=uint32 flags, RAX=self return; 157-byte fixed contract unchanged outside relocations.',
            'hooks': 'Two E8 rel32 calls still decode to reviewed native bodies; vtable first slot corroborated by constructor and disk pointer. GL IAT selected by import name.',
        },
        'engine_owned_changes': ['draw queue +0xe50 -> +0xe68', 'GPU caller batch +0x2830 -> +0x2898',
                                 'post-monitor-call native loop count +0x5b0 -> +0x5c8 (visible in per-build signature; not loader access)',
                                 'monitor update float +0x2468 -> +0x2928 (prior comparison; loader does not dereference it)'],
        'trace_sources': ['tools/ghidra/video_pipeline.txt:1-53,91-113',
                          'tools/ghidra/video_fx_compile.txt:22-83',
                          'tools/ghidra/video_fx_gpu.txt:1-19,37-73',
                          'tools/ghidra/video_fx_resources.txt:118-368'],
        'limits': ['Static compatibility evidence, not observed rendering/capture execution.',
                   'Only the two exact SHA-256 executables were reviewed; no generic build whitelist.',
                   'Central engine bindings must gate exact disk identity, per-build byte signatures, and table expectations before installation.',
                   'OpenGL uniform/attribute names and capabilities remain checked on native render context at runtime; no game was launched or attached.']}
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
        print(f'Wrote {args.output}: all offline video assertions passed')
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
