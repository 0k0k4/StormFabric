"""Offline native asset tests; real stock comparisons are read-only."""
from collections import Counter, defaultdict
import math
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import custom_display_assets as assets
from custom_display_probe import decode_archive, archive_identity

GAME = TOOLS.parents[1] / "Stormworks"


def fixture():
    # Independent explicit triangle record, including section and opaque trailer.
    vertices = ((-.125, .1, -.125), (.125, .1, -.125), (0., .1, .125))
    return (b"mesh\x07\x00\x01\x00" + struct.pack("<HI", 3, 19)
            + b"".join(struct.pack("<3f4B3f", *v, 0, 0, 0, 255, 0., 1., 0.) for v in vertices)
            + struct.pack("<I3H", 3, 0, 1, 2)
            + struct.pack("<HIIHH6f", 1, 0, 3, 0, 0, -.125, .1, -.125, .125, .1, .125)
            + b"\0\0\x02\0ID" + struct.pack("<3fH", 1., 1., 1., 0))


def native_xml(data):
    # Only the test oracle converts the stock numeric-attribute dialect to XML.
    # Production preserves the native dialect verbatim.
    data = re.sub(rb'<physics_shape_rotation\s[^>]*/>', b'<physics_shape_rotation/>', data)
    return ET.fromstring(data)


def positions(root, path):
    return {tuple(int(v.get(a)) for a in ("x", "y", "z"))
            for v in root.findall(path)}


def surfaces(root, tag):
    return {(int(s.get("orientation")), *tuple(int(s.find("position").get(a))
             for a in ("x", "y", "z"))) for s in root.findall(f"./{tag}/surface")}


