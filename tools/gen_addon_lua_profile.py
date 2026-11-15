"""Generate the addon/mission Lua VM profile for the newer hash-pinned executable.

Only the newer reviewed build (v1.15.23) is supported for addon VMs; the older
reference build keeps this capability disabled. Never loads or runs the target.

The c_playlist_lua_script vtable is identified by exact RTTI. Slot 1 closes the
Lua state at self+8, slot 3 registers the game's bindings on it. Their complete
bodies are retained so the loader can refuse a changed or already hooked image.

  python tools/gen_addon_lua_profile.py [path/to/stormworks64.exe]
or set STORMWORKS_NEWER_EXE. Standard library only.
"""
import os
import struct
import sys
from pathlib import Path

from compare_newer_offline import PE

NEW_HASH = 'f9206d85c82f4d02fd0ac391781d19c5c68394a9ffc48accca0e1f5966db8699'
TYPE = '.?AUc_playlist_lua_script@@'
REGISTER_LIBRARY = 0x1e22d0  # reviewed newer-build RVA of loader key 0x1e3ee0
BASE_PCALL = 0x9cc60        # the `pcall` entry of the base library (luaB_pcall)
INDEX2ADDR = 0x97920        # reviewed index2addr, identical RVA in both builds
ON_TICK_DISPATCHER = 0x59c4e0  # addon onTick dispatch (docs/p0_world_evidence.md, new RVA)
ROOT = Path(__file__).resolve().parents[1]


def vtables(pe):
    found = []
    for _, va, _, raw, size, flags in pe.sections:
        if not flags & 0x40000000 or flags & 0xa0000000:
            continue
        blob = pe.data[raw:raw + size]
        for at in range(0, size - 8, 8):
            col = struct.unpack_from('<Q', blob, at)[0] - pe.image_base
            try:
                sig, offset, _, typ, _, self_rva = struct.unpack('<6I', pe.read(col, 24))
                if sig == 1 and not offset and col == self_rva and \
                        pe.read(typ + 16, len(TYPE) + 1) == TYPE.encode() + b'\0':
                    found.append(va + at + 8)
            except (ValueError, struct.error):
                continue
    return found


def body(pe, rva):
    region = pe.region(rva)
    assert region and int(region['begin'], 16) == rva, (hex(rva), region)
    return pe.read(rva, region['size'])


