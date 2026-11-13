#!/usr/bin/env python3
"""Generate src/lua_bindings.generated.h from the read-only modding kit.

Source of truth:
  stormworks-modding-kit/lua-api/lua_bindings.json
  stormworks-modding-kit/lua-api/annotated-bindings/*.c   (arity, when present)

The output is deterministic: running the script twice produces byte-identical
files.  Do not edit the generated header by hand.

Arity convention
----------------
The kit annotates some bindings with several accepted argument counts, e.g.
``// ARITY: 1 | 4``.  A single int8_t cannot represent a set, so the generated
table stores the *smallest* accepted count (the conservative lower bound) and
``-1`` when the binding has no annotation (or is annotated ``variable``).

RVA convention
--------------
``rva`` is image-base-relative (image base 0x140000000).  The kit contains 12
Lua *value* entries (``_G``, ``_VERSION``, ``pi``, ...) whose function pointer is
NULL; those are emitted with ``rva == 0`` and must not be called.  No address is
ever guessed.
"""

from __future__ import annotations

import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "stormworks-modding-kit", "lua-api")
JSON_PATH = os.path.join(KIT, "lua_bindings.json")
ANNOTATED_DIR = os.path.join(KIT, "annotated-bindings")
OUT_PATH = os.path.join(ROOT, "src", "lua_bindings.generated.h")

TABLES = ("base", "mc", "mission", "other", "stdlib")
ARITY_RE = re.compile(r"^//\s*ARITY:\s*(.+)$", re.MULTILINE)
FILE_RE = re.compile(r"^0x([0-9a-fA-F]+)__lua_(.+)\.c$")


def load_bindings():
    with open(JSON_PATH, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    entries = []
    for array in data["arrays"]:
        table = array.get("kind")
        if table not in TABLES:
            raise SystemExit("unknown table kind in kit: %r" % (table,))
        for entry in array["entries"]:
            name = entry["name"]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                raise SystemExit("unexpected binding name: %r" % (name,))
            func = entry.get("func")
            entries.append(
                {
                    "name": name,
                    "table": table,
                    "rva": 0 if func is None else int(func),
                }
            )
    return entries


def load_arities():
    """Map RVA -> smallest annotated arity.  Missing/variable -> -1."""
    arities = {}
    for path in sorted(glob.glob(os.path.join(ANNOTATED_DIR, "*.c"))):
        match = FILE_RE.match(os.path.basename(path))
        if not match:
            continue
        rva = int(match.group(1), 16)
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        arity_match = ARITY_RE.search(text)
        if not arity_match:
            arities[rva] = -1
            continue
        # Only the part before the bracketed evidence is the arity list.
        head = arity_match.group(1).split("[", 1)[0]
        numbers = [int(value) for value in re.findall(r"\d+", head)]
        arities[rva] = min(numbers) if numbers else -1
    return arities


def build_rows(entries, arities):
    rows = []
    for entry in entries:
        arity = arities.get(entry["rva"], -1) if entry["rva"] else -1
        rows.append((entry["name"], entry["table"], entry["rva"], arity))
    return rows


def render(rows):
    total = len(rows)
    per_table = {table: 0 for table in TABLES}
    typed = 0
    for _name, table, _rva, arity in rows:
        per_table[table] += 1
        if arity >= 0:
            typed += 1

    lines = []
    lines.append("// GÉNÉRÉ — ne pas éditer. Source : stormworks-modding-kit/lua-api/lua_bindings.json")
    lines.append("// Régénérer avec : python tools/gen_lua_bindings.py")
    lines.append("//")
    lines.append("// rva est relative à l'image (base 0x140000000). rva == 0 marque une")
    lines.append("// entrée valeur Lua sans pointeur de fonction (ne pas appeler).")
    lines.append("// arity = plus petit nombre d'arguments accepté, -1 si non annoté.")
    lines.append("#pragma once")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("#ifndef SW_LUA_BINDING_INFO_DEFINED")
    lines.append("#define SW_LUA_BINDING_INFO_DEFINED")
    lines.append("struct SwLuaBindingInfo {")
    lines.append("    const char* name;      // nom exact du binding dans le jeu")
    lines.append('    const char* table;     // "base" | "mc" | "mission" | "other" | "stdlib"')
    lines.append("    uint32_t rva;          // RVA (base d'image 0x140000000), 0 = pas de fonction")
    lines.append("    int8_t arity;          // -1 = inconnue")
    lines.append("};")
    lines.append("#endif")
    lines.append("")
    lines.append("inline constexpr SwLuaBindingInfo kLuaBindings[] = {")
    for name, table, rva, arity in rows:
        lines.append('    {"%s", "%s", 0x%Xu, %d},' % (name, table, rva, arity))
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr size_t kLuaBindingCount = %d;" % total)
    for table in TABLES:
        lines.append(
            "inline constexpr size_t kLuaBindingCount%s = %d;"
            % (table.capitalize(), per_table[table])
        )
    lines.append("inline constexpr size_t kLuaBindingTypedCount = %d;" % typed)
    lines.append("")
    lines.append("constexpr bool SwLuaTableEquals(const char* a, const char* b) {")
    lines.append("    if (!a || !b) return false;")
    lines.append("    while (*a && *a == *b) { ++a; ++b; }")
    lines.append("    return *a == *b;")
    lines.append("}")
    lines.append("")
    lines.append("constexpr size_t SwLuaCountTable(const char* table) {")
    lines.append("    size_t count = 0;")
    lines.append("    for (size_t i = 0; i < kLuaBindingCount; ++i)")
    lines.append("        if (SwLuaTableEquals(kLuaBindings[i].table, table)) ++count;")
    lines.append("    return count;")
    lines.append("}")
    lines.append("")
    lines.append("constexpr size_t SwLuaCountTyped() {")
    lines.append("    size_t count = 0;")
    lines.append("    for (size_t i = 0; i < kLuaBindingCount; ++i)")
    lines.append("        if (kLuaBindings[i].arity >= 0) ++count;")
    lines.append("    return count;")
    lines.append("}")
    lines.append("")
    lines.append(
        "static_assert(sizeof(kLuaBindings) / sizeof(kLuaBindings[0]) == kLuaBindingCount,"
    )
    lines.append('              "kLuaBindings size does not match kLuaBindingCount");')
    for table in TABLES:
        lines.append(
            'static_assert(SwLuaCountTable("%s") == kLuaBindingCount%s,'
            % (table, table.capitalize())
        )
        lines.append('              "table count mismatch: %s");' % table)
    lines.append(
        "static_assert(SwLuaCountTyped() == kLuaBindingTypedCount,"
    )
    lines.append('              "typed binding count mismatch");')
    lines.append("")
    return "\n".join(lines)


def main():
    entries = load_bindings()
    arities = load_arities()
    rows = build_rows(entries, arities)
    output = render(rows)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(output)
    print(
        "wrote %s: %d bindings (%s), %d typed"
        % (
            os.path.relpath(OUT_PATH, ROOT),
            len(rows),
            ", ".join(
                "%s=%d" % (table, sum(1 for row in rows if row[1] == table))
                for table in TABLES
            ),
            sum(1 for row in rows if row[3] >= 0),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