class MeshCodecTests(unittest.TestCase):
    def test_explicit_byte_fixture(self):
        mesh = assets.inspect_mesh(fixture())
        self.assertEqual(len(mesh.vertices), 3)
        self.assertEqual(mesh.indices, (0, 1, 2))
        self.assertEqual(mesh.bounds[0], -.125)

    def test_all_truncations_and_trailing_bytes(self):
        source = fixture()
        for length in range(len(source)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                assets.inspect_mesh(source[:length])
        with self.assertRaises(ValueError):
            assets.inspect_mesh(source + b"\0")

    def test_invalid_layout_index_float_and_bounds(self):
        source = fixture()
        for offset, fmt, value in ((4, "<H", 8), (6, "<H", 2), (8, "<H", 65535),
                                   (10, "<I", 18), (14, "<f", math.nan),
                                   (14 + 20, "<f", 0.), (98, "<I", 0xffffffff),
                                   (102, "<H", 3), (110, "<I", 5),
                                   (122, "<f", .5)):
            data = bytearray(source)
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                assets.inspect_mesh(data)


class SpecTests(unittest.TestCase):
    def test_canonical_identity_and_uint32(self):
        for values in ((2, 1, 480, 272), (0xffffffff,) * 4):
            spec = assets.DisplaySpec(*values)
            self.assertEqual(assets.DisplaySpec.parse(spec.identity), spec)
        for identity in ("swml_cd_v1_b02x1_p480x272", "swml_cd_v1_b2x1_p0x272",
                         "swml_cd_v1_b2x1_p4294967296x272", "swml_cd_v1_b2x1_p480x272\n",
                         "swml_cd_v1_b٢x1_p480x272", "../swml_cd_v1_b2x1_p480x272"):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                assets.DisplaySpec.parse(identity)
        for value in (0, -1, 1.0, True, 0x100000000):
            with self.subTest(value=value), self.assertRaises(ValueError):
                assets.DisplaySpec(value, 1, 32, 32)

    def test_anchor_convention(self):
        self.assertEqual(assets.DisplaySpec(1, 1, 1, 1).bounds, (0, 0, 0, 0))
        self.assertEqual(assets.DisplaySpec(2, 2, 1, 1).bounds, (-1, 0, 0, 1))
        self.assertEqual(assets.DisplaySpec(3, 3, 1, 1).bounds, (-1, -1, 1, 1))
        self.assertEqual(assets.DisplaySpec(4, 2, 1, 1).bounds, (-2, 0, 1, 1))


@unittest.skipUnless((GAME / "rom/meshes/component_screen_1.mesh").exists(),
                     "Reviewed stock asset installation unavailable")
class StockAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = {name: (GAME / "rom" / name).read_bytes() for name in assets.ASSET_HASHES}
        cls.frame = cls.sources["meshes/component_screen_1.mesh"]
        cls.arrow = cls.sources["meshes/arrow_screen_1.mesh"]
        cls.xml = cls.sources["data/definitions/monitor_1.xml"]

    def test_one_by_one_meshes_byte_identical(self):
        spec = assets.DisplaySpec(1, 1, 480, 272)
        self.assertEqual(assets.build_frame(self.frame, spec), self.frame)
        self.assertEqual(assets.build_arrow(self.arrow, spec), self.arrow)

    def test_corner_construction_matches_stock_two_and_three(self):
        for n in (2, 3):
            with self.subTest(n=n):
                built = assets.inspect_mesh(assets.build_frame(self.frame, assets.DisplaySpec(n, n, 1, 1)))
                stock = assets.inspect_mesh((GAME / f"rom/meshes/component_screen_{n}.mesh").read_bytes())
                # Exporter reorders vertices AND changes quad diagonals. Compare
                # vertex multiset and oriented area per plane independently.
                def vertices(mesh):
                    return Counter(tuple(round(v, 5) for v in vertex[:7]) for vertex in mesh.vertices)
                def plane_areas(mesh):
                    result = defaultdict(float)
                    for i in range(0, len(mesh.indices), 3):
                        p, q, r = [mesh.vertices[j][:3] for j in mesh.indices[i:i + 3]]
                        u, v = [q[j] - p[j] for j in range(3)], [r[j] - p[j] for j in range(3)]
                        cross = (u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0])
                        length = math.sqrt(sum(c*c for c in cross))
                        self.assertGreater(length, 1e-9)
                        normal = [c / length for c in cross]
                        plane = (*[round(c, 3) for c in normal], round(sum(normal[j]*p[j] for j in range(3)), 4))
                        result[plane] += length / 2
                    return result
                self.assertEqual(vertices(built), vertices(stock))
                actual_areas, expected_areas = plane_areas(built), plane_areas(stock)
                self.assertEqual(set(actual_areas), set(expected_areas))
                for plane in actual_areas:
                    self.assertAlmostEqual(actual_areas[plane], expected_areas[plane], places=6)
                for actual, expected in zip(built.bounds, stock.bounds):
                    self.assertAlmostEqual(actual, expected, places=6)

    def test_xml_voxels_and_surface_sets_match_stock(self):
        for n in (1, 2, 3):
            with self.subTest(n=n):
                built = native_xml(assets.build_definition(self.xml, assets.DisplaySpec(n, n, 480, 272)))
                stock = native_xml((GAME / f"rom/data/definitions/monitor_{n}.xml").read_bytes())
                self.assertEqual(positions(built, "./voxels/voxel/position"),
                                 positions(stock, "./voxels/voxel/position"))
                for tag in ("surfaces", "buoyancy_surfaces"):
                    self.assertEqual(surfaces(built, tag), surfaces(stock, tag))
                for tag in ("voxel_min", "voxel_max", "voxel_physics_min", "voxel_physics_max", "constraint_pos_parent"):
                    self.assertEqual(built.find(tag).attrib, stock.find(tag).attrib)
                self.assertEqual(ET.tostring(built.find("logic_nodes")),
                                 ET.tostring(native_xml(self.xml).find("logic_nodes")))

    def test_rectangular_geometry_independent_of_pixels(self):
        spec = assets.DisplaySpec(4, 2, 480, 272)
        mesh = assets.inspect_mesh(assets.build_frame(self.frame, spec))
        self.assertEqual(mesh.bounds[3] - mesh.bounds[0], 1.)
        self.assertEqual(mesh.bounds[5] - mesh.bounds[2], .5)
        built = native_xml(assets.build_definition(self.xml, spec))
        self.assertEqual(len(built.findall("./voxels/voxel")), 8)
        self.assertEqual(len(built.findall("./surfaces/surface")), 20)
        other = assets.DisplaySpec(4, 2, 17, 59)
        self.assertEqual(assets.build_frame(self.frame, spec), assets.build_frame(self.frame, other))
        self.assertEqual(assets.build_arrow(self.arrow, spec), assets.build_arrow(self.arrow, other))
        original_arrow = assets.inspect_mesh(self.arrow)
        arrow = assets.inspect_mesh(assets.build_arrow(self.arrow, spec))
        for old, new in zip(original_arrow.vertices, arrow.vertices):
            self.assertAlmostEqual(new[0] - old[0], -.125, places=6)
            self.assertAlmostEqual(new[2] - old[2], .125, places=6)
            self.assertEqual(new[1], old[1])
            self.assertEqual(new[3:], old[3:])
        self.assertEqual(arrow.indices, original_arrow.indices)

    def test_archive_complete_and_deterministic(self):
        spec = assets.DisplaySpec(2, 1, 480, 272)
        packed = assets.build_archive(self.sources, spec)
        self.assertEqual(packed, assets.build_archive(self.sources, spec))
        archive = decode_archive(packed)
        self.assertEqual(archive.definition_id, spec.identity)
        self.assertEqual(len(archive.resources), 2)
        root = native_xml(archive.xml)
        self.assertEqual({name for name, _ in archive.resources},
                         {root.get("mesh_data_name"), root.get("mesh_editor_only_name")})
        self.assertNotEqual(archive_identity(packed), archive_identity(assets.build_archive(
            self.sources, assets.DisplaySpec(2, 1, 640, 360))))

    def test_source_mutations_and_tool_limits_rejected(self):
        spec = assets.DisplaySpec(2, 1, 480, 272)
        for name in self.sources:
            changed = dict(self.sources)
            changed[name] = changed[name][:-1] + bytes([changed[name][-1] ^ 1])
            with self.subTest(name=name), self.assertRaises(ValueError):
                assets.build_archive(changed, spec)
        with self.assertRaises(ValueError):
            assets.build_archive(self.sources, assets.DisplaySpec(65, 1, 1, 1))

    def test_stage_does_not_install_or_overwrite(self):
        with tempfile.TemporaryDirectory(dir=TOOLS.parent / "build") as temporary:
            out = Path(temporary) / "geometry"
            receipt = assets.stage(GAME, out, assets.DisplaySpec(2, 1, 480, 272))
            self.assertFalse(receipt["independent_resolution_active"])
            self.assertEqual(receipt["unmodified_engine_resolution"], [64, 32])
            self.assertFalse((out / "mod.xml").exists())
            before = {p.name: p.read_bytes() for p in out.iterdir()}
            with self.assertRaises(FileExistsError):
                assets.stage(GAME, out, assets.DisplaySpec(3, 2, 480, 272))
            self.assertEqual(before, {p.name: p.read_bytes() for p in out.iterdir()})
        with self.assertRaises(ValueError):
            assets.stage(GAME, GAME / "mods/should_not_exist", assets.DisplaySpec(1, 1, 32, 32))


if __name__ == "__main__":
    unittest.main()
