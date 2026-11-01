"""Package the built loader and source-only Custom Display prototype, never install.

Run after build.bat and tests/run_tests.bat. Existing delivery folders are refused.
No game binaries/resources, local configuration, old backups or example binaries
are included. This is deliberately NOT advertised as a functional Custom Display.
"""
import argparse
from datetime import datetime, timezone
import hashlib
from pathlib import Path
import shutil
import struct
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checksums(root):
    files = sorted(p for p in root.rglob("*") if p.is_file())
    (root / "SHA256SUMS.txt").write_text(
        "".join(f"{digest(p)}  {p.relative_to(root).as_posix()}\n" for p in files),
        encoding="utf-8",
    )


def archive(root, destination):
    with zipfile.ZipFile(destination, "x", zipfile.ZIP_DEFLATED) as output:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                output.write(path, path.relative_to(root).as_posix())
    with zipfile.ZipFile(destination) as output:
        if output.testzip() is not None:
            raise RuntimeError(f"Archive integrity check failed: {destination}")


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%SZ")
    delivery = args.output or project / "build" / f"delivery-{stamp}"
    dll = project / "build/iphlpapi.dll"
    binary = dll.read_bytes()
    if binary[:2] != b"MZ":
        raise RuntimeError("Loader is not a PE file")
    pe = struct.unpack_from("<I", binary, 0x3C)[0]
    if binary[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", binary, pe + 4)[0] != 0x8664:
        raise RuntimeError("Loader is not Windows x64")
    if not struct.unpack_from("<H", binary, pe + 22)[0] & 0x2000:
        raise RuntimeError("Loader is not a DLL")
    build_log = project / "build/delivery-build.log"
    test_log = project / "build/delivery-tests-portable.log"
    # Require the completed logs from this delivery workflow. They are included
    # unchanged, including SKIP and intentional negative-test diagnostics.
    if "OK :" not in build_log.read_text(errors="replace"):
        raise RuntimeError("Completed delivery build log is missing")
    if not test_log.read_text(errors="replace").rstrip().endswith(
        "p0_demo_tests: PASS (production SDK wrappers, fake native widgets; no gameplay proof)"
    ):
        raise RuntimeError("Completed available-test-suite log is missing")
    delivery.mkdir(parents=True, exist_ok=False)
    runtime = delivery / "loader"
    runtime.mkdir()
    shutil.copy2(dll, runtime / dll.name)
    shutil.copy2(project / "docs/delivery_readme.md", runtime / "README.md")
    for directory in ("sdk", "docs", "validation", "prototype/src", "prototype/tests"):
        (runtime / directory).mkdir(parents=True)
    shutil.copy2(project / "include/sw_mod_sdk.h", runtime / "sdk/sw_mod_sdk.h")
    for name in ("custom_displays.md", "engine_discovery.md"):
        shutil.copy2(project / "docs" / name, runtime / "docs" / name)
    for log in (build_log, test_log):
        shutil.copy2(log, runtime / "validation" / log.name)
    shutil.copy2(project / "src/custom_display_model.h", runtime / "prototype/src/custom_display_model.h")
    shutil.copy2(project / "tests/custom_display_model_tests.cpp", runtime / "prototype/tests/custom_display_model_tests.cpp")
    checksums(runtime)
    archive(runtime, delivery / "StormworksModLoader-experimental-win64.zip")

    source = delivery / "sources"
    source.mkdir()
    for name in ("README.md", "build.bat", "install.bat", "uninstall.bat"):
        shutil.copy2(project / name, source / name)
    allowed = {".h", ".cpp", ".c", ".json", ".lua", ".bat", ".md", ".py", ".java"}
    for directory in ("src", "include", "examples", "tests", "docs", "tools"):
        for path in sorted((project / directory).rglob("*")):
            if not path.is_file() or "__pycache__" in path.parts:
                continue
            if path.suffix not in allowed and not path.name.endswith(".commands.txt"):
                continue
            relative = path.relative_to(project)
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
    checksums(source)
    archive(source, delivery / "StormworksModLoader-and-CustomDisplay-prototype-SOURCES.zip")
    with (delivery / "SHA256SUMS-archives.txt").open("w", encoding="utf-8") as output:
        for path in sorted(delivery.glob("*.zip")):
            output.write(f"{digest(path)}  {path.name}\n")
    print(delivery)
    print("Loader packaged; Custom Display is SOURCE-ONLY, not a working mod.")
    print("No installation or in-game validation performed.")


if __name__ == "__main__":
    main()
