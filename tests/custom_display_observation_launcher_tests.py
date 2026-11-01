"""Exercise transaction/rollback on disposable fake files, never the game install."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/custom_display_observation.ps1"


@unittest.skipUnless(os.name == "nt" and shutil.which("powershell.exe"), "Windows PowerShell required")
class LauncherTests(unittest.TestCase):
    def setUp(self):
        approved = Path(os.environ["TEMP"]) / "opencode"
        approved.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="cd-observer-launcher-", dir=approved)
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.bundle, self.game = root / "bundle", root / "fake-game"
        self.native = root / "fake-native-data"
        self.bundle.mkdir()
        self.game.mkdir()
        self.native.mkdir()
        shutil.copyfile(SCRIPT, self.bundle / "observe.ps1")
        self.original, self.observer = b"original loader fixture", b"observation loader fixture"
        # Intentionally NOT an executable. Run must fail before launching any program.
        (self.game / "stormworks64.exe").write_bytes(b"not a Windows executable")
        (self.game / "Launcher.exe").write_bytes(b"not a Windows launcher")
        (self.game / "iphlpapi.dll").write_bytes(self.original)
        (self.bundle / "iphlpapi.dll").write_bytes(self.observer)
        self.manifest = {
            "kind": "swml-custom-display-pass-through-observation-v1",
            "executableSha256": self.digest((self.game / "stormworks64.exe").read_bytes()),
            "launcherSha256": self.digest((self.game / "Launcher.exe").read_bytes()),
            "originalLoaderSha256": self.digest(self.original),
            "observerSha256": self.digest(self.observer),
        }
        (self.bundle / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    @staticmethod
    def digest(data):
        return hashlib.sha256(data).hexdigest()

    def invoke(self, mode):
        # Only diagnostic subprocesses, with a strict timeout. No Stormworks launched.
        return subprocess.run([
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            str(self.bundle / "observe.ps1"), "-Mode", mode,
            "-GameDirectory", str(self.game),
            "-NativeDataDirectory", str(self.native),
        ], capture_output=True, timeout=30)

    def unchanged(self):
        self.assertEqual((self.game / "iphlpapi.dll").read_bytes(), self.original)

    def pending(self, installed=None):
        run = "run-20261015-010203-" + "a" * 32
        directory = self.bundle / "runs" / run
        directory.mkdir(parents=True)
        (directory / "original-iphlpapi.dll").write_bytes(self.original)
        receipt = {"run": run, "gameDirectory": str(self.game),
                   "originalSha256": self.digest(self.original),
                   "observerSha256": self.digest(self.observer)}
        (self.bundle / "active.json").write_text(json.dumps(receipt), encoding="utf-8")
        (self.game / "iphlpapi.dll").write_bytes(installed or self.observer)
        return directory

    def test_preflight_does_not_install(self):
        result = self.invoke("Check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.unchanged()
        self.assertFalse((self.bundle / "active.json").exists())
        self.assertFalse((self.bundle / "runs").exists())

    def test_candidate_tampering_rejected(self):
        (self.bundle / "iphlpapi.dll").write_bytes(b"changed")
        self.assertNotEqual(self.invoke("Run").returncode, 0)
        self.unchanged()
        self.assertFalse((self.bundle / "active.json").exists())

    def test_installed_change_not_overwritten(self):
        (self.game / "iphlpapi.dll").write_bytes(b"user change")
        self.assertNotEqual(self.invoke("Run").returncode, 0)
        self.assertEqual((self.game / "iphlpapi.dll").read_bytes(), b"user change")

    def test_launch_failure_restores_and_preserves_previous_log(self):
        (self.game / "StormworksModLoader.log").write_bytes(b"previous log")
        result = self.invoke("Run")
        self.assertNotEqual(result.returncode, 0)  # invalid fake executable
        self.assertIn(b"Starting via Launcher.exe", result.stdout)  # reached process launch, not an earlier failure
        self.unchanged()
        self.assertFalse((self.bundle / "active.json").exists(), result.stderr)
        runs = list((self.bundle / "runs").iterdir())
        self.assertEqual(len(runs), 1)
        self.assertEqual((runs[0] / "previous.log").read_bytes(), b"previous log")
        self.assertEqual((runs[0] / "original-iphlpapi.dll").read_bytes(), self.original)
        self.assertTrue((runs[0] / "restored.json").exists())

    def test_recovery_restores_exact_original_and_retains_evidence(self):
        directory = self.pending()
        (self.game / "StormworksModLoader.log").write_bytes(b"observation evidence")
        result = self.invoke("Restore")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.unchanged()
        self.assertEqual((directory / "observation.log").read_bytes(), b"observation evidence")
        self.assertFalse((self.bundle / "active.json").exists())

    def test_recovery_will_not_overwrite_new_user_change(self):
        directory = self.pending(b"later user DLL")
        self.assertNotEqual(self.invoke("Restore").returncode, 0)
        self.assertEqual((self.game / "iphlpapi.dll").read_bytes(), b"later user DLL")
        self.assertTrue((self.bundle / "active.json").exists())
        self.assertTrue((directory / "original-iphlpapi.dll").exists())

    def test_corrupt_backup_not_restored(self):
        directory = self.pending()
        (directory / "original-iphlpapi.dll").write_bytes(b"corrupt backup")
        self.assertNotEqual(self.invoke("Restore").returncode, 0)
        self.assertEqual((self.game / "iphlpapi.dll").read_bytes(), self.observer)

    def test_pending_session_blocks_new_install(self):
        self.pending()
        self.assertNotEqual(self.invoke("Run").returncode, 0)
        self.assertEqual((self.game / "iphlpapi.dll").read_bytes(), self.observer)

    def test_recovery_does_not_need_the_candidate_copy(self):
        self.pending()
        (self.bundle / "iphlpapi.dll").unlink()
        result = self.invoke("Restore")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.unchanged()

    def test_launcher_tampering_rejected_before_install(self):
        (self.game / "Launcher.exe").write_bytes(b"changed launcher")
        self.assertNotEqual(self.invoke("Run").returncode, 0)
        self.unchanged()
        self.assertFalse((self.bundle / "active.json").exists())

    def test_recovery_does_not_need_launcher(self):
        self.pending()
        (self.game / "Launcher.exe").unlink()
        self.assertEqual(self.invoke("Restore").returncode, 0)
        self.unchanged()


class VehicleUiLauncherTests(LauncherTests):
    def setUp(self):
        super().setUp()
        self.manifest["kind"] = "swml-custom-display-vehicle-ui-observation-v1"
        (self.bundle / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_vehicle_mode_creates_no_fixtures_and_preserves_native_data(self):
        vehicle = self.native / "vehicles/user_vehicle.xml"
        vehicle.parent.mkdir()
        vehicle.write_bytes(b"user vehicle")
        result = self.invoke("Run")
        self.assertNotEqual(result.returncode, 0)  # Non-executable test Launcher.
        self.assertIn(b"VEHICLE UI OBSERVATION", result.stdout)
        self.assertIn(b"RETURN TO MAIN MENU (keep the game running)", result.stdout)
        self.assertIn(b"reload the same save", result.stdout)
        self.assertIn(b"Starting via Launcher.exe", result.stdout)
        self.unchanged()
        self.assertEqual(list(self.native.rglob("*")), [vehicle.parent, vehicle])
        self.assertEqual(vehicle.read_bytes(), b"user vehicle")


class SmallExperimentLauncherTests(LauncherTests):
    paths = (
        "mods/swml_cd_resolution_test/mod.xml",
        "mods/swml_cd_resolution_test/data/components/26f8c812f42cd41a9173ae6030881eb9.bin",
        "vehicles/SWML_Resolution_Test_24x16.xml",
        "vehicles/SWML_Resolution_Test_24x16/26f8c812f42cd41a9173ae6030881eb9.bin",
    )

    def setUp(self):
        super().setUp()
        self.manifest["kind"] = "swml-custom-display-small-resolution-experiment-v1"
        self.manifest["fixtures"] = {}
        for relative in self.paths:
            file = self.bundle / "fixtures" / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(("fixture: " + relative).encode())
            self.manifest["fixtures"][relative] = self.digest(file.read_bytes())
        (self.bundle / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def pending(self, installed=None):
        directory = super().pending(installed)
        path = self.bundle / "active.json"
        receipt = json.loads(path.read_text())
        receipt["nativeDataDirectory"] = str(self.native)
        receipt["createdFixtures"] = []
        for relative in self.paths:
            target = self.native / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.bundle / "fixtures" / relative, target)
            receipt["createdFixtures"].append({"path": relative, "sha256": self.manifest["fixtures"][relative]})
        path.write_text(json.dumps(receipt), encoding="utf-8")
        return directory

    def test_launch_failure_cleans_only_own_new_fixtures(self):
        existing = self.native / "vehicles/my_vehicle.xml"
        existing.parent.mkdir(parents=True)
        existing.write_bytes(b"user data must stay")
        result = self.invoke("Run")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Starting via Launcher.exe", result.stdout)
        self.unchanged()
        for relative in self.paths:
            self.assertFalse((self.native / relative).exists(), relative)
        self.assertEqual(existing.read_bytes(), b"user data must stay")

    def test_existing_test_vehicle_is_not_overwritten(self):
        vehicle = self.native / self.paths[2]
        vehicle.parent.mkdir(parents=True)
        vehicle.write_bytes(b"user edited test vehicle")
        self.assertNotEqual(self.invoke("Run").returncode, 0)
        self.unchanged()
        self.assertEqual(vehicle.read_bytes(), b"user edited test vehicle")

    def test_changed_test_files_and_unexpected_files_are_retained(self):
        directory = self.pending()
        edited = self.native / self.paths[2]
        edited.write_bytes(b"user edits during experiment")
        other = self.native / "mods/swml_cd_resolution_test/my_notes.txt"
        other.write_bytes(b"unrecognized file")
        result = self.invoke("Restore")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.unchanged()
        self.assertEqual(edited.read_bytes(), b"user edits during experiment")
        self.assertEqual(other.read_bytes(), b"unrecognized file")
        self.assertIn(self.paths[2], json.loads((directory / "restored.json").read_text(encoding="utf-8-sig"))["retainedFixtures"])

    def test_tampered_fixture_refused_before_install(self):
        (self.bundle / "fixtures" / self.paths[0]).write_bytes(b"tampered")
        self.assertNotEqual(self.invoke("Run").returncode, 0)
        self.unchanged()
        self.assertFalse((self.bundle / "active.json").exists())

    def test_recovery_without_fixture_sources(self):
        self.pending()
        for relative in self.paths:
            (self.bundle / "fixtures" / relative).unlink()
        result = self.invoke("Restore")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.unchanged()
        self.assertTrue(all(not (self.native / relative).exists() for relative in self.paths))

    def test_receipt_path_traversal_refused(self):
        self.pending()
        path = self.bundle / "active.json"
        receipt = json.loads(path.read_text())
        receipt["createdFixtures"][0]["path"] = "../user-file"
        path.write_text(json.dumps(receipt), encoding="utf-8")
        self.assertNotEqual(self.invoke("Restore").returncode, 0)
        self.assertTrue((self.native / self.paths[0]).exists())


if __name__ == "__main__":
    unittest.main()
