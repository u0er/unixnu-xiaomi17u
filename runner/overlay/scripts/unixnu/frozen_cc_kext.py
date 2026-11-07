"""Validate Candidate A's frozen compiler-rt kernel support archive."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from . import frozen_firehose, sdk_metadata
from .util import Error

PIN = '796625112a6eb7517023a72ea95573c8bf7ce0ef60d62650cade0930a055098b'
LLVM = '58c46bae21181fb4240cf964461bea738d96d50e'
LIBC = '899a3b2d52d95d75e05fb286a5e64975ec3de757'
NAMES = ('InstrProfiling.o', 'InstrProfilingBuffer.o', 'InstrProfilingPlatformDarwin.o',
         'InstrProfilingWriter.o', 'InstrProfilingInternal.o', 'InstrProfilingVersionVar.o')


def validate_archive(data):
    if hashlib.sha256(data).hexdigest() != PIN or not data.startswith(b'!<arch>\n'):
        raise Error('cc_kext archive SHA-256 mismatch')
    offset, objects = 8, []
    try:
        while offset < len(data):
            header = data[offset:offset + 60]
            if len(header) != 60 or header[58:60] != b'`\n':
                raise Error('malformed cc_kext archive')
            size = int(header[48:58].strip())
            body = data[offset + 60:offset + 60 + size]
            if len(body) != size:
                raise Error('truncated cc_kext archive')
            name = header[:16].decode('ascii').strip()
            if name.startswith('#1/'):
                length = int(name[3:])
                name, body = body[:length].rstrip(b'\0').decode('ascii'), body[length:]
            else:
                name = name.rstrip('/')
            if not name.startswith('__.SYMDEF'):
                objects.append(name)
                if len(body) < 32 or struct.unpack_from('<4I', body) != (0xfeedfacf, 0x100000c, 0xc0000002, 1):
                    raise Error(f'{name}: expected ARM64E KER00 MH_OBJECT')
                ncmds, command_bytes = struct.unpack_from('<II', body, 16)
                pos, versions = 32, []
                for _ in range(ncmds):
                    cmd, length = struct.unpack_from('<II', body, pos)
                    if length < 8 or pos + length > min(len(body), 32 + command_bytes):
                        raise Error(f'{name}: invalid Mach-O command')
                    if cmd == 0x32:
                        if length < 24:
                            raise Error(f'{name}: invalid build version')
                        versions.append(struct.unpack_from('<III', body, pos + 8))
                    pos += length
                if pos != 32 + command_bytes or versions != [(2, 26 << 16, 0)]:
                    raise Error(f'{name}: expected iOS 26.0 and unspecified SDK')
            offset += 60 + size + (size & 1)
    except (ValueError, UnicodeError, struct.error) as error:
        raise Error(f'malformed cc_kext archive: {error}') from error
    if offset != len(data) or tuple(objects) != NAMES:
        raise Error('unexpected cc_kext archive members')


def preflight(ctx, *, sdk_version=None):
    spec = ctx.board.get('kernel', {}).get('prebuilt_cc_kext')
    board = ctx.board.get('board', {})
    if spec is None:
        if board.get('name') == 'nezha' and board.get('os_profile') == 'ipados-26.5-23F77-nezha':
            raise Error('Nezha 23F77 requires [kernel.prebuilt_cc_kext]')
        return None
    if not isinstance(spec, dict) or set(spec) != {'archive', 'manifest', 'sha256'} or spec['sha256'] != PIN:
        raise Error('invalid frozen cc_kext configuration')
    profile = ctx.board.get('os', {})
    if profile.get('arch') != 'arm64e' or profile.get('ios_version') != '26.0':
        raise Error('cc_kext target profile mismatch')
    configured_sdk = profile.get('sdk_version')
    if sdk_version and configured_sdk and sdk_version != configured_sdk:
        raise Error('cc_kext SDK version conflicts with the selected profile')
    if not isinstance(sdk_version or configured_sdk, str) or not (sdk_version or configured_sdk):
        raise Error('cc_kext requires a resolved os.sdk_version; candidate fakeroot SDK version is UNKNOWN')
    paths = {key: (ctx.device / spec[key]).resolve() for key in ('archive', 'manifest')}
    try:
        candidate = json.loads(paths['manifest'].read_text())['cc_kext_kernel_abi_candidate']
        expected = {'sha256': PIN, 'llvm_commit': LLVM, 'libc_commit': LIBC,
                    'xnu_commit': frozen_firehose.XNU, 'target': 'arm64e-apple-ios26.0',
                    'sdk_root': 'Candidate A generated fakeroot; Mach-O LC_BUILD_VERSION sdk n/a'}
        if any(candidate.get(key) != value for key, value in expected.items()):
            raise Error('cc_kext provenance manifest mismatch')
        validate_archive(paths['archive'].read_bytes())
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise Error(f'cc_kext inputs unavailable: {error}') from error
    return paths


def prepare(ctx, env, headers, *, apple_sdk=None):
    sdk_version = apple_sdk['sdk_version'] if apple_sdk else None
    paths = preflight(ctx, sdk_version=sdk_version)
    if paths is None:
        return None
    if env.get('UNIXNU_XNU_ARCH') != 'arm64e' or env.get('UNIXNU_IOS_VERSION') != '26.0':
        raise Error('cc_kext build target mismatch')
    expected_sdk = sdk_version or ctx.board['os']['sdk_version']
    if env.get('UNIXNU_SDK_VERSION') != expected_sdk:
        raise Error('cc_kext resolved SDK version does not match the selected build SDK')
    tree = ctx.tree.resolve()
    if apple_sdk:
        from . import xnu_only_sdk
        inspected = xnu_only_sdk.inspect(tree / '.cache/fakeroot')
        if inspected['sdkroot'] != apple_sdk['sdkroot'] or inspected['clang'] != apple_sdk['clang']:
            raise Error('cc_kext Apple SDK or compiler changed after preflight')
    else:
        sdk_metadata.verify_iphoneos_sdk(tree / '.cache/fakeroot', expected_sdk)
    for name, expected in (('llvm-project', LLVM), ('Libc', LIBC)):
        source_root = Path(env['UNIXNU_CC_KEXT_SOURCE_ROOT']) if apple_sdk and env.get('UNIXNU_CC_KEXT_SOURCE_ROOT') else tree / '.cache/linux'
        repo = source_root / name
        try:
            head = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            dirty = subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain', '--untracked-files=no'], text=True)
        except (OSError, subprocess.CalledProcessError) as error:
            raise Error(f'cc_kext source {name} unavailable: {error}') from error
        if head != expected or dirty:
            raise Error(f'cc_kext source {name} commit or tracked files mismatch')
    frozen_firehose.verify_bundle(headers)
    frozen_firehose.verify_headers(headers, tree)
    data = paths['archive'].read_bytes()
    validate_archive(data)
    target = tree / '.cache/fakeroot/usr/local/lib/kernel/libcc_kext.a'
    frozen_firehose.install_exact(target, data)
    validate_archive(target.read_bytes())
    return target
