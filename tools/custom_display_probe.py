"""Build a native component-import probe, NOT a configurable Custom Display.

No injection, installation, game launch or changes to game files. Archive format
reconstructed from static reference evidence; import, placement and native video
were subsequently reported working by the user on the reference installation.
Stock monitor geometry/resolution are deliberately left unchanged.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET


REFERENCE_EXE_SHA256 = "55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3"
PROBE_ID = "swml_cd_probe_v1_1x1"
PROBE_NAME = "SWML Display Probe 1x1"
# Offline tool bounds, NOT engine/GPU limits or a runtime compatibility policy.
MAX_ARCHIVE_BYTES = 8 * 1024 * 1024
MAX_STRING_BYTES = 1024 * 1024
MAX_RESOURCES = 64


@dataclass(frozen=True)
class ComponentArchive:
    definition_id: str
    xml: bytes
    resources: tuple = ()


def _definition_id(value):
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,95}", value):
        raise ValueError("Definition identity must be a short lowercase ASCII basename")


def _cstring(value):
    if not value or b"\0" in value or len(value) > MAX_STRING_BYTES:
        raise ValueError("Empty, unterminated/embedded-NUL or oversized string")
    return value + b"\0"


def _resource_name(value):
    # Paths stay virtual: the decoder never extracts files from an archive.
    if (not value or len(value) > 240 or "\\" in value or ":" in value
            or value.startswith("/") or any(p in ("", ".", "..") for p in value.split("/"))
            or any(ord(c) < 32 or ord(c) > 126 for c in value)):
        raise ValueError("Unsafe resource name")


def encode_archive(archive):
    """Version-1 internal blob plus the single-file uint32 length envelope."""
    _definition_id(archive.definition_id)
    if len(archive.resources) > MAX_RESOURCES:
        raise ValueError("Too many resources for the offline tool")
    parts = [struct.pack("<I", 1), _cstring((archive.definition_id + ".xml").encode("ascii")),
             _cstring(archive.xml), struct.pack("<H", len(archive.resources))]
    size = sum(map(len, parts))
    names = set()
    for name, data in archive.resources:
        _resource_name(name)
        if not data:
            raise ValueError("Empty resource blobs are not accepted by this offline subset")
        if name.casefold() in names:
            raise ValueError("Duplicate resource name")
        names.add(name.casefold())
        size += len(name) + 1 + 4 + len(data)
        if size > MAX_ARCHIVE_BYTES:
            raise ValueError("Archive exceeds the offline tool budget")
        parts.extend((_cstring(name.encode("ascii")), struct.pack("<I", len(data)), data))
    blob = b"".join(parts)
    if len(blob) > MAX_ARCHIVE_BYTES:
        raise ValueError("Archive exceeds the offline tool budget")
    return struct.pack("<I", len(blob)) + blob


def decode_archive(data):
    """Strict offline subset: no legacy fallback, partial parsing or extraction."""
    if len(data) < 4 or len(data) > MAX_ARCHIVE_BYTES + 4:
        raise ValueError("Missing or oversized archive envelope")
    if struct.unpack_from("<I", data)[0] != len(data) - 4:
        raise ValueError("Archive envelope length mismatch")
    cursor = 4

    def take(size):
        nonlocal cursor
        if size > len(data) - cursor:
            raise ValueError("Truncated archive")
        result = data[cursor:cursor + size]
        cursor += size
        return result

    def cstring():
        nonlocal cursor
        end = data.find(b"\0", cursor, min(len(data), cursor + MAX_STRING_BYTES + 1))
        if end < 0 or end == cursor:
            raise ValueError("Missing, empty or oversized archive string")
        result = data[cursor:end]
        cursor = end + 1
        return result

    if struct.unpack("<I", take(4))[0] != 1:
        raise ValueError("Unsupported archive version (no guessed legacy fallback)")
    filename = cstring().decode("ascii")
    if not filename.endswith(".xml"):
        raise ValueError("Expected a definition XML basename")
    identity = filename[:-4]
    _definition_id(identity)
    xml = cstring()
    count = struct.unpack("<H", take(2))[0]
    if count > MAX_RESOURCES:
        raise ValueError("Too many resources for the offline tool")
    names = set()
    resources = []
    for _ in range(count):
        name = cstring().decode("ascii")
        _resource_name(name)
        if name.casefold() in names:
            raise ValueError("Duplicate resource name")
        names.add(name.casefold())
        size = struct.unpack("<I", take(4))[0]
        if not size:
            raise ValueError("Empty resource blobs are not accepted by this offline subset")
        resources.append((name, take(size)))
    if cursor != len(data):
        raise ValueError("Trailing archive bytes")
    return ComponentArchive(identity, xml, tuple(resources))


def archive_identity(data):
    # Native content identity, NOT executable authentication or a security hash.
    decode_archive(data)
    return hashlib.md5(data[4:]).hexdigest()


def prepare_probe_xml(source):
    """Change only the stock definition's display name; preserve native XML bytes.

    Stock files contain numeric attribute names (e.g. physics_shape_rotation 00)
    that strict XML parsers reject. Parse only the valid root opening tag instead
    of silently rewriting or 'repairing' the game's native XML dialect.
    """
    if len(source) > MAX_STRING_BYTES or b"\0" in source:
        raise ValueError("Invalid or oversized stock definition")
    source.decode("utf-8")
    if (b"<!DOCTYPE" in source.upper() or b"<!ENTITY" in source.upper()
            or not source.rstrip().endswith(b"</definition>")):
        raise ValueError("Unexpected stock definition structure")
    match = re.search(rb"<definition\s[^>]*>", source)
    if not match or source.count(b"<definition ") != 1:
        raise ValueError("Expected exactly one native definition")
    root = ET.fromstring(match.group()[:-1] + b"/>")
    if root.get("type") != "43" or root.get("category") != "6" or root.get("flags") != "0":
        raise ValueError("Stock monitor template contract differs")
    for tag in (b"voxel_min", b"voxel_max"):
        element = re.search(rb"<" + tag + rb"\s[^>]*/>", source)
        if not element:
            raise ValueError("Monitor bounds are missing")
        bounds = ET.fromstring(element.group())
        if any(bounds.get(axis) != "0" for axis in ("x", "y", "z")):
            raise ValueError("Probe must retain stock 1x1 bounds")
    opening, count = re.subn(rb'\bname="[^"]*"', b'name="' + PROBE_NAME.encode("ascii") + b'"',
                             match.group(), count=1)
    if count != 1:
        raise ValueError("Stock display name is missing")
    return source[:match.start()] + opening + source[match.end():], root.attrib


def _read_bounded(path, limit):
    with path.open("rb") as source:
        result = source.read(limit + 1)
    if len(result) > limit:
        raise ValueError(f"File exceeds offline budget: {path}")
    return result


def build_probe(game, output):
    game, output = Path(game).resolve(), Path(output).resolve()
    # Never create a mod or alter assets/configuration inside the installation.
    if output == game or game in output.parents:
        raise ValueError("Choose a staging directory outside the game installation")
    if output.exists():
        raise FileExistsError(f"Refusing to overwrite: {output}")
    with (game / "stormworks64.exe").open("rb") as executable:
        checksum = hashlib.file_digest(executable, "sha256").hexdigest()
    if checksum != REFERENCE_EXE_SHA256:
        raise ValueError("Probe only reviewed for the static reference executable")
    source = _read_bounded(game / "rom/data/definitions/monitor_1.xml", MAX_STRING_BYTES)
    xml, attributes = prepare_probe_xml(source)
    # No resources bundled: native fallback resolves the unchanged stock paths.
    # Existence is checked, but this does not certify the runtime resource loader.
    for key in ("mesh_data_name", "mesh_editor_only_name"):
        name = attributes.get(key, "")
        _resource_name(name)
        resource = (game / "rom" / name).resolve()
        if game not in resource.parents or not resource.is_file():
            raise ValueError(f"Missing stock resource: {name}")
    archive = ComponentArchive(PROBE_ID, xml)
    packed = encode_archive(archive)
    if decode_archive(packed) != archive:
        raise ValueError("Archive self-check failed")
    identity = archive_identity(packed)
    instructions = (Path(__file__).resolve().parents[1] / "docs/custom_display_probe.md").read_bytes()
    native = output / "swml_display_probe"
    components = native / "data/components"
    output.mkdir(parents=True, exist_ok=False)
    components.mkdir(parents=True)
    (components / (identity + ".bin")).write_bytes(packed)
    (native / "mod.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<mod name="SWML Display Import Probe" author="StormworksModLoader" '
        'desc="Import test only: stock 1x1 geometry and resolution, not Custom Display."/>\n',
        encoding="utf-8")
    manifest = {
        "status": "UNTESTED_NATIVE_IMPORT_PROBE_NOT_CUSTOM_DISPLAY",
        "definition_id": PROBE_ID, "display_name": PROBE_NAME,
        "archive_id_native_md5": identity,
        "archive_sha256": hashlib.sha256(packed).hexdigest(),
        "source_definition_sha256": hashlib.sha256(source).hexdigest(),
        "reviewed_executable_sha256": checksum,
        "geometry": "stock monitor_1, unchanged", "logical_resolution": [32, 32],
        "bundled_resources": 0, "resource_policy": "unchanged stock mesh paths",
        "installed": False, "runtime_validated": False,
    }
    (output / "probe_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (output / "README.md").write_bytes(instructions)
    sums = "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(output).as_posix()}\n"
                   for p in sorted(output.rglob("*")) if p.is_file())
    (output / "SHA256SUMS.txt").write_text(sums, encoding="utf-8")
    return manifest


def verify_saved_probe(vehicle, reference_archive):
    """Read-only disk evidence, NOT proof of restart, runtime load or graphics."""
    vehicle, reference_archive = Path(vehicle), Path(reference_archive)
    if vehicle.suffix.lower() != ".xml":
        raise ValueError("Expected a saved vehicle .xml file")
    reference = _read_bounded(reference_archive, MAX_ARCHIVE_BYTES + 4)
    archive = decode_archive(reference)
    if archive.definition_id != PROBE_ID:
        raise ValueError("Reference is not the separate monitor witness")
    source = _read_bounded(vehicle, MAX_ARCHIVE_BYTES)
    # Do not allow encodings that could hide a DTD from this byte-level check.
    # Native vehicle saves observed here are UTF-8, unlike some stock definitions.
    text = source.decode("utf-8-sig")
    if "\0" in text or "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
        raise ValueError("Vehicle DTD/entities are not accepted")
    root = ET.fromstring(text)
    if root.tag != "vehicle":
        raise ValueError("Expected a native vehicle document")
    components = [c for c in root.findall("./bodies/body/components/c")
                  if c.get("d") == PROBE_ID]
    if not components:
        raise ValueError("Saved vehicle does not reference the witness definition")
    identity = archive_identity(reference)
    saved_archive = vehicle.with_suffix("") / (identity + ".bin")
    saved = _read_bounded(saved_archive, MAX_ARCHIVE_BYTES + 4)
    decode_archive(saved)
    if saved != reference:
        raise ValueError("Saved archive differs from the supplied reference bytes")
    return {"vehicle": str(vehicle), "definition_id": PROBE_ID,
            "saved_definition_references": len(components), "saved_archive": str(saved_archive),
            "archive_id_native_md5": identity, "archive_sha256": hashlib.sha256(saved).hexdigest(),
            "vehicle_sha256": hashlib.sha256(source).hexdigest(),
            "saved_archive_byte_identical": True, "check": "READ_ONLY_DISK_EVIDENCE",
            "restart_or_reload_proven": False, "runtime_validated": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="Generate an uninstalled native import probe")
    build.add_argument("--game", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    inspect = commands.add_parser("inspect", help="Strict offline archive inspection (no extraction)")
    inspect.add_argument("archive", type=Path)
    saved = commands.add_parser("verify-save", help="Read-only witness XML/companion archive check")
    saved.add_argument("vehicle", type=Path)
    saved.add_argument("--reference-archive", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "build":
            result = build_probe(args.game, args.output)
        elif args.command == "verify-save":
            result = verify_saved_probe(args.vehicle, args.reference_archive)
        else:
            data = _read_bounded(args.archive, MAX_ARCHIVE_BYTES + 4)
            archive = decode_archive(data)
            result = {"definition_id": archive.definition_id, "xml_bytes": len(archive.xml),
                      "archive_id_native_md5": archive_identity(data),
                      "resources": [{"name": n, "bytes": len(b)} for n, b in archive.resources],
                      "runtime_validated": False}
        print(json.dumps(result, indent=2))
    except (OSError, ValueError, ET.ParseError) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    main()
