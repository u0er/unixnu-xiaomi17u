#!/usr/bin/env python3
"""Compile a BT driver object against an extracted matching XNU CI buildtree.

Run on Linux/WSL. Preserve archived BSD defines and optimization settings,
translate CI paths, and replace the Apple-only architecture/deployment driver
options with an explicit arm64 Apple target. No generated-header or ABI stubs
are introduced. A successful compile is not a kernel link or device result.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--buildtree', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--sdk-include', type=Path, required=True,
                        help='real SDK usr/include directory for standard C headers')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    bt, source, compiler, sdk, out = (p.resolve() for p in
                                     (args.buildtree, args.source, args.clang,
                                      args.sdk_include, args.out))
    cwd = bt / 'build/xnu.obj/DEVELOPMENT_ARM64_PANDORA/bsd/DEVELOPMENT'
    raw_flags = shlex.split((cwd / '.CFLAGS').read_text().strip('\0\r\n '))[1:]
    # XNU's flags cache removes shell quotes around expression-valued
    # defines. Rejoin only balanced parenthesized -D values, never execute it.
    flags = []
    pending = ''
    for flag in raw_flags:
        if pending:
            pending += ' ' + flag
        elif flag.startswith('-D') and flag.count('(') > flag.count(')'):
            pending = flag
        else:
            flags.append(flag)
        if pending and pending.count('(') == pending.count(')'):
            flags.append(pending)
            pending = ''
    if pending:
        raise ValueError('unbalanced macro expression in CFLAGS')
    versions = [f.split('=', 1)[1] for f in flags
                if f.startswith('-mmacosx-version-min=')]
    if len(versions) != 1:
        raise ValueError('expected one deployment version in archived CFLAGS')
    command = [str(compiler), '--target=arm64-apple-macos' + versions[0]]
    compiler_version = subprocess.run([str(compiler), '--version'], check=True,
                                      capture_output=True, text=True).stdout
    if re.search(r'clang version 14\.', compiler_version):
        command.append('-enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang')
    skip = False
    for flag in flags:
        if skip:
            skip = False
            continue
        if flag == '-arch':
            skip = True
            continue
        if flag.startswith('-mmacosx-version-min='):
            continue
        flag = re.sub(r'/Users/runner/work/[^/ ]+/[^/ ]+/dxb', str(bt), flag)
        if flag.startswith('-I/Applications/'):
            platform = bt / 'sdk-kernel-platform/platform'
            if platform.is_dir():
                command.append('-I' + str(platform))
            # CI sometimes archives an empty SDK-platform directory. This
            # driver does not use those headers; keep missing C headers fatal.
            continue
        command.append(flag)
    command += ['-I' + str(sdk), '-include',
                str(bt / 'build/xnu.obj/EXPORT_HDRS/osfmk/string.h'),
                '-Wall', '-Wextra', '-Wno-unknown-warning-option',
                '-c', str(source), '-o', str(out)]
    out.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    log = out.with_name(out.name + '.compile.log')
    log.write_text(result.stdout + result.stderr)
    record = dict(buildtree=str(bt), source=str(source), cwd=str(cwd),
                  command=command, exit_code=result.returncode,
                  elapsed_seconds=round(time.monotonic() - started, 3), log=str(log))
    out.with_name(out.name + '.compile.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'Compile exit {result.returncode}; diagnostics: {log}', flush=True)
    if result.returncode:
        print((result.stdout + result.stderr)[-12000:])
    result.check_returncode()


if __name__ == '__main__':
    main()
