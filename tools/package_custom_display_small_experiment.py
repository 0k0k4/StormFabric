"""Stage the fixed local 24x16 experiment; never install/launch/overwrite files."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET

from custom_display_assets import ASSET_HASHES, DisplaySpec, build_archive, load_reviewed_assets
from custom_display_probe import PROBE_ID, archive_identity, decode_archive

IDENTITY = "swml_cd_v1_b1x1_p24x16"
ARCHIVE = "26f8c812f42cd41a9173ae6030881eb9"
SHA256 = "c809a2f3a5277d7048b287920dbb4b6c8bea71a834f06f83e0b2aea267d48e56"
ORIGINAL_LOADER = "c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb"
REFERENCE_EXE = "55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare_vehicle(raw):
    if len(raw) > 8 * 1024 * 1024:
        raise ValueError("Vehicle template too large for local experiment")
    root = ET.fromstring(raw)
    if root.tag != "vehicle" or len(root.findall(f".//c[@d='{PROBE_ID}']")) != 1:
        raise ValueError("Exactly one existing witness component is required")
    if not root.findall(".//c[@d='camera_small']") or not root.findall("./logic_node_links/logic_node_link"):
        raise ValueError("Connected camera template required")
    for component in root.findall(".//c"):
        definition = component.get("d", "")
        if definition not in ("", PROBE_ID, "camera_small", "battery_medium", "gate_bool_constant"):
            raise ValueError("Unexpected component in the fixed local camera template")
    original = f'd="{PROBE_ID}"'.encode()
    if raw.count(original) != 1:
        raise ValueError("Unexpected vehicle XML encoding; no rewrite attempted")
    # Keep all native XML, positions, wiring and properties byte-for-byte.
    return raw.replace(original, f'd="{IDENTITY}"'.encode(), 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--vehicle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    game, output = args.game.resolve(), args.output.resolve()
    if output.exists():
        raise ValueError("Bundle already exists; refusing overwrite")
    if digest((game / "stormworks64.exe").read_bytes()) != REFERENCE_EXE:
        raise ValueError("Unreviewed executable; no experiment staged")
    if digest((game / "iphlpapi.dll").read_bytes()) != ORIGINAL_LOADER:
        raise ValueError("Installed loader changed; review before staging")
    data = build_archive(load_reviewed_assets(game), DisplaySpec(1,1,24,16))
    metadata = {"source_asset_sha256": ASSET_HASHES, "runtime_validated": False,
                "definition_id": IDENTITY, "physical_blocks": [1,1],
                "requested_resolution": [24,16], "unmodified_engine_resolution": [32,32]}
    if archive_identity(data) != ARCHIVE or digest(data) != SHA256 or decode_archive(data).definition_id != IDENTITY:
        raise ValueError("Generated archive does not match the native experiment allowlist")
    vehicle_raw = args.vehicle.read_bytes()
    vehicle = prepare_vehicle(vehicle_raw)
    # Verify proxy exports without executing any code in either DLL.
    import pefile
    def exports(path):
        with pefile.PE(str(path)) as pe:
            return sorted((entry.ordinal,entry.name,entry.forwarder) for entry in pe.DIRECTORY_ENTRY_EXPORT.symbols)
    original_exports, candidate_exports = exports(game / "iphlpapi.dll"), exports(root / "build/iphlpapi.dll")
    if len(candidate_exports) != 313 or original_exports != candidate_exports:
        raise ValueError("Proxy exports differ from the installed loader")
    fixture_data = {
        "mods/swml_cd_resolution_test/mod.xml": b'<?xml version="1.0" encoding="UTF-8"?>\n<mod name="SWML Resolution Test 24x16" author="StormworksModLoader" desc="Temporary local resolution experiment, not the finished Custom Display mod."/>\n',
        f"mods/swml_cd_resolution_test/data/components/{ARCHIVE}.bin": data,
        "vehicles/SWML_Resolution_Test_24x16.xml": vehicle,
        f"vehicles/SWML_Resolution_Test_24x16/{ARCHIVE}.bin": data,
    }
    manifest = {
        "kind": "swml-custom-display-small-resolution-experiment-v1",
        "executableSha256": REFERENCE_EXE,
        "originalLoaderSha256": ORIGINAL_LOADER,
        "observerSha256": digest((root / "build/iphlpapi.dll").read_bytes()),
        "launcherSha256": digest((game / "Launcher.exe").read_bytes()),
        "fixtures": {path:digest(content) for path,content in fixture_data.items()},
        "templateVehicleSha256": digest(vehicle_raw),
        "nativeArchiveMd5": ARCHIVE,
        "requestedResolution": [24,16], "physicalBlocks": [1,1],
        "runtimeValidated": False, "installed": False,
    }
    output.mkdir(parents=True)
    for relative, content in fixture_data.items():
        target = output / "fixtures" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    for source, target in (
        ("build/iphlpapi.dll", "iphlpapi.dll"),
        ("tools/custom_display_observation.ps1", "observe.ps1"),
        ("tools/custom_display_observation_run.bat", "TESTER.bat"),
        ("tools/custom_display_observation_restore.bat", "RESTAURER.bat"),
        ("docs/custom_display_small_resolution_readme.md", "LIRE-MOI.md"),
        ("build/custom-display-small-resolution-full-tests.log", "offline-tests.log"),
        ("build/custom-display-small-resolution-build.log", "build.log"),
    ):
        shutil.copyfile(root / source, output / target)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (output / "geometry-manifest.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"bundle":str(output), **manifest},indent=2))


if __name__ == "__main__":
    main()
