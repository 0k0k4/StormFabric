"""Generate the complete Custom Display native mod for Stormworks.

Places mod into %APPDATA%/Stormworks/data/mods/swml_custom_display
Contains:
- mod.xml
- Base catalog item: data/definitions/swml_custom_display.xml
- Parametric sizes: data/definitions/swml_cd_{W}x{H}.xml
- Parametric meshes: meshes/swml_cd_{W}x{H}.mesh and arrow mesh
- Embedded archives: data/components/{hash}.bin
"""
import os
import sys
import struct
import math
import hashlib
from pathlib import Path

# Add tools dir to path to import custom_display_assets
tools_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(tools_dir))

from custom_display_assets import (
    DisplaySpec, build_frame, build_arrow, build_definition, build_archive
)
from custom_display_probe import encode_archive, archive_identity

SIZES = [(w, h) for w in range(1, 17) for h in range(1, 17)]

def main():
    appdata = os.environ.get("APPDATA")
    if not appdata:
        print("ERROR: APPDATA environment variable not found")
        return 1

    game_path = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("STORMWORKS_DIR", r"..\Stormworks")`r`n    game_dir = Path(game_path)
    if not game_dir.exists():
        print(f"ERROR: Game dir not found at {game_dir}")
        return 1

    mod_dir = Path(appdata) / "Stormworks" / "data" / "mods" / "swml_custom_display"
    defs_dir = mod_dir / "data" / "definitions"
    meshes_dir = mod_dir / "meshes"
    comps_dir = mod_dir / "data" / "components"
    lang_dir = mod_dir / "data" / "languages" / "english"

    defs_dir.mkdir(parents=True, exist_ok=True)
    meshes_dir.mkdir(parents=True, exist_ok=True)
    comps_dir.mkdir(parents=True, exist_ok=True)
    lang_dir.mkdir(parents=True, exist_ok=True)

    rom_defs_dir = game_dir / "rom" / "data" / "definitions"
    rom_meshes_dir = game_dir / "rom" / "meshes"
    rom_lang_dir = game_dir / "rom" / "data" / "languages" / "english"
    rom_defs_dir.mkdir(parents=True, exist_ok=True)
    rom_meshes_dir.mkdir(parents=True, exist_ok=True)
    rom_lang_dir.mkdir(parents=True, exist_ok=True)

    # 1. mod.xml
    mod_xml = """<?xml version="1.0" encoding="UTF-8"?>
<mod name="Custom Display" author="StormworksModLoader" desc="Dynamic custom displays with configurable physical size and pixel resolution."/>
"""
    (mod_dir / "mod.xml").write_text(mod_xml, encoding="utf-8")
    print(f"Written: {mod_dir / 'mod.xml'}")

    # Load stock assets
    monitor1_xml_path = game_dir / "rom" / "data" / "definitions" / "monitor_1.xml"
    screen1_mesh_path = game_dir / "rom" / "meshes" / "component_screen_1.mesh"
    arrow1_mesh_path = game_dir / "rom" / "meshes" / "arrow_screen_1.mesh"

    monitor1_xml = monitor1_xml_path.read_bytes()
    screen1_mesh = screen1_mesh_path.read_bytes()
    arrow1_mesh = arrow1_mesh_path.read_bytes()

    stock_assets = {
        "data/definitions/monitor_1.xml": monitor1_xml,
        "meshes/component_screen_1.mesh": screen1_mesh,
        "meshes/arrow_screen_1.mesh": arrow1_mesh,
    }

    # 2. Base catalog item: swml_custom_display.xml
    # Clicking this item or selecting from toolbar opens the native dialog!
    cat_text = monitor1_xml.decode("utf-8")
    cat_text = cat_text.replace('name="Monitor 1x1"', 'name="Custom Display"')
    cat_text = cat_text.replace('tags=""', 'tags="display,screen,monitor,custom,swml"')
    cat_text = cat_text.replace('description="Displays video data."', 
                                'description="Custom video screen. Click in catalogue or hotbar to configure physical size and pixel resolution."')
    cat_text = cat_text.replace('short_description="A video screen."', 
                                'short_description="Custom Screen"')
    (defs_dir / "swml_custom_display.xml").write_text(cat_text, encoding="utf-8")
    (rom_defs_dir / "swml_custom_display.xml").write_text(cat_text, encoding="utf-8")
    print(f"Written catalog base: {defs_dir / 'swml_custom_display.xml'}")

    # 3. Generate all sizes (1x1 to 10x10)
    # flags="128" hides them from inventory/catalogue so they do not clutter the UI
    tsv_lines = [
        "DEF_SWML_CUSTOM_DISPLAY_NAME\tCustom Display",
        "def_swml_custom_display_name\tCustom Display",
        "def_swml_custom_display_desc\tCustom video screen with configurable physical size and pixel resolution.",
        "def_swml_custom_display_s_desc\tCustom Screen",
        "def_swml_custom_display_node_0_label\tVideo Signal",
        "def_swml_custom_display_node_1_label\tTouch Output",
        "def_swml_custom_display_node_2_label\tElectric",
        "def_swml_custom_display_node_3_label\tPower Switch",
    ]

    NUM_SLOTS = 4

    for w, h in SIZES:
        ident_base = f"swml_cd_{w}x{h}"
        spec = DisplaySpec(width=w, height=h, pixels_x=w * 32, pixels_y=h * 32)

        # Meshes (generated once per physical size)
        frame_mesh = build_frame(screen1_mesh, spec)
        arrow_mesh = build_arrow(arrow1_mesh, spec)

        (meshes_dir / f"{ident_base}.mesh").write_bytes(frame_mesh)
        (meshes_dir / f"{ident_base}_arrow.mesh").write_bytes(arrow_mesh)
        (rom_meshes_dir / f"{ident_base}.mesh").write_bytes(frame_mesh)
        (rom_meshes_dir / f"{ident_base}_arrow.mesh").write_bytes(arrow_mesh)

        # Resources for archives
        resources = (
            (f"meshes/{ident_base}.mesh", frame_mesh),
            (f"meshes/{ident_base}_arrow.mesh", arrow_mesh)
        )

        for slot in range(1, NUM_SLOTS + 1):
            ident = ident_base if slot == 1 else f"{ident_base}_s{slot}"

            # Definition XML
            xml_bytes = build_definition(monitor1_xml, spec)
            xml_str = xml_bytes.decode("utf-8")
            xml_str = xml_str.replace(f'name="Custom Display {w}x{h} ({w*32}x{h*32})"', 
                                      f'name="Custom Display {w}x{h}"')
            xml_str = xml_str.replace(f'mesh_data_name="meshes/{spec.identity}.mesh"',
                                      f'mesh_data_name="meshes/{ident_base}.mesh"')
            xml_str = xml_str.replace(f'mesh_editor_only_name="meshes/{spec.identity}_arrow.mesh"',
                                      f'mesh_editor_only_name="meshes/{ident_base}_arrow.mesh"')
            xml_str = xml_str.replace('tags="custom_display_size"', 'tags=""')

            def_file = defs_dir / f"{ident}.xml"
            def_file.write_text(xml_str, encoding="utf-8")
            (rom_defs_dir / f"{ident}.xml").write_text(xml_str, encoding="utf-8")

            # Component archive .bin
            archive_bytes = encode_archive(
                type("Archive", (), {
                    "definition_id": ident,
                    "xml": xml_str.encode("utf-8"),
                    "resources": resources
                })()
            )
            archive_id = archive_identity(archive_bytes)
            (comps_dir / f"{archive_id}.bin").write_bytes(archive_bytes)

            # Translation entries
            tsv_lines.append(f"DEF_{ident.upper()}_NAME\tCustom Display {w}x{h}")
            tsv_lines.append(f"def_{ident}_name\tCustom Display {w}x{h}")
            tsv_lines.append(f"def_{ident}_desc\tCustom video screen {w}x{h}.")
            tsv_lines.append(f"def_{ident}_s_desc\tCustom Screen {w}x{h}")
            tsv_lines.append(f"def_{ident}_node_0_label\tVideo Signal")
            tsv_lines.append(f"def_{ident}_node_1_label\tTouch Output")
            tsv_lines.append(f"def_{ident}_node_2_label\tElectric")
            tsv_lines.append(f"def_{ident}_node_3_label\tPower Switch")

    tsv_content = "\n".join(tsv_lines) + "\n"
    (lang_dir / "language.tsv").write_text(tsv_content, encoding="utf-8")
    (rom_lang_dir / "language.tsv").write_text(tsv_content, encoding="utf-8")
    (game_dir / "rom" / "data" / "languages" / "language.tsv").write_text(tsv_content, encoding="utf-8")
    (mod_dir / "data" / "languages" / "language.tsv").write_text(tsv_content, encoding="utf-8")

    print(f"Successfully generated {len(SIZES)} Custom Display sizes in {mod_dir} and {game_dir / 'rom'}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
