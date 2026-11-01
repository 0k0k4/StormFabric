"""Inventory the reviewed loader's dependencies BEFORE migrating address expressions.
The JSON is retained input for Ghidra, not an automatically trusted new-build profile.
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
functions = {}

def add(rva, group, name=None):
    entry = functions.setdefault(rva, {"rva": rva, "name": name or f"native_{rva:x}", "groups": []})
    if group not in entry["groups"]:
        entry["groups"].append(group)
    if name:
        entry["name"] = name

game = (ROOT / "src/game.cpp").read_text()
for name, rva in re.findall(r'constexpr FuncSpec (\w+) = \{"[^"]*", (0x[0-9a-f]+)', game):
    add(int(rva, 16), "notifications" if name.startswith(("kHud", "kEditor", "kUiTeardown")) else "ui", name)
for rva in (0x241fb0, 0x1dd500):
    add(rva, "ui")

core = {0x957478, 0x17cb00, 0x9b15a0, 0x1e3ee0, 0x097920, 0x0be4a0,
        0x097a10, 0x0ba830, 0x0982c0, 0x098050, 0x0983d0, 0x2e5ad0, 0x2e6050}
editor = {0x63f300, 0x63f420, 0x24b140, 0x25e730, 0x9253e0, 0x25ed10, 0x24b650,
          0x63f550, 0x63fbc0, 0x63d980}
lua = (ROOT / "src/native_lua.cpp").read_text()
for value in re.findall(r"g_base\s*\+\s*(0x[0-9a-fA-F]+)", lua):
    rva = int(value, 16)
    if rva >= 0xa92000:
        continue
    add(rva, "lua" if rva in core else "editor" if rva in editor else "qol")
for rva in core:
    add(rva, "lua")

for rva in (0x74aa50, 0x688ec0, 0x367c80, 0x366e90, 0x20cf60, 0x366010):
    add(rva, "video")

# Validate slots, including their position, class RTTI and the native method bodies.
tables = [
    (0xb88818, "ui", [1, 30]), (0xb904d8, "ui", [3]),
    (0xb850a8, "ui", [0, 1, 2, 3, 4, 30]),
    (0xb6dac0, "lua", [1, 3]),
    (0xb67728, "editor", [1, 3]), (0xae9a20, "editor", [1]),
    (0xb423c0, "notifications", [0, 1, 2, 3]),
    (0xb71d60, "notifications", [0, 1, 2, 3]),
    (0xb70ea0, "video", [0]),
]
result = {
    "reference_sha256": "55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3",
    "functions": sorted(functions.values(), key=lambda e: e["rva"]),
    "tables": [{"rva": r, "group": g, "slots": s} for r,g,s in tables],
    "windows": [
        {"rva": 0x74a538, "start": 0x74a4d5, "length": 104, "name": "monitor_onDraw_call", "groups": ["video"]},
        {"rva": 0x6d87d0, "start": 0x6d878d, "length": 72, "name": "native_gpu_monitor_call", "groups": ["video"]},
    ],
}
(ROOT / "tools/discovery_symbols.json").write_text(json.dumps(result, indent=2) + "\n")
print(f"Inventoried {len(functions)} native functions, {len(tables)} vtables and two validated call windows")
