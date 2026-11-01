"""Create-new bundle checks against disposable fake files, no installation."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / "tools/package_custom_display_vehicle_observer.py"
spec = importlib.util.spec_from_file_location("vehicle_package", path)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        import os
        approved = Path(os.environ["TEMP"]) / "opencode"
        approved.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="cd-vehicle-package-", dir=approved)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.game, self.output = self.root / "game", self.root / "bundle"
        self.game.mkdir()
        for name in ("stormworks64.exe", "iphlpapi.dll", "Launcher.exe"):
            (self.game / name).write_bytes(name.encode())
        self.sources = (
            "build/iphlpapi.dll", "tools/custom_display_observation.ps1",
            "tools/custom_display_observation_run.bat", "tools/custom_display_observation_restore.bat",
            "docs/custom_display_vehicle_observation_readme.md", "tools/custom_display_vehicle_report.py",
            "build/custom-display-vehicle-activity-tests.log", "build/custom-display-vehicle-activity-build.log")
        for name in self.sources:
            target = self.root / name
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(name.encode())
        for name, value in (("ROOT", self.root),
                            ("REFERENCE_EXE", package.digest(self.game / "stormworks64.exe")),
                            ("ORIGINAL_LOADER", package.digest(self.game / "iphlpapi.dll"))):
            context = patch.object(package, name, value)
            context.start(); self.addCleanup(context.stop)
        context = patch.object(package, "exports", return_value=list(range(313)))
        self.exports = context.start(); self.addCleanup(context.stop)

    def test_stage_has_no_fixtures_or_installation(self):
        original = (self.game / "iphlpapi.dll").read_bytes()
        manifest = package.stage(self.game, self.output)
        self.assertTrue(manifest["observationOnly"])
        self.assertEqual(manifest["observerProtocolVersion"], 2)
        self.assertFalse(manifest["runtimeValidated"])
        self.assertFalse(manifest["installed"])
        self.assertFalse((self.output / "fixtures").exists())
        self.assertEqual((self.game / "iphlpapi.dll").read_bytes(), original)
        for name, digest in manifest["files"].items():
            self.assertEqual(hashlib.sha256((self.output / name).read_bytes()).hexdigest(), digest)

    def test_existing_output_never_overwritten(self):
        self.output.mkdir()
        (self.output / "user.txt").write_bytes(b"keep")
        with self.assertRaises(ValueError):
            package.stage(self.game, self.output)
        self.assertEqual((self.output / "user.txt").read_bytes(), b"keep")
        self.exports.assert_not_called()

    def test_changed_install_and_exports_refused_before_output(self):
        for name in ("stormworks64.exe", "iphlpapi.dll"):
            file = self.game / name
            saved = file.read_bytes(); file.write_bytes(b"user change")
            with self.assertRaises(ValueError):
                package.stage(self.game, self.output)
            self.assertFalse(self.output.exists())
            self.assertEqual(file.read_bytes(), b"user change")
            file.write_bytes(saved)
        self.exports.side_effect = [list(range(313)), list(range(312))]
        with self.assertRaises(ValueError):
            package.stage(self.game, self.output)
        self.assertFalse(self.output.exists())

    def test_missing_source_does_not_leave_partial_output(self):
        (self.root / self.sources[-1]).unlink()
        with self.assertRaises(FileNotFoundError):
            package.stage(self.game, self.output)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
