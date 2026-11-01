"""Create-new vehicle UI observation bundle. Never install or launch anything."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE_EXE = "55914301628a31b278ff1d4aa9fd0c0446341759bf72083a8e3aa1dff859e6c3"
ORIGINAL_LOADER = "c882c4b1d4b8eb73f59801c82d743a874328c546f98c069cf007c0377c1c89cb"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exports(path):
    import pefile
    with pefile.PE(str(path)) as pe:
        return sorted((e.ordinal, e.name, e.forwarder) for e in pe.DIRECTORY_ENTRY_EXPORT.symbols)


def stage(game, output):
    game, output = Path(game).resolve(), Path(output).resolve()
    if output.exists():
        raise ValueError("Bundle already exists; no overwrite")
    if digest(game / "stormworks64.exe") != REFERENCE_EXE:
        raise ValueError("Executable is not the authenticated reference")
    if digest(game / "iphlpapi.dll") != ORIGINAL_LOADER:
        raise ValueError("Installed loader changed; refusing to stage")
    candidate = ROOT / "build/iphlpapi.dll"
    original_exports, candidate_exports = exports(game / "iphlpapi.dll"), exports(candidate)
    if len(candidate_exports) != 313 or original_exports != candidate_exports:
        raise ValueError("Proxy exports differ from the installed loader")
    files = {
        "build/iphlpapi.dll": "iphlpapi.dll",
        "tools/custom_display_observation.ps1": "observe.ps1",
        "tools/custom_display_observation_run.bat": "TESTER.bat",
        "tools/custom_display_observation_restore.bat": "RESTAURER.bat",
        "docs/custom_display_vehicle_observation_readme.md": "LIRE-MOI.md",
        "tools/custom_display_vehicle_report.py": "audit.py",
        "build/custom-display-vehicle-activity-tests.log": "offline-tests.log",
        "build/custom-display-vehicle-activity-build.log": "build.log",
    }
    # Read/check everything before creating output; no fixture directory exists.
    contents = {target: (ROOT / source).read_bytes() for source, target in files.items()}
    manifest = dict(kind="swml-custom-display-vehicle-ui-observation-v1",
                    executableSha256=REFERENCE_EXE, originalLoaderSha256=ORIGINAL_LOADER,
                    observerSha256=digest(candidate), launcherSha256=digest(game / "Launcher.exe"),
                    runtimeValidated=False, installed=False, proxyExportCount=313, observerProtocolVersion=2,
                    observationOnly=True, files={k: hashlib.sha256(v).hexdigest() for k, v in contents.items()})
    output.mkdir(parents=True)
    for name, data in contents.items():
        with (output / name).open("xb") as file:
            file.write(data)
    with (output / "manifest.json").open("x", encoding="utf-8") as file:
        json.dump(manifest, file, indent=2)
        file.write("\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(stage(args.game, args.output), indent=2))


if __name__ == "__main__":
    main()
