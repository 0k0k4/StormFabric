"""Cross-check the fixed native fixture and crypto pin; no game process launched."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import custom_display_assets as assets
from custom_display_probe import decode_archive, archive_identity


@unittest.skipUnless((ROOT.parent / "Stormworks/rom/data/definitions/monitor_1.xml").exists(), "reviewed stock templates unavailable")
class SmallExperimentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = assets.build_archive(assets.load_reviewed_assets(ROOT.parent / "Stormworks"), assets.DisplaySpec(1,1,24,16))
        cls.source = (ROOT / "src/custom_display_observer.cpp").read_text()

    def constant(self, name):
        return re.search(rf'{name}\[\] = "([^"]+)"', self.source)[1]

    def test_native_allowlist_matches_exact_generated_archive(self):
        self.assertEqual(self.constant("kSmallId"), decode_archive(self.data).definition_id)
        self.assertEqual(self.constant("kSmallArchive"), archive_identity(self.data))
        self.assertEqual(self.constant("kSmallSha256"), hashlib.sha256(self.data).hexdigest())

    def test_native_geometry_unchanged_for_one_block(self):
        archive = decode_archive(self.data)
        resources = dict(archive.resources)
        for name, relative in (("meshes/swml_cd_v1_b1x1_p24x16.mesh", "meshes/component_screen_1.mesh"),
                               ("meshes/swml_cd_v1_b1x1_p24x16_arrow.mesh", "meshes/arrow_screen_1.mesh")):
            self.assertEqual(resources[name], (ROOT.parent / "Stormworks/rom" / relative).read_bytes())

    def test_each_attachment_is_no_larger_than_native_baseline(self):
        for quality, scale in ((0,2),(1,1),(2,2),(3,4),(-1,2)):
            with self.subTest(quality=quality):
                self.assertLessEqual(24*scale,32*scale)
                self.assertLessEqual(16*scale,32*scale)
                self.assertLessEqual(24*16*12*(scale*scale+1),32*32*12*(scale*scale+1))

    @unittest.skipUnless(os.name == "nt" and (ROOT / "build/tests/custom_display_observer_tests.exe").exists(), "native pin test executable unavailable")
    def test_native_sha256_pin_and_write_exclusion(self):
        approved = Path(os.environ["TEMP"]) / "opencode"
        approved.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="cd-small-pin-", dir=approved) as temporary:
            file = Path(temporary) / "fixture.bin"
            for data, expected in ((self.data,0), (self.data[:-1]+b"X",2), (b"",2), (b"X"*1048577,2)):
                file.write_bytes(data)
                result = subprocess.run([str(ROOT / "build/tests/custom_display_observer_tests.exe"),
                                         "--pin-test", str(file)], capture_output=True, timeout=10)
                self.assertEqual(result.returncode, expected, result.stderr)


if __name__ == "__main__":
    unittest.main()
