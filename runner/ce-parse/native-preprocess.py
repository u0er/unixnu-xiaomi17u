#!/usr/bin/env python3
"""Execute only the expanded native Clang recipe with preprocessing flags."""
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys

dry, cwd, work, evidence = map(pathlib.Path, sys.argv[1:5])
component, target = sys.argv[5:7]
source_name = 'pmap.c' if component == 'osfmk' else 'ubc_subr.c'
lines = []
for line in dry.read_text().splitlines():
    if 'clang' in line and source_name in line and ' -c ' in (' ' + line + ' '):
        try:
            args = shlex.split(line.strip())
        except ValueError:
            continue
        if args and pathlib.Path(args[0]).name in ('clang', 'clang++'):
            lines.append(args)
if len(lines) != 1:
    (evidence / f'{component}-preprocess.log').write_text(
        f'NOT_RUN: expected exactly one native compiler command, found {len(lines)}\n')
    raise SystemExit(0)
original = lines[0]
command = []
skip = False
for arg in original:
    if skip:
        skip = False
        continue
    if arg in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
        skip = True
        continue
    if arg in ('-c', '-MD', '-MMD', '-MP'):
        continue
    command.append(arg)
output = work / f'{component}-preprocessed.i'
command.extend(['-E', '-H', '-o', str(output)])
result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
log = f'cwd: {cwd}\ncommand: {shlex.join(command)}\nexit: {result.returncode}\n{result.stdout}'
if result.returncode == 0:
    data = output.read_bytes()
    text = data.decode(errors='replace')
    names = re.findall(r'libCoreEntitlements\s*->\s*(\w+)', text)
    log += '\npreprocessed_output_sha256=' + hashlib.sha256(data).hexdigest()
    log += '\nmember_counts=' + json.dumps({n: names.count(n) for n in sorted(set(names))})
    log += '\nRaw output retained only in runner temporary storage; it includes SDK/KDK header text.\n'
else:
    log += '\nPreprocessing failed; activation remains unknown.\n'
(evidence / f'{component}-preprocess.log').write_text(log)
print(f'{component}: preprocessing exit {result.returncode}')
