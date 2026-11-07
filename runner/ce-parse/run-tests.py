#!/usr/bin/env python3
"""Parse KDK headers with Candidate-derived flags; persist every outcome."""
import json
import pathlib
import re
import shlex
import subprocess
import sys


xnu, headers, work, evidence, sdk, clang, record = map(pathlib.Path, sys.argv[1:])
data = json.loads(record.read_text().rstrip().rstrip(','))
original = data['arguments']
flags = []
for i, arg in enumerate(original):
    if arg in ('-D', '-U') and i + 1 < len(original):
        flags.extend([arg, original[i + 1]])
    elif arg in ('-mkernel', '-nostdlibinc', '-fno-builtin', '-fno-common', '-fblocks'):
        flags.append(arg)
    elif arg.startswith('--target=') or arg.startswith('-mios-version-min='):
        flags.append(arg)
if '--target=arm64e-apple-ios26.0.0' not in flags or '-mkernel' not in flags:
    raise SystemExit('Candidate record lacks expected target/mkernel')

kernel = (headers / 'CoreEntitlements/V2/Kernel.h').read_text()
members = re.findall(r'typeof\((CE\w+)\)\s*\*(\w+)\s*;', kernel)
if len(members) != 45:
    raise SystemExit(f'unexpected member count: {len(members)}')

include = ['-I', str(headers), '-I', str(xnu / 'EXTERNAL_HEADERS'),
           '-I', str(xnu / 'libkern'), '-I', str(xnu / 'osfmk'),
           '-I', str(xnu / 'bsd'), '-I', str(xnu / 'iokit'),
           '-I', str(xnu / 'pexpert'), '-isysroot', str(sdk)]
common = [str(clang), '-x', 'c', '-std=gnu11', *flags, *include]
prefix = '#include <CoreEntitlements/V2/API.h>\n#include <CoreEntitlements/V2/Kernel.h>\n'
expr = '''
void candidate_members(const CEKernelAPI_t *a, const CEContext_t *ctx,
                       CEContext_t *mutable_ctx, CEElement_t *element,
                       CEBuffer_t *buffer, const struct CERuntime *runtime,
                       const uint8_t *data, size_t n) {
    const void *legacy = 0; size_t length = 0;
    (void)a->contextGetLegacyContext(ctx, &legacy);
    (void)a->contextValueForKey(ctx, "key", element);
    (void)a->elementMatchBool(element, 1);
    (void)a->elementGetCEBuffer(element, buffer);
    (void)a->contextInitWithTypeLegacy(runtime, kCEContextTypeProvisioningProfile,
                                      mutable_ctx, data, n);
    (void)a->contextCheckAcceleration(ctx);
    (void)a->contextEvaluateAcceleration(ctx, &length);
    (void)a->contextAccelerate(mutable_ctx);
}
'''
layout = prefix + '\n' + '\n'.join(
    f'unsigned long ce_offset_{name} = __builtin_offsetof(CEKernelAPI_t, {name});'
    for _, name in members) + '\nunsigned long ce_size = sizeof(CEKernelAPI_t);\n'
tests = {
    'api-alone': ('#include <CoreEntitlements/V2/API.h>\n', ['-fsyntax-only']),
    'kernel-alone': ('#include <CoreEntitlements/V2/Kernel.h>\n', ['-fsyntax-only']),
    'candidate-order': (prefix + '#include <CoreEntitlements/V2/API.h>\n#include <CoreEntitlements/V2/Kernel.h>\n', ['-fsyntax-only', '-H']),
    'candidate-amfi': ('#include <libkern/amfi/amfi.h>\n', ['-fsyntax-only', '-H']),
    'member-access': (prefix + expr, ['-fsyntax-only']),
    'ast': (prefix + expr, ['-fsyntax-only', '-Xclang', '-ast-dump']),
    'layout-ir': (layout, ['-S', '-emit-llvm', '-o', '-']),
    'record-layout': (prefix + 'CEKernelAPI_t ce_layout_instance;\n', ['-fsyntax-only', '-Xclang', '-fdump-record-layouts-complete']),
}
statuses = {}
for name, (source, options) in tests.items():
    path = work / f'{name}.c'
    path.write_text(source)
    command = [*common, *options, str(path)]
    result = subprocess.run(command, cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (evidence / f'{name}.log').write_text('cwd: ' + str(work) + '\ncommand: ' + shlex.join(command) +
                                          '\nexit: ' + str(result.returncode) + '\n' + result.stdout)
    statuses[name] = result.returncode
    print(name, 'exit', result.returncode, flush=True)
(evidence / 'status.json').write_text(json.dumps({'statuses': statuses, 'candidate_record': str(record),
                                                  'sdk': str(sdk), 'clang': str(clang),
                                                  'flags': flags, 'include_paths': include}, indent=2))
