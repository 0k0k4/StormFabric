#!/usr/bin/env python3
"""Generate src/component_attrs.generated.h from the read-only modding kit.

Source of truth:
  stormworks-modding-kit/formats/component_physics_record.json

The kit reconstructs the 120 fields of ``component_physics_record`` (the record
filled by ``component_physics_record_parse`` @ 0x3168d0) by aligning the key
strings read by the parser with the destination slots.  Each entry carries the
proven offset, the proven width, the C type and the XML kind.

The output is deterministic: running the script twice produces byte-identical
files.  Do not edit the generated header by hand.

Classification
--------------
The kit README distinguishes three groups:
  * 80 XML *attributes*   (``in_xml == true``)
  * 38 XML *elements*     (vector fields: ``kind`` is ``ivec3`` / ``fvec3``)
  *  2 *internal* fields  (``max_item_capacity``, ``lua_filename``)

``SwComponentAttrInfo`` only has a single ``isElement`` flag, so the two
internal fields are emitted with ``isElement == false`` (they are neither an XML
attribute nor an XML element).  The generated header exposes the three counts
separately so a mod can still tell them apart.

Widths are taken verbatim from the kit.  ``flags`` is 8 bytes (the kit corrected
it from 4 after decoding ``mov [r15],rax``); never assume a width from the type.
"""

from __future__ import annotations

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSON_PATH = os.path.join(
    ROOT, "stormworks-modding-kit", "formats", "component_physics_record.json"
)
OUT_PATH = os.path.join(ROOT, "src", "component_attrs.generated.h")

# kind -> (SwComponentAttrType, expected width in bytes)
KIND_TYPES = {
    "f32": ("SW_ATTR_FLOAT", 4),
    "i32": ("SW_ATTR_INT", 4),
    "enum_i32": ("SW_ATTR_INT", 4),
    "bool": ("SW_ATTR_BOOL", 1),
    "string": ("SW_ATTR_STRING", 16),
    "u64": ("SW_ATTR_UINT64", 8),
    "fvec3": ("SW_ATTR_FLOAT3", 12),
    "ivec3": ("SW_ATTR_INT3", 12),
}

ELEMENT_KINDS = ("fvec3", "ivec3")

# ctype -> kind, used as a cross-check so a kit edit cannot silently change the
# meaning of a field without the generator noticing.
CTYPE_KINDS = {
    "float": ("f32", "fvec3"),
    "int32_t": ("i32", "enum_i32", "ivec3"),
    "bool": ("bool",),
    "string_t": ("string",),
    "uint64_t": ("u64",),
}


