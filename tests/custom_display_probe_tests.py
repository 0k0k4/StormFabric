"""Offline format/probe tests. These never call Stormworks or install a mod."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools/custom_display_probe.py"
SPEC = importlib.util.spec_from_file_location("custom_display_probe", MODULE_PATH)
probe = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = probe
SPEC.loader.exec_module(probe)

TEMPLATE = (
    b'<?xml version="1.0" encoding="UTF-8"?>\r\n'
    b'<definition name="Monitor 1x1" category="6" type="43" flags="0" '
    b'mesh_data_name="meshes/screen.mesh" mesh_editor_only_name="meshes/arrow.mesh">\r\n'
    b' <voxel_min x="0" y="0" z="0"/>\r\n'
    b' <voxel_max x="0" y="0" z="0"/>\r\n'
    b' <physics_shape_rotation 00="1" 11="1" 22="1"/>\r\n'
    b'</definition>\r\n'
)


def envelope(blob):
    return struct.pack("<I", len(blob)) + blob


class ArchiveTests(unittest.TestCase):
    def test_independent_byte_fixture(self):
        # Byte layout is specified explicitly, NOT produced by encode_archive.
        blob = (b'\x01\x00\x00\x00test_monitor.xml\x00<definition/>\x00'
                b'\x01\x00meshes/a.mesh\x00\x03\x00\x00\x00ABC')
        packed = envelope(blob)
        expected = probe.ComponentArchive("test_monitor", b"<definition/>", (("meshes/a.mesh", b"ABC"),))
        self.assertEqual(probe.decode_archive(packed), expected)
        self.assertEqual(probe.encode_archive(expected), packed)
        self.assertEqual(probe.archive_identity(packed), hashlib.md5(blob).hexdigest())
        self.assertNotEqual(probe.archive_identity(packed), hashlib.md5(packed).hexdigest())

    def test_deterministic_and_distinct_identities(self):
        first = probe.ComponentArchive("first", b'<definition name="Example"/>')
        second = probe.ComponentArchive("second", first.xml)
        packed = probe.encode_archive(first)
        self.assertEqual(packed, probe.encode_archive(first))
        self.assertNotEqual(probe.archive_identity(packed),
                            probe.archive_identity(probe.encode_archive(second)))
        self.assertEqual(probe.decode_archive(packed).definition_id, "first")

    def test_every_truncation_rejected(self):
        data = probe.encode_archive(probe.ComponentArchive("probe", b"<definition/>", (("a.mesh", b"abc"),)))
        for length in range(len(data)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                probe.decode_archive(data[:length])
            # Fix the outer envelope too: exercise inner boundaries, not just it.
            if length >= 4:
                with self.subTest(inner_length=length), self.assertRaises(ValueError):
                    probe.decode_archive(envelope(data[4:length]))

    def test_unknown_version_trailing_data_and_missing_strings(self):
        data = probe.encode_archive(probe.ComponentArchive("probe", b"<definition/>"))
        for blob in (struct.pack("<I", 2) + data[8:], data[4:] + b"extra",
                     struct.pack("<I", 1) + b"probe.xml",
                     struct.pack("<I", 1) + b"probe.xml\0\0\0\0"):
            with self.subTest(blob=blob), self.assertRaises(ValueError):
                probe.decode_archive(envelope(blob))

    def test_identity_and_resource_paths_rejected(self):
        for identity in ("", "../x", "Upper", "a.xml", "a/b", "a\\b", "1bad", "x" * 97):
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                probe.encode_archive(probe.ComponentArchive(identity, b"<definition/>"))
        for name in ("../a", "/a", "a//b", "a/../b", "C:/a", "a\\b", "a/./b", "a\0b", "é"):
            with self.subTest(resource=name), self.assertRaises(ValueError):
                probe.encode_archive(probe.ComponentArchive("probe", b"xml", ((name, b"abc"),)))

    def test_duplicate_resources_in_encoder_and_decoder(self):
        resources = (("a.mesh", b"one"), ("A.mesh", b"two"))
        with self.assertRaises(ValueError):
            probe.encode_archive(probe.ComponentArchive("probe", b"xml", resources))
        blob = (struct.pack("<I", 1) + b"probe.xml\0xml\0" + struct.pack("<H", 2)
                + b"a.mesh\0" + struct.pack("<I", 3) + b"one"
                + b"A.mesh\0" + struct.pack("<I", 3) + b"two")
        with self.assertRaises(ValueError):
            probe.decode_archive(envelope(blob))

    def test_declared_size_and_count_never_drive_allocation(self):
        prefix = struct.pack("<I", 1) + b"probe.xml\0xml\0"
        for body in (struct.pack("<H", 65535),
                     struct.pack("<H", 1) + b"a.mesh\0" + struct.pack("<I", 0xffffffff)):
            with self.assertRaises(ValueError):
                probe.decode_archive(envelope(prefix + body))
        with patch.object(probe, "MAX_ARCHIVE_BYTES", 32), self.assertRaises(ValueError):
            probe.encode_archive(probe.ComponentArchive("probe", b"x" * 40))
        with patch.object(probe, "MAX_STRING_BYTES", 3), self.assertRaises(ValueError):
            probe.encode_archive(probe.ComponentArchive("probe", b"x" * 4))
        for xml in (b"", b"xml\0hidden"):
            with self.assertRaises(ValueError):
                probe.encode_archive(probe.ComponentArchive("probe", xml))
        with self.assertRaises(ValueError):
            probe.encode_archive(probe.ComponentArchive("probe", b"xml", (("a.mesh", b""),)))
        with self.assertRaises(ValueError):
            probe.decode_archive(envelope(prefix + b"\x01\x00a.mesh\0\0\0\0\0"))


class SavedProbeTests(unittest.TestCase):
    def setUp(self):
        import os
        temp_root = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
        temp_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="swml-save-check-", dir=temp_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.vehicle = self.root / "test.xml"
        self.vehicle.write_bytes(b'<vehicle><bodies><body><components><c d="' +
                                 probe.PROBE_ID.encode() + b'"/></components></body></bodies></vehicle>')
        self.reference = self.root / "reference.bin"
        self.reference.write_bytes(probe.encode_archive(probe.ComponentArchive(probe.PROBE_ID, TEMPLATE)))
        self.companion = self.root / "test" / (probe.archive_identity(self.reference.read_bytes()) + ".bin")
        self.companion.parent.mkdir()
        self.companion.write_bytes(self.reference.read_bytes())

    def test_saved_probe_and_companion_match_without_any_write(self):
        before = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        result = probe.verify_saved_probe(self.vehicle, self.reference)
        self.assertEqual(result["saved_definition_references"], 1)
        self.assertTrue(result["saved_archive_byte_identical"])
        self.assertFalse(result["restart_or_reload_proven"])
        self.assertFalse(result["runtime_validated"])
        self.assertEqual(before, {p.relative_to(self.root): p.read_bytes()
                                  for p in self.root.rglob("*") if p.is_file()})

    def test_missing_companion_rejected(self):
        self.companion.unlink()
        with self.assertRaises(FileNotFoundError):
            probe.verify_saved_probe(self.vehicle, self.reference)

    def test_changed_companion_rejected_even_if_well_formed(self):
        self.companion.write_bytes(probe.encode_archive(probe.ComponentArchive(probe.PROBE_ID, b"changed")))
        with self.assertRaises(ValueError):
            probe.verify_saved_probe(self.vehicle, self.reference)

    def test_other_identity_in_reference_rejected(self):
        self.reference.write_bytes(probe.encode_archive(probe.ComponentArchive("other", TEMPLATE)))
        with self.assertRaises(ValueError):
            probe.verify_saved_probe(self.vehicle, self.reference)

    def test_wrong_vehicle_document_or_component_rejected(self):
        for data in (b"<not_vehicle/>", b"<vehicle/>", b'<vehicle><c d="' + probe.PROBE_ID.encode() + b'"/></vehicle>',
                     b'<!DOCTYPE vehicle [<!ENTITY x "test">]><vehicle/>',
                     '<!DOCTYPE vehicle><vehicle/>'.encode("utf-16"),
                     '<!DOCTYPE vehicle><vehicle/>'.encode("utf-16-le")):
            self.vehicle.write_bytes(data)
            with self.subTest(data=data), self.assertRaises((ValueError, UnicodeError)):
                probe.verify_saved_probe(self.vehicle, self.reference)

    def test_xml_size_bound_and_extension(self):
        with patch.object(probe, "MAX_ARCHIVE_BYTES", 32), self.assertRaises(ValueError):
            probe.verify_saved_probe(self.vehicle, self.reference)
        with self.assertRaises(ValueError):
            probe.verify_saved_probe(self.root / "test.bin", self.reference)


class ProbeTests(unittest.TestCase):
    def test_template_preserved_except_display_name(self):
        result, attributes = probe.prepare_probe_xml(TEMPLATE)
        self.assertEqual(result, TEMPLATE.replace(b'name="Monitor 1x1"',
                                                b'name="SWML Display Probe 1x1"', 1))
        self.assertEqual(attributes["mesh_data_name"], "meshes/screen.mesh")
        self.assertIn(b'physics_shape_rotation 00="1"', result)

    def test_wrong_template_rejected(self):
        for data in (TEMPLATE.replace(b'type="43"', b'type="1"'),
                     TEMPLATE.replace(b'flags="0"', b'flags="128"'),
                     TEMPLATE.replace(b'<voxel_max x="0"', b'<voxel_max x="1"'),
                     TEMPLATE.replace(b'<voxel_min', b'<not_voxel_min'),
                     TEMPLATE.replace(b'</definition>', b''),
                     b'<!DOCTYPE definition>' + TEMPLATE,
                     TEMPLATE + b"\0", b"<not_a_definition/>"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                probe.prepare_probe_xml(data)

    def test_staging_and_no_overwrite_no_game_writes(self):
        # tempfile is explicitly rooted under the approved OpenCode temp dir.
        import os
        temp_root = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
        temp_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="swml-probe-", dir=temp_root) as temporary:
            root = Path(temporary)
            game = root / "game"
            definitions = game / "rom/data/definitions"
            definitions.mkdir(parents=True)
            (definitions / "monitor_1.xml").write_bytes(TEMPLATE)
            (game / "rom/meshes").mkdir()
            for name in ("screen.mesh", "arrow.mesh"):
                (game / "rom/meshes" / name).write_bytes(b"fake resource: never executed")
            (game / "stormworks64.exe").write_bytes(b"fake PE: never executed")
            def snapshot():
                return {p.relative_to(game).as_posix(): p.read_bytes()
                        for p in game.rglob("*") if p.is_file()}
            original = snapshot()
            output = root / "staging"
            with self.assertRaises(ValueError):
                probe.build_probe(game, output)
            self.assertFalse(output.exists())
            checksum = hashlib.sha256((game / "stormworks64.exe").read_bytes()).hexdigest()
            with patch.object(probe, "REFERENCE_EXE_SHA256", checksum):
                with self.assertRaises(ValueError):
                    probe.build_probe(game, game / "new-mod")
                result = probe.build_probe(game, output)
                with self.assertRaises(FileExistsError):
                    probe.build_probe(game, output)
            self.assertFalse(result["installed"])
            self.assertFalse(result["runtime_validated"])
            self.assertEqual(result["definition_id"], probe.PROBE_ID)
            archives = list(output.rglob("*.bin"))
            self.assertEqual(len(archives), 1)
            self.assertEqual(archives[0].stem, probe.archive_identity(archives[0].read_bytes()))
            for line in (output / "SHA256SUMS.txt").read_text().splitlines():
                checksum, relative = line.split("  ", 1)
                self.assertEqual(hashlib.sha256((output / relative).read_bytes()).hexdigest(), checksum)
            self.assertEqual(snapshot(), original)


if __name__ == "__main__":
    unittest.main()
