"""Builds the demo content from YOUR copy of the game (no game file is shipped here).

Copies rom/data/definitions/01_block.xml to content/data/definitions/sf_demo_block.xml
and renames the component, so a new "StormFabric Block" appears in the vehicle editor.

    python make_content.py "C:\\path\\to\\Stormworks"
"""
import re
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    source = Path(sys.argv[1]) / 'rom' / 'data' / 'definitions' / '01_block.xml'
    text = source.read_text(encoding='utf-8')
    renamed, count = re.subn(r'<definition name="Block"', '<definition name="StormFabric Block"', text, count=1)
    if count != 1:
        raise SystemExit(f'unexpected format in {source}')
    target = Path(__file__).resolve().parent / 'content' / 'data' / 'definitions' / 'sf_demo_block.xml'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(renamed, encoding='utf-8', newline='')
    print(f'wrote {target}')


if __name__ == '__main__':
    main()
