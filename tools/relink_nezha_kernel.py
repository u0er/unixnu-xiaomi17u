#!/usr/bin/env python3
"""Relink an extracted Nezha CI build using the matching objects and ld64.

--replace NAME=OBJECT replaces exactly one listed object, leaving all other
objects from --ci intact. --objects retains the original run-62 driver-bundle
mode. The caller must supply buildtree and objects from the same CI run.
This tool performs host linking only; it never accesses a device.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

CONFIG = 'DEVELOPMENT_ARM64_PANDORA'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--buildtree', type=Path, required=True)
    p.add_argument('--ci', type=Path, required=True)
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--objects', type=Path,
                      help='legacy run-62 replacement driver directory')
    mode.add_argument('--replace', action='append', metavar='NAME=OBJECT',
                      help='replace one uniquely named filelist object; repeatable')
    p.add_argument('--ld', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    bt, ci, out = (x.resolve() for x in (a.buildtree, a.ci, a.out))
    out.parent.mkdir(parents=True, exist_ok=True)
    config = bt / 'build/xnu.obj' / CONFIG
    if a.replace:
        replacements = {}
        for value in a.replace:
            name, separator, path = value.partition('=')
            if not separator or not re.fullmatch(r'[A-Za-z_]\w*', name):
                p.error('--replace requires an object stem and path: NAME=OBJECT')
            if name in replacements:
                p.error(f'duplicate replacement: {name}')
            obj = Path(path).resolve()
            if not obj.is_file():
                p.error(f'replacement object does not exist: {obj}')
            replacements[name] = obj
        wanted, additions = set(replacements), set()
    else:
        replacements = {f.name.split('__')[-1].split('.')[0]: f
                        for f in a.objects.resolve().glob('*.o')}
        wanted, additions = legacy_objects(replacements)
    files, replaced = [], set()
    for original in (config / 'link.filelist').read_text().splitlines():
        stem = Path(original).stem
        if stem in replacements:
            if stem in replaced:
                raise ValueError(f'object stem is not unique in filelist: {stem}')
            file = replacements[stem]
            replaced.add(stem)
        else:
            file = ci / 'obj' / original.split('/build/xnu.obj/')[1]
        if not file.is_file():
            raise FileNotFoundError(file)
        files.append(file)
    if replaced != wanted:
        raise ValueError(f'replacements missing from filelist: {wanted - replaced}')
    files.extend(replacements[name] for name in sorted(additions))
    link(bt, ci, config, files, replaced, additions, a.ld, out)


def legacy_objects(replacements):
    """Preserve the original run-62 bundle's exact replacement contract."""
    additions = {'PeachMhiFirmware', 'PeachMhiHost', 'PeachWlfw',
                 'PeachCe', 'PeachHtc', 'PeachWmi', 'PeachHtt', 'PeachSrng', 'PeachRx', 'PeachTx', 'PeachStation', 'PeachRsn', 'PeachNet', 'PeachApple80211', 'PeachWLAN', 'pandora_wifi', 'pandora_amp_tas'}
    wanted = {'pandora_gpu', 'pandora_touch', 'pandora_adsp', 'pandora_files', 'pandora_dpu', 'pandora_cpu', 'pandora_prof', 'kern_exec', 'OSKext', 'SweetFramebuffer', 'SweetPlatformExpert', 'sweet_ufs'}
    if 'pandora_amp' in replacements:
        wanted.add('pandora_amp')
    if 'SweetSurface' in replacements:
        wanted.add('SweetSurface')
    if 'kern_exit' in replacements:
        wanted.add('kern_exit')
    if replacements.keys() != wanted | additions:
        raise ValueError(f'expected exactly the Nezha driver objects, got {list(replacements)}')
    return wanted, additions


def link(bt, ci, config, files, replaced, additions, linker, out):
    """Reuse archived kernel flags, exports and four final link-only objects."""
    filelist = out.with_name(out.name + '.filelist')
    filelist.write_text(''.join(f'{f.as_posix()}\n' for f in files))
    text = (config / '.LDFLAGS').read_text().strip('\0\r\n ')
    text = re.sub(r'/Users/runner/work/[^/ ]+/[^/ ]+/dxb', bt.as_posix(), text)
    flags = shlex.split(text)[1:]
    versions = {flag.split('=', 1)[1] for flag in flags
                if flag.startswith('-mmacosx-version-min=')}
    if len(versions) != 1:
        raise ValueError('archived linker flags must specify one macOS deployment version')
    version = versions.pop()
    args = [str(linker.resolve()), '-arch', 'arm64']
    for flag in flags:
        if flag.startswith('-Wl,'):
            args.extend(flag[4:].split(','))
        elif flag.startswith('-l'):
            args.append(flag)
    # Windows: ld64.lld cannot open /dev/null for empty -sectcreate; use a real empty file.
    nullfile = out.with_name(out.name + '.null')
    nullfile.write_bytes(b'')
    for i, a in enumerate(args):
        if a == '/dev/null':
            args[i] = str(nullfile)
    cc = ci / 'linklibs/libcc_kext.a'
    if not cc.exists():
        shutil.copyfile(ci / 'linklibs/libclang_rt.cc_kext.a', cc)
    args += ['-L', str(ci / 'linklibs'), '-platform_version', 'macos', version, version,
             '-filelist', str(filelist)]
    args += [str(ci / 'obj' / CONFIG / n) for n in
             ('version.o', 'lastkerneldataconst.o', 'lastkernelconstructor.o', 'nonlto.o')]
    args += ['-o', str(out)]
    print(f'Linking {len(files)} objects: {len(replaced)} Nezha replacements, {len(additions)} additions', flush=True)
    result = subprocess.run(args, capture_output=True, text=True)
    log = out.with_name(out.name + '.link.log')
    log.write_text(result.stdout + result.stderr)
    record = out.with_name(out.name + '.link.json')
    record.write_text(json.dumps(dict(buildtree=str(bt), ci=str(ci),
                                     replaced=sorted(replaced), additions=sorted(additions),
                                     deployment_version=version, command=args,
                                     exit_code=result.returncode, log=str(log)), indent=2) + '\n')
    print(f'Link exit {result.returncode}; diagnostics: {log}', flush=True)
    if result.returncode:
        print((result.stdout + result.stderr)[-8000:])
    result.check_returncode()


if __name__ == '__main__':
    main()
