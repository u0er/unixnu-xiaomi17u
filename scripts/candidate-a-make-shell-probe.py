#!/usr/bin/env python3
"""Demonstrate make SHELL interception limits using harmless sandboxed fixtures."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(os.environ['RUNNER_TEMP']).resolve() / 'candidate-a-make-shell-probe'
root.mkdir(parents=True, exist_ok=False)
guard = root / 'deny-shell'
guard.write_text('#!/bin/bash\nprintf "guard invoked\\n" >> "${PROBE_ROOT}/guard.log"\nexit 88\n')
guard.chmod(0o755)
tools = [Path('/usr/bin/make').resolve(), Path('/bin/bash').resolve(), Path('/usr/bin/touch').resolve(), guard]
policy = '(version 1)\n(allow default)\n(deny network*)\n(deny process-exec)\n'
policy += '(allow process-exec ' + ' '.join(f'(literal "{tool}")' for tool in tools) + ')\n'
policy += f'(deny file-write*)\n(allow file-write* (subpath "{root}") (literal "/dev/null"))\n'
(root / 'probe.sb').write_text(policy)
makefiles = {
    'baseline': 'all:\n\t/usr/bin/touch baseline.marker\n',
    'override': 'override SHELL := /bin/bash\nall:\n\t/usr/bin/touch override.marker\n',
    'shell-function': 'override SHELL := /bin/bash\nprobe := $(shell /usr/bin/touch shell-function.marker)\nall:\n\t@:\n',
    'recursive': 'override SHELL := /bin/bash\nall:\n\t$(MAKE) -f child.mk SHELL=/bin/bash child\n',
}
(root / 'child.mk').write_text('child:\n\t/usr/bin/touch recursive.marker\n')
report = {'scope': 'harmless GNU Make fixture; no Candidate make or compilation', 'cases': [],
          'programs': [], 'status': 'FAIL', 'errors': []}
try:
    for tool in tools:
        report['programs'].append({'path': str(tool), 'sha256': hashlib.sha256(tool.read_bytes()).hexdigest()})
    report['make_version'] = subprocess.run(['/usr/bin/make', '--version'], capture_output=True,
                                            text=True, check=True).stdout.splitlines()[0]
    for name, text in makefiles.items():
        makefile = root / (name + '.mk')
        makefile.write_text(text)
        env = {'PATH': '/usr/bin:/bin', 'HOME': str(root), 'TMPDIR': str(root), 'PROBE_ROOT': str(root), 'LC_ALL': 'C'}
        argv = ['/usr/bin/sandbox-exec', '-f', str(root / 'probe.sb'), '/usr/bin/make',
                '-f', str(makefile), 'SHELL=' + str(guard), 'all']
        result = subprocess.run(argv, cwd=root, env=env, text=True, capture_output=True, check=False)
        (root / (name + '.stdout')).write_text(result.stdout)
        (root / (name + '.stderr')).write_text(result.stderr)
        marker = (root / (name + '.marker')).exists()
        report['cases'].append({'name': name, 'argv': argv, 'cwd': str(root), 'exit': result.returncode,
                                'marker_created': marker, 'makefile': text})
    cases = {item['name']: item for item in report['cases']}
    if cases['baseline']['marker_created'] or cases['baseline']['exit'] == 0:
        raise RuntimeError('baseline deny-shell did not stop the recipe')
    if not all(cases[name]['marker_created'] for name in ('override', 'shell-function', 'recursive')):
        raise RuntimeError('one or more expected override demonstrations did not reproduce')
    report['status'] = 'BYPASS_REPRODUCED'
    report['limitation'] = 'SHELL interception alone is not an enforceable make execution boundary.'
except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
    report['errors'].append(str(error))
finally:
    (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
raise SystemExit(0 if report['status'] == 'BYPASS_REPRODUCED' else 1)
