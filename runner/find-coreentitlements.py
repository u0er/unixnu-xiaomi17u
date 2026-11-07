#!/usr/bin/env python3
"""Inventory CoreEntitlements headers on the runner without exporting them."""
import hashlib
import json
import os
from pathlib import Path
import subprocess


developer = Path('/Applications/Xcode_26.5.app/Contents/Developer')
sdk = Path(subprocess.check_output(['/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip())
roots = [developer, Path('/Library/Developer/KDKs')]
results, errors = [], []
for root in roots:
    if not root.exists():
        continue
    def report_error(error):
        errors.append(str(error))
    for directory, subdirs, names in os.walk(root, followlinks=False, onerror=report_error):
        subdirs[:] = [name for name in subdirs if not (Path(directory) / name).is_symlink()]
        if 'coreentitlements' not in directory.lower():
            continue
        for name in names:
            if not name.endswith(('.h', '.modulemap')):
                continue
            path = Path(directory) / name
            try:
                data = path.read_bytes()
                results.append({'path': str(path), 'resolved': str(path.resolve()), 'bytes': len(data),
                                'sha256': hashlib.sha256(data).hexdigest(),
                                'under_sdk': path.resolve().is_relative_to(sdk.resolve())})
            except OSError as error:
                errors.append(str(error))
result = {'xcode': '26.5 (17F42)', 'sdk': str(sdk), 'sdk_version': '26.5',
          'sdk_build': '23F73', 'scanned_roots': [str(p) for p in roots],
          'headers': results, 'errors': errors}
output = Path(os.environ['RUNNER_TEMP']) / 'candidate-a-evidence/coreentitlements-sources.json'
output.write_text(json.dumps(result, indent=2) + '\n')
print(f'CoreEntitlements header inventory: {len(results)} files; {len(errors)} read errors')
if errors:
    raise SystemExit('CoreEntitlements inventory incomplete')
