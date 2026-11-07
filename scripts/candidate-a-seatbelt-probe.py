#!/usr/bin/env python3
"""Test Seatbelt exec and write restrictions with harmless host programs.

This is a capability probe only; it does not run make or compile anything.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(os.environ['RUNNER_TEMP']).resolve() / 'candidate-a-seatbelt-probe'
root.mkdir(parents=True, exist_ok=False)
allowed = root / 'allowed'
allowed.mkdir()
# macOS /bin/sh re-execs /bin/bash; start the actual shell explicitly so the
# path allowlist covers the program the OS really executes.
sh = Path('/bin/bash').resolve()
yes = Path('/usr/bin/true').resolve()
blocked = Path('/bin/ls').resolve()
outside = root / 'outside.txt'
inside = allowed / 'inside.txt'
report = {'scope': 'Seatbelt capability probe only; no XNU make or compilation', 'cases': [],
          'programs': {}, 'status': 'FAIL', 'errors': []}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


try:
    for name, path in [('shell', sh), ('allowed_program', yes), ('denied_program', blocked)]:
        report['programs'][name] = {'path': str(path), 'sha256': sha(path)}
    # Probe exec filtering separately from system initialization. This profile
    # is not the final build sandbox and intentionally grants general reads.
    profile = ('(version 1)\n(allow default)\n(deny network*)\n'
               '(deny process-exec)\n'
               f'(allow process-exec (literal "{sh}") (literal "{yes}"))\n'
               '(deny file-write*)\n'
               f'(allow file-write* (subpath "{allowed}") (literal "/dev/null"))\n')
    (root / 'probe.sb').write_text(profile)
    cases = [
        ('allowed-exec', f'"{yes}"', 0),
        ('denied-exec', f'"{blocked}" /', 126),
        ('nested-denied-exec', f'"{sh}" -c \'"{blocked}" /\'', 126),
        ('allowed-write', f'echo allowed > "{inside}"', 0),
        ('denied-write', f'echo blocked > "{outside}"', 1),
        # Demonstrate that path-based exec filtering does not distinguish argv.
        ('allowed-program-arbitrary-argument', f'"{yes}" forbidden-target', 0),
    ]
    for name, command, expected in cases:
        argv = ['/usr/bin/sandbox-exec', '-f', str(root / 'probe.sb'), str(sh), '-c', command]
        result = subprocess.run(argv, text=True, capture_output=True, check=False)
        (root / (name + '.stdout')).write_text(result.stdout)
        (root / (name + '.stderr')).write_text(result.stderr)
        passed = result.returncode == expected
        report['cases'].append({'name': name, 'argv': argv, 'exit': result.returncode,
                                'expected_exit': expected, 'passed': passed})
    if not inside.is_file() or outside.exists():
        raise RuntimeError('write boundary did not match expectations')
    if not all(item['passed'] for item in report['cases']):
        raise RuntimeError('one or more isolation probes failed')
    report['status'] = 'PASS_CAPABILITY_ONLY'
    report['limitation'] = ('Seatbelt path filtering does not constrain the allowed program arguments; '
                            'make/clang subprocess enforcement and the actual build policy are not validated.')
except (OSError, RuntimeError) as error:
    report['errors'].append(str(error))
finally:
    (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
raise SystemExit(0 if report['status'] == 'PASS_CAPABILITY_ONLY' else 1)