def literal(name, data):
    rows = [', '.join(f'0x{b:02x}' for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f'static const unsigned char {name}[] = {{\n    ' + ',\n    '.join(rows) + '\n};'


def main():
    pe = PE(Path(sys.argv[1] if len(sys.argv) > 1 else os.environ['STORMWORKS_NEWER_EXE']))
    assert pe.sha256 == NEW_HASH, 'unsupported executable; review before porting'
    tables = vtables(pe)
    assert len(tables) == 1, tables
    table = tables[0]
    close, init = (struct.unpack('<Q', pe.read(table + i * 8, 8))[0] - pe.image_base for i in (1, 3))
    close_body, init_body = body(pe, close), body(pe, init)
    # Layout witnesses the loader relies on: the state lives at self+8 in both
    # methods, and slot 3 registers tables through the reviewed helper.
    assert close_body[6:13] == bytes.fromhex('488bd9488b4908'), 'close: mov rbx,rcx; mov rcx,[rcx+8]'
    assert bytes.fromhex('48c7430800000000') in close_body, 'close: mov qword [rbx+8],0'
    assert bytes.fromhex('488bd9488b4108488948f8') in init_body[:0x60], 'init: state=[self+8]; [state-8]=self'
    calls = [c for c in pe.calls_to(REGISTER_LIBRARY) if init <= c < init + len(init_body)]
    assert len(calls) == 4, calls
    # lua_pcallk(L, nargs, nresults, errfunc, ctx, k): the only protected-call
    # entry of the Lua core. Identified as the call luaB_pcall makes with
    # nresults = LUA_MULTRET and errfunc = 0; its body resolves errfunc through
    # the reviewed index2addr and tests the continuation k before luaD_pcall.
    pcall_body = body(pe, BASE_PCALL)
    at = pcall_body.index(bytes.fromhex('458d41ff'))  # lea r8d,[r9-1] : nresults = -1
    call = pcall_body.index(bytes([0xe8]), at)
    assert bytes.fromhex('4533c9') in pcall_body[:at], 'luaB_pcall: xor r9d,r9d (errfunc 0)'
    pcallk = BASE_PCALL + call + 5 + struct.unpack_from('<i', pcall_body, call + 1)[0]
    first = pe.region(pcallk)
    assert first and int(first['begin'], 16) == pcallk
    # The function continues in later unwind chunks: retain everything up to
    # the end of the chunk holding its farthest near-conditional branch target.
    branch = pe.read(pcallk, first['size'])
    reach = max(pcallk + i + 6 + struct.unpack_from('<i', branch, i + 2)[0]
                for i in range(len(branch) - 6) if branch[i] == 0x0f and branch[i + 1] in (0x83, 0x84, 0x87))
    span_end = int(pe.region(reach)['end'], 16)
    assert pcallk + first['size'] <= reach < span_end and span_end - pcallk < 0x400, (hex(reach), hex(span_end))
    pcallk_body = pe.read(pcallk, span_end - pcallk)
    assert [c for c in pe.calls_to(INDEX2ADDR) if pcallk <= c < span_end], 'pcallk resolves errfunc through index2addr'

    # Simulation tick: the server update that runs every addon's onTick.
    # The onTick dispatcher builds the "onTick" name and is called from exactly
    # one site; the function holding that site is the server update, called from
    # exactly three sites. The loader redirects those three rel32 operands.
    dispatcher_body = body(pe, ON_TICK_DISPATCHER)
    names = [ON_TICK_DISPATCHER + i + 7 + struct.unpack_from('<i', dispatcher_body, i + 3)[0]
             for i in range(len(dispatcher_body) - 7) if dispatcher_body[i:i + 3] == bytes.fromhex('488d15')]
    assert any(pe.read(n, 7) == b'onTick\0' for n in names), 'dispatcher builds "onTick"'
    dispatch_sites = pe.calls_to(ON_TICK_DISPATCHER)
    assert len(dispatch_sites) == 1, dispatch_sites
    update = int(pe.region(dispatch_sites[0])['begin'], 16)
    update_sites = sorted(pe.calls_to(update))
    assert len(update_sites) == 3, update_sites
    # Loop witness: [script+0x78]=1, game ticks from S->vf+0x450, dispatch, [script+0x78]=0.
    loop = dispatch_sites[0] - 0x41
    loop_bytes = pe.read(loop, 0x5e)
    assert loop_bytes[0x1f:0x23] == bytes.fromhex('c6477801'), 'mov byte [rdi+0x78],1'
    assert loop_bytes[0x2a:0x30] == bytes.fromhex('ff9250040000'), 'call [rdx+0x450]'
    assert loop_bytes[0x46:0x4a] == bytes.fromhex('c6477800'), 'mov byte [rdi+0x78],0'
    prologue = pe.read(update, 0x40)
    assert prologue[:3] == bytes.fromhex('488bc4'), 'mov rax,rsp'

    # Game events: every addon callback (onPlayerJoin, onVehicleSpawn...) is called
    # through one helper call_by_name(script, EStr* name, ArgRing* args). It is the
    # callee the onTick dispatcher invokes after building the "onTick" name; its body
    # walks a ring of 0xb8-byte typed values through a six-way jump table.
    candidates = set()
    for i in range(len(dispatcher_body) - 5):
        if dispatcher_body[i] == 0xe8:
            callee = ON_TICK_DISPATCHER + i + 5 + struct.unpack_from('<i', dispatcher_body, i + 1)[0]
            region = pe.region(callee)
            if not region or int(region['begin'], 16) != callee:
                continue
            callee_body = body(pe, callee)
            if bytes.fromhex('83f8050f87') in callee_body and bytes.fromhex('4c69c1b8000000') in callee_body:
                candidates.add(callee)
    assert len(candidates) == 1, [hex(c) for c in candidates]
    call_by_name = candidates.pop()
    call_by_name_body = body(pe, call_by_name)
    assert call_by_name % 16 == 0, 'entry is 16-byte aligned (atomic 8-byte patch)'
    assert call_by_name_body[:5] == bytes.fromhex('48895c2408'), 'entry: mov [rsp+8],rbx (5 bytes)'
    assert call_by_name_body[5:8] == bytes.fromhex('48896c'), 'next instruction starts at +5'
    assert len(pe.calls_to(call_by_name)) == 50, 'callers changed; review the callback list'
    lines = [
        '// Generated by tools/gen_addon_lua_profile.py; do not edit by hand.',
        '// Newer hash-pinned build only (SHA-256 f9206d85...); no other build is supported.',
        '#pragma once',
        f'constexpr uint32_t kAddonLuaVtable = 0x{table:x}; // {TYPE}',
        f'constexpr uint32_t kAddonLuaClose = 0x{close:x};  // slot 1',
        f'constexpr uint32_t kAddonLuaInit = 0x{init:x};   // slot 3',
        literal('kAddonLuaCloseBytes', close_body),
        literal('kAddonLuaInitBytes', init_body),
        '// lua_pcallk(L, nargs, nresults, errfunc, ctx, k), complete body including its unwind chunks.',
        f'constexpr uint32_t kLuaPcallk = 0x{pcallk:x};',
        literal('kLuaPcallkBytes', pcallk_body),
        '// Server update running every addon onTick; its three direct call sites (E8 rel32).',
        f'constexpr uint32_t kSimUpdate = 0x{update:x};',
        'constexpr uint32_t kSimUpdateSites[] = {' + ', '.join(f'0x{s:x}' for s in update_sites) + '};',
        literal('kSimUpdatePrologue', prologue),
        f'constexpr uint32_t kSimUpdateLoop = 0x{loop:x}; // addon onTick loop witness',
        literal('kSimUpdateLoopBytes', loop_bytes),
        '// call_by_name(script, EStr* name, ArgRing* args): every Lua callback goes through it.',
        f'constexpr uint32_t kCallByName = 0x{call_by_name:x};',
        literal('kCallByNameBytes', call_by_name_body),
        f'static_assert(sizeof(kAddonLuaCloseBytes) == {len(close_body)} && sizeof(kAddonLuaInitBytes) == {len(init_body)}, "addon Lua profile");',
    ]
    (ROOT / 'src/addon_lua_profile.generated.h').write_text('\n'.join(lines) + '\n', newline='\n')
    print(f'Addon Lua profile: vtable {table:#x}, close {close:#x} ({len(close_body)} bytes), '
          f'init {init:#x} ({len(init_body)} bytes); lua_pcallk {pcallk:#x} ({len(pcallk_body)} bytes); '
          f'update {update:#x} called from {", ".join(hex(s) for s in update_sites)}; '
          f'call_by_name {call_by_name:#x} ({len(call_by_name_body)} bytes)')


if __name__ == '__main__':
    main()
