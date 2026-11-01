"""Deterministic native geometry/archive builder; offline, NOT a runtime plugin.

Only reviewed stock assets are accepted. No game files, saves or installed mods
are modified. Encoded pixel dimensions require the future native target bridge;
the unmodified game still derives its resolution from physical voxel bounds.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import struct

from custom_display_probe import (ComponentArchive, MAX_STRING_BYTES,
                                  _read_bounded, encode_archive, decode_archive,
                                  archive_identity)


# Tool allocation bounds, NOT certified native placement or GPU limits.
MAX_BLOCKS = 64
MAX_MESH_BYTES = 1024 * 1024
ASSET_HASHES = {
    "data/definitions/monitor_1.xml": "f824fda06ac01b4bc59f9aae0d4bdee754d68163118d978abbc128564535e550",
    "meshes/component_screen_1.mesh": "7be899a0d4b2a7d8f5c02a68fd539d98cfddb5b5b1981a1cf17c80f1feeee790",
    "meshes/arrow_screen_1.mesh": "95d554c3d9a954bdc93455a37d542dced0fd79e2dde75d52d160713aa5155015",
}


@dataclass(frozen=True)
class DisplaySpec:
    width: int
    height: int
    pixels_x: int
    pixels_y: int

    def __post_init__(self):
        if any(type(v) is not int or not 1 <= v <= 0xffffffff
               for v in (self.width, self.height, self.pixels_x, self.pixels_y)):
            raise ValueError("Dimensions must be positive uint32 integers")

    @property
    def identity(self):
        return f"swml_cd_v1_b{self.width}x{self.height}_p{self.pixels_x}x{self.pixels_y}"

    @classmethod
    def parse(cls, identity):
        match = re.fullmatch(r"swml_cd_v1_b([1-9][0-9]{0,9})x([1-9][0-9]{0,9})_p"
                             r"([1-9][0-9]{0,9})x([1-9][0-9]{0,9})", identity, flags=re.ASCII)
        if not match:
            raise ValueError("Noncanonical Custom Display identity")
        return cls(*(int(v) for v in match.groups()))

    @property
    def bounds(self):
        # Same integer anchor convention as stock monitor_2 and monitor_3.
        return (-(self.width // 2), -((self.height - 1) // 2),
                (self.width - 1) // 2, self.height // 2)


@dataclass(frozen=True)
class NativeMesh:
    vertices: tuple
    indices: tuple
    bounds_offset: int
    bounds: tuple


def inspect_mesh(data):
    """Strict reviewed v7 / one format-19 stream / one-section mesh subset.

    Static contract: FUN_1402cbfb0, FUN_140187fe0, FUN_140187d40. Unsupported
    layouts are rejected, never scanned heuristically for position-like floats.
    """
    if not 14 <= len(data) <= MAX_MESH_BYTES:
        raise ValueError("Missing or oversized native mesh")
    cursor = 0

    def take(fmt):
        nonlocal cursor
        size = struct.calcsize(fmt)
        if size > len(data) - cursor:
            raise ValueError("Truncated native mesh")
        value = struct.unpack_from(fmt, data, cursor)
        cursor += size
        return value

    if take("<4sHH") != (b"mesh", 7, 1):
        raise ValueError("Unsupported native mesh header")
    count, fmt = take("<HI")
    if fmt != 19 or not count or count > (len(data) - cursor) // 28:
        raise ValueError("Unsupported vertex stream")
    vertices = tuple(take("<3f4B3f") for _ in range(count))
    for v in vertices:
        if not all(math.isfinite(f) for f in (*v[:3], *v[7:])):
            raise ValueError("Nonfinite vertex")
        if not 0.99 <= sum(f * f for f in v[7:]) <= 1.01:
            raise ValueError("Invalid vertex normal")
    index_count, = take("<I")
    if not index_count or index_count % 3 or index_count > (len(data) - cursor) // 2:
        raise ValueError("Invalid triangle index count")
    indices = take("<" + str(index_count) + "H")
    if max(indices) >= count:
        raise ValueError("Vertex index out of range")
    if take("<HIIHH") != (1, 0, index_count, 0, 0):
        raise ValueError("Unsupported section layout")
    bounds_offset = cursor
    bounds = take("<6f")
    if not all(math.isfinite(f) for f in bounds):
        raise ValueError("Nonfinite section bounds")
    for axis in range(3):
        if (bounds[axis] > min(v[axis] for v in vertices) + 1e-6 or
                bounds[axis + 3] < max(v[axis] for v in vertices) - 1e-6):
            raise ValueError("Section bounds do not enclose vertices")
    for _ in range(2):
        size, = take("<H")
        if size > 8192:
            raise ValueError("Oversized section string")
        text, = take(f"<{size}s")
        if b"\0" in text or any(c < 32 or c > 126 for c in text):
            raise ValueError("Unsupported section string")
    if take("<3f") != (1.0, 1.0, 1.0):
        raise ValueError("Unsupported section scale")
    # Reviewed stock files have this final zero word. Preserve it as opaque;
    # do not reinterpret it as a section or add fields the reader does not use.
    if take("<H") != (0,) or cursor != len(data):
        raise ValueError("Unexpected native mesh trailer")
    return NativeMesh(vertices, indices, bounds_offset, bounds)


def _reviewed_asset(name, data):
    if hashlib.sha256(data).hexdigest() != ASSET_HASHES[name]:
        raise ValueError(f"Unreviewed source asset: {name}")


def _geometry_limit(spec):
    if spec.width > MAX_BLOCKS or spec.height > MAX_BLOCKS:
        raise ValueError("Physical dimensions exceed the offline tool budget")


def build_frame(source, spec):
    """Move four corner groups, preserving native border width and normals.

    Uniform scaling would stretch the bezel and invalidate bevel normals. The
    stock 1x1 topology instead extends straight edges between translated corners.
    This construction is checked against stock 2x2 and 3x3 meshes in offline tests.
    """
    _geometry_limit(spec)
    _reviewed_asset("meshes/component_screen_1.mesh", source)
    mesh = inspect_mesh(source)
    xmin, zmin, xmax, zmax = spec.bounds
    output = bytearray(source)
    for i, vertex in enumerate(mesh.vertices):
        x, _, z = vertex[:3]
        struct.pack_into("<f", output, 14 + i * 28, x + .25 * (xmin if x < 0 else xmax))
        struct.pack_into("<f", output, 22 + i * 28, z + .25 * (zmin if z < 0 else zmax))
    bounds = list(mesh.bounds)
    for axis, delta in ((0, xmin), (2, zmin), (3, xmax), (5, zmax)):
        bounds[axis] += delta * .25
    struct.pack_into("<6f", output, mesh.bounds_offset, *bounds)
    result = bytes(output)
    inspect_mesh(result)
    return result


def build_arrow(source, spec):
    """Translate the unchanged editor arrow to the centre of the new face."""
    _geometry_limit(spec)
    _reviewed_asset("meshes/arrow_screen_1.mesh", source)
    mesh = inspect_mesh(source)
    xmin, zmin, xmax, zmax = spec.bounds
    dx, dz = (xmin + xmax) * .125, (zmin + zmax) * .125
    output = bytearray(source)
    for i, v in enumerate(mesh.vertices):
        struct.pack_into("<f", output, 14 + i * 28, v[0] + dx)
        struct.pack_into("<f", output, 22 + i * 28, v[2] + dz)
    bounds = list(mesh.bounds)
    for axis, delta in ((0, dx), (2, dz), (3, dx), (5, dz)):
        bounds[axis] += delta
    struct.pack_into("<6f", output, mesh.bounds_offset, *bounds)
    result = bytes(output)
    inspect_mesh(result)
    return result


def build_definition(source, spec):
    _geometry_limit(spec)
    _reviewed_asset("data/definitions/monitor_1.xml", source)
    text = source.decode("utf-8")
    xmin, zmin, xmax, zmax = spec.bounds

    def replace(pattern, value):
        nonlocal text
        text, count = re.subn(pattern, lambda _: value, text, flags=re.DOTALL)
        if count != 1:
            raise ValueError("Source definition structure differs")

    replace(r'name="Monitor 1x1"', f'name="Custom Display {spec.width}x{spec.height} '
            f'({spec.pixels_x}x{spec.pixels_y})"')
    replace(r'mass="4"', f'mass="{4 * spec.width * spec.height}"')
    replace(r'value="500"', f'value="{500 * spec.width * spec.height}"')
    replace(r'mesh_data_name="[^"]*"', f'mesh_data_name="meshes/{spec.identity}.mesh"')
    replace(r'mesh_editor_only_name="[^"]*"',
            f'mesh_editor_only_name="meshes/{spec.identity}_arrow.mesh"')

    def surface(orientation, x, z):
        return (f'\t\t<surface orientation="{orientation}" rotation="0" shape="1" trans_type="0">\n'
                f'\t\t\t<position x="{x}" y="0" z="{z}"/>\n\t\t</surface>')

    # Five exterior faces as stock: no attachment surface on the display front
    # (+Y, orientation 2), no hidden internal side faces between adjacent voxels.
    faces = []
    for x in range(xmin, xmax + 1):
        for z in range(zmin, zmax + 1):
            faces.append(surface(3, x, z))
            if x == xmin:
                faces.append(surface(1, x, z))
            if x == xmax:
                faces.append(surface(0, x, z))
            if z == zmin:
                faces.append(surface(5, x, z))
            if z == zmax:
                faces.append(surface(4, x, z))
    for tag in ("surfaces", "buoyancy_surfaces"):
        replace(fr'<{tag}>.*?</{tag}>', f'<{tag}>\n' + '\n'.join(faces) + f'\n\t</{tag}>')
    voxels = []
    for x in range(xmin, xmax + 1):
        for z in range(zmin, zmax + 1):
            voxels.append('\t\t<voxel flags="1" physics_shape="0" buoy_pipes="0">\n'
                          f'\t\t\t<position x="{x}" y="0" z="{z}"/>\n'
                          '\t\t\t<physics_shape_rotation 00="1" 01="0" 02="0" '
                          '10="0" 11="1" 12="0" 20="0" 21="0" 22="1"/>\n\t\t</voxel>')
    replace(r'<voxels>.*?</voxels>', '<voxels>\n' + '\n'.join(voxels) + '\n\t</voxels>')
    for tag, x, z in (("voxel_min", xmin, zmin), ("voxel_max", xmax, zmax),
                      ("voxel_physics_min", xmin, zmin), ("voxel_physics_max", xmax, zmax)):
        replace(fr'<{tag}\s[^>]*/>', f'<{tag} x="{x}" y="0" z="{z}"/>')
    cx, cz = (xmin + xmax) * .125, (zmin + zmax) * .125
    replace(r'<constraint_pos_parent\s[^>]*/>', f'<constraint_pos_parent x="{cx:g}" y="0" z="{cz:g}"/>')
    result = text.encode("utf-8")
    if len(result) > MAX_STRING_BYTES:
        raise ValueError("Generated definition exceeds offline XML budget")
    return result


def build_archive(assets, spec):
    xml = build_definition(assets["data/definitions/monitor_1.xml"], spec)
    resources = ((f"meshes/{spec.identity}.mesh",
                  build_frame(assets["meshes/component_screen_1.mesh"], spec)),
                 (f"meshes/{spec.identity}_arrow.mesh",
                  build_arrow(assets["meshes/arrow_screen_1.mesh"], spec)))
    archive = ComponentArchive(spec.identity, xml, resources)
    packed = encode_archive(archive)
    if decode_archive(packed) != archive:
        raise ValueError("Generated archive failed its roundtrip check")
    return packed


def load_reviewed_assets(game):
    assets = {name: _read_bounded(Path(game) / "rom" / name, MAX_MESH_BYTES)
              for name in ASSET_HASHES}
    for name, data in assets.items():
        if hashlib.sha256(data).hexdigest() != ASSET_HASHES[name]:
            raise ValueError(f"Unreviewed stock template: {name}")
    return assets


def stage(game, output, spec):
    game, output = Path(game).resolve(), Path(output).resolve()
    if output == game or game in output.parents:
        raise ValueError("Staging must be outside the game installation")
    if output.exists():
        raise FileExistsError(f"Refusing to overwrite: {output}")
    _geometry_limit(spec)
    assets = load_reviewed_assets(game)
    packed = build_archive(assets, spec)
    identity = archive_identity(packed)
    manifest = {
        "status": "OFFLINE_GEOMETRY_NOT_RUNTIME_PLUGIN", "definition_id": spec.identity,
        "archive_id_native_md5": identity, "archive_sha256": hashlib.sha256(packed).hexdigest(),
        "physical_blocks": [spec.width, spec.height],
        "requested_resolution": [spec.pixels_x, spec.pixels_y],
        "unmodified_engine_resolution": [spec.width * 32, spec.height * 32],
        "independent_resolution_active": False, "runtime_validated": False,
        "source_asset_sha256": ASSET_HASHES,
        "geometry": "native mesh corners, exterior surfaces and full cuboid voxels",
        "installed": False,
    }
    output.mkdir(parents=True, exist_ok=False)
    # Intentionally no mod.xml: not a loadable substitute for the future DLL.
    (output / (identity + ".bin")).write_bytes(packed)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (output / "README.md").write_text(
        "# Custom Display — géométrie hors jeu uniquement\n\n"
        "Ce dossier n'est pas le plugin final et n'est pas installé.\n"
        "L'archive contient mesh, flèche, voxels, surfaces et identité de configuration.\n"
        "La résolution demandée reste inactive sans le pont natif du loader.\n"
        "Aucun test en jeu, tactile, réflexion ou collision n'est revendiqué.\n",
        encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--identity", required=True)
    args = parser.parse_args()
    try:
        result = stage(args.game, args.output, DisplaySpec.parse(args.identity))
    except (ValueError, OSError) as error:
        parser.exit(1, f"Error: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
