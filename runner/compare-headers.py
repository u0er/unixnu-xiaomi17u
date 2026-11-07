#!/usr/bin/env python3
"""Diagnose frozen header differences without uploading the SDK or headers."""
import json
import os
from pathlib import Path
import plistlib
import sys
import tarfile

bundle, tree, output = map(Path, sys.argv[1:])
differences = []
with tarfile.open(bundle, 'r:gz') as archive:
    for member in archive:
        relative = Path(member.name)
        if relative.is_absolute() or '..' in relative.parts:
            raise SystemExit('unsafe frozen bundle path')
        target = tree / relative
        if member.isdir():
            problem = None if target.is_dir() else 'directory missing'
        elif member.issym():
            problem = None if target.is_symlink() and os.readlink(target) == member.linkname else 'symlink mismatch'
        elif member.isfile():
            expected = archive.extractfile(member)
            if not expected or target.is_symlink() or not target.is_file():
                problem = 'file missing'
            else:
                before, after = expected.read(), target.read_bytes()
                problem = None if before == after else 'content mismatch'
                if problem and relative.suffix == '.plist':
                    try:
                        problem = 'plist encoding only' if plistlib.loads(before) == plistlib.loads(after) else 'plist values differ'
                    except (ValueError, plistlib.InvalidFileException):
                        problem = 'plist parse failed'
        else:
            problem = 'unsupported bundle entry'
        if problem:
            differences.append({'path': member.name, 'reason': problem})
output.write_text(json.dumps({'differences': differences}, indent=2) + '\n')
print(f'Frozen header diagnostic: {len(differences)} differences; see header-differences.json')
