#!/usr/bin/env python3
"""Inspect an actual Apple-built kernel's LTO symbols/chains, without digests."""
import json
from pathlib import Path
import sys

from kc_image import Image


def main():
    root = Path(sys.argv[1]).resolve()
    directory = root / 'obj/RELEASE_ARM64_NEZHA'
    candidates = [directory / ('mach.release.nezha'+suffix) for suffix in ('.unstripped.noctf', '.unstripped')]
    selected = next((path for path in candidates if path.is_file()), None)
    if selected is None:
        raise SystemExit('Actual unstripped RELEASE kernel is absent')
    data = selected.read_bytes()
    image = Image(data)
    chains = image.fixups()
    if image.kind != 2 or not chains or image.command(0x1e) is None:
        raise SystemExit('Apple reference lacks actual chained-fixup/split-segment records')
    if not (directory / 'justlto.o').exists():
        raise SystemExit('Actual LTO generated object is absent')
    report = {'kernel': str(selected), 'bytes': len(data), 'defined_symbol_addresses': len(image.symbols()),
              'pointer_sites': len(chains), 'split_segment_command': True,
              'actual_lto_generated_object': True, 'runtime_qualified': False,
              'purpose': 'symbol naming reference only; not a device candidate'}
    (root / 'reference-report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
