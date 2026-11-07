#!/usr/bin/env python3
"""Attempt pmap preprocessing from a saved Candidate command; fail visibly.

Paths are remapped into the isolated runner, and the verified KDK header root
is added. No missing generated headers are synthesized. The preprocessed file
is temporary and never uploaded; only the command, include trace and a compact
identifier summary are retained.
"""
import json
import pathlib
import re
import shlex
import subprocess
import sys

xnu, headers, work, evidence, sdk, clang, record = map(pathlib.Path, sys.argv[1:])
data = json.loads(record.read_text().rstrip().rstrip(','))
original = data['arguments']
old_root = '/Users/runner/work/_temp/candidate-a-output/nezha/ipados-26.5-23F77-nezha/nezha/tree'
original_sdk = '/Applications/Xcode_26.5.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'

def remap(value):
    if value.startswith(old_root + '/.cache/xnu-work'):
        return str(xnu) + value[len(old_root + '/.cache/xnu-work'):]
    if value.startswith(old_root + '/.cache/fakeroot'):
        return str(work / 'frozen/.cache/fakeroot') + value[len(old_root + '/.cache/fakeroot'):]
    if value.startswith(old_root):
        return str(work / 'missing-candidate-tree') + value[len(old_root):]
    if value.startswith(original_sdk):
        return str(sdk) + value[len(original_sdk):]
    return value

command = [str(clang), '-I', str(headers), '-I', str(work / 'patched'),
           '-I', str(headers / 'platform')]
skip = 0
for index, arg in enumerate(original[1:], 1):
    if skip:
        skip -= 1
        continue
    if arg in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
        skip = 1
        continue
    if arg in ('-c', '-MD', '-MMD', '-MP'):
        continue
    command.append(remap(arg))
output = work / 'pmap-preprocessed.i'
command.extend(['-E', '-H', '-o', str(output)])
result = subprocess.run(command, cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
log = 'cwd: ' + str(work) + '\ncommand: ' + shlex.join(command) + '\nexit: ' + str(result.returncode) + '\n' + result.stdout
if result.returncode == 0:
    text = output.read_text(errors='replace')
    names = re.findall(r'libCoreEntitlements\s*->\s*(\w+)', text)
    log += '\nPREPROCESSED_MEMBER_COUNTS: ' + json.dumps({n: names.count(n) for n in sorted(set(names))}) + '\n'
else:
    log += '\nPREPROCESS_INCOMPLETE: no active member conclusion\n'
(evidence / 'pmap-real-record-preprocess.log').write_text(log)
print('pmap preprocessing exit:', result.returncode, flush=True)