def load_fields():
    with open(JSON_PATH, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    fields = data["fields"]
    if not fields:
        raise SystemExit("kit contains no component fields")
    return fields


def classify(field):
    kind = field["kind"]
    ctype = field["ctype"]
    if kind not in KIND_TYPES:
        raise SystemExit("unknown field kind in kit: %r (%s)" % (kind, field["attr"]))
    if ctype not in CTYPE_KINDS or kind not in CTYPE_KINDS[ctype]:
        raise SystemExit(
            "ctype/kind mismatch in kit: %r ctype=%r kind=%r"
            % (field["attr"], ctype, kind)
        )
    type_name, expected_width = KIND_TYPES[kind]
    width = int(field["width"])
    if width != expected_width:
        raise SystemExit(
            "width mismatch in kit: %r kind=%r width=%d expected=%d"
            % (field["attr"], kind, width, expected_width)
        )
    is_element = kind in ELEMENT_KINDS
    in_xml = bool(field["in_xml"])
    if is_element and in_xml:
        raise SystemExit("vector field marked in_xml: %r" % (field["attr"],))
    return {
        "name": field["attr"],
        "offset": int(field["offset"]),
        "width": width,
        "type": type_name,
        "is_element": is_element,
        "in_xml": in_xml,
    }


def build_rows(fields):
    rows = [classify(field) for field in fields]
    rows.sort(key=lambda row: row["offset"])
    # The kit guarantees no slot clashes; enforce it here so a bad regeneration
    # fails loudly instead of producing an overlapping table.
    for previous, current in zip(rows, rows[1:]):
        if previous["offset"] + previous["width"] > current["offset"]:
            raise SystemExit(
                "overlapping fields in kit: %r [0x%x,+%d) and %r [0x%x,+%d)"
                % (
                    previous["name"],
                    previous["offset"],
                    previous["width"],
                    current["name"],
                    current["offset"],
                    current["width"],
                )
            )
    names = [row["name"] for row in rows]
    if len(set(names)) != len(names):
        raise SystemExit("duplicate field names in kit")
    return rows


def render(rows):
    total = len(rows)
    attributes = sum(1 for row in rows if row["in_xml"])
    elements = sum(1 for row in rows if row["is_element"])
    internal = total - attributes - elements
    widths = {}
    for row in rows:
        widths[row["width"]] = widths.get(row["width"], 0) + 1
    max_end = max(row["offset"] + row["width"] for row in rows)

    lines = []
    lines.append(
        "// GÉNÉRÉ — ne pas éditer. Source : "
        "stormworks-modding-kit/formats/component_physics_record.json"
    )
    lines.append("// Régénérer avec : python tools/gen_component_attrs.py")
    lines.append("//")
    lines.append("// 120 champs de component_physics_record (parseur 0x3168d0).")
    lines.append("// Les largeurs viennent du kit, jamais d'une supposition : flags = 8 octets.")
    lines.append("// isElement distingue un élément XML (vecteur) d'un attribut scalaire ;")
    lines.append("// les 2 champs internes (max_item_capacity, lua_filename) ont isElement=false.")
    lines.append("#pragma once")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("#include \"../include/sw_sdk_p0.h\"")
    lines.append("")
    lines.append("inline constexpr SwComponentAttrInfo kComponentAttrs[] = {")
    for row in rows:
        lines.append(
            '    {"%s", 0x%Xu, %d, %s, %s},'
            % (
                row["name"],
                row["offset"],
                row["width"],
                row["type"],
                "true" if row["is_element"] else "false",
            )
        )
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr size_t kComponentAttrCount = %d;" % total)
    lines.append(
        "inline constexpr size_t kComponentAttrAttributeCount = %d;  // attributs XML"
        % attributes
    )
    lines.append(
        "inline constexpr size_t kComponentAttrElementCount = %d;    // éléments XML (vecteurs)"
        % elements
    )
    lines.append(
        "inline constexpr size_t kComponentAttrInternalCount = %d;   // champs internes"
        % internal
    )
    lines.append(
        "inline constexpr size_t kComponentAttrMaxEnd = 0x%Xu;       // fin du dernier champ"
        % max_end
    )
    for width in sorted(widths):
        lines.append(
            "inline constexpr size_t kComponentAttrWidth%dCount = %d;"
            % (width, widths[width])
        )
    lines.append("")
    lines.append("// Largeur attendue d'un type scalaire/vecteur de la table.")
    lines.append("constexpr uint8_t SwComponentAttrWidthOf(uint8_t type) {")
    lines.append("    switch (type) {")
    lines.append("        case SW_ATTR_FLOAT:  return 4;")
    lines.append("        case SW_ATTR_INT:    return 4;")
    lines.append("        case SW_ATTR_BOOL:   return 1;")
    lines.append("        case SW_ATTR_STRING: return 16;")
    lines.append("        case SW_ATTR_UINT64: return 8;")
    lines.append("        case SW_ATTR_FLOAT3: return 12;")
    lines.append("        case SW_ATTR_INT3:   return 12;")
    lines.append("        default:             return 0;")
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("// Vrai pour un type que componentAttrRead/Write acceptent (scalaire numérique).")
    lines.append("constexpr bool SwComponentAttrIsScalarNumeric(uint8_t type) {")
    lines.append("    return type == SW_ATTR_FLOAT || type == SW_ATTR_INT ||")
    lines.append("           type == SW_ATTR_BOOL || type == SW_ATTR_UINT64;")
    lines.append("}")
    lines.append("")
    lines.append("constexpr bool SwComponentAttrsConsistent() {")
    lines.append("    for (size_t i = 0; i < kComponentAttrCount; ++i) {")
    lines.append("        if (kComponentAttrs[i].width != SwComponentAttrWidthOf(kComponentAttrs[i].type))")
    lines.append("            return false;")
    lines.append("        if (kComponentAttrs[i].offset + kComponentAttrs[i].width > kComponentAttrMaxEnd)")
    lines.append("            return false;")
    lines.append("        if (i > 0 && kComponentAttrs[i - 1].offset + kComponentAttrs[i - 1].width >")
    lines.append("                         kComponentAttrs[i].offset)")
    lines.append("            return false;")
    lines.append("    }")
    lines.append("    return true;")
    lines.append("}")
    lines.append("")
    lines.append("constexpr size_t SwComponentAttrCountElements() {")
    lines.append("    size_t count = 0;")
    lines.append("    for (size_t i = 0; i < kComponentAttrCount; ++i)")
    lines.append("        if (kComponentAttrs[i].isElement) ++count;")
    lines.append("    return count;")
    lines.append("}")
    lines.append("")
    lines.append("constexpr size_t SwComponentAttrCountWidth(uint8_t width) {")
    lines.append("    size_t count = 0;")
    lines.append("    for (size_t i = 0; i < kComponentAttrCount; ++i)")
    lines.append("        if (kComponentAttrs[i].width == width) ++count;")
    lines.append("    return count;")
    lines.append("}")
    lines.append("")
    lines.append(
        "static_assert(sizeof(kComponentAttrs) / sizeof(kComponentAttrs[0]) == kComponentAttrCount,"
    )
    lines.append('              "kComponentAttrs size does not match kComponentAttrCount");')
    lines.append(
        "static_assert(kComponentAttrAttributeCount + kComponentAttrElementCount +"
    )
    lines.append(
        "                  kComponentAttrInternalCount == kComponentAttrCount,"
    )
    lines.append('              "attribute/element/internal counts do not add up");')
    lines.append(
        "static_assert(SwComponentAttrCountElements() == kComponentAttrElementCount,"
    )
    lines.append('              "element count mismatch");')
    for width in sorted(widths):
        lines.append(
            "static_assert(SwComponentAttrCountWidth(%d) == kComponentAttrWidth%dCount,"
            % (width, width)
        )
        lines.append('              "width %d count mismatch");' % width)
    lines.append(
        "static_assert(SwComponentAttrsConsistent(),"
    )
    lines.append('              "component attribute table is inconsistent");')
    lines.append("")
    return "\n".join(lines)


def main():
    rows = build_rows(load_fields())
    output = render(rows)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(output)
    widths = {}
    for row in rows:
        widths[row["width"]] = widths.get(row["width"], 0) + 1
    print(
        "wrote %s: %d fields (%d attributes, %d elements, %d internal), widths %s"
        % (
            os.path.relpath(OUT_PATH, ROOT),
            len(rows),
            sum(1 for row in rows if row["in_xml"]),
            sum(1 for row in rows if row["is_element"]),
            len(rows)
            - sum(1 for row in rows if row["in_xml"])
            - sum(1 for row in rows if row["is_element"]),
            ", ".join("%d=%d" % (width, widths[width]) for width in sorted(widths)),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
