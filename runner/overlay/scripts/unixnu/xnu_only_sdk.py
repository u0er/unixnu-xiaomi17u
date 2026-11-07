"""Identify the Apple SDK and kernel overlay for an XNU-only experiment.

This preflight does not prove that XNU make actually uses either path.
"""
import os
import json
from pathlib import Path
import platform
import subprocess

from . import sdk_metadata
from .util import Error


DEVELOPER = Path('/Applications/Xcode_26.5.app/Contents/Developer')


def inspect_sdk():
    """Identify the runner-local Apple toolchain before any overlay is created."""
    if platform.system() != 'Darwin':
        raise Error('XNU-only requires a macOS runner with Xcode 26.5')
    if os.environ.get('DEVELOPER_DIR') != str(DEVELOPER):
        raise Error('XNU-only requires the selected Xcode 26.5 DEVELOPER_DIR')
    env = dict(os.environ, DEVELOPER_DIR=str(DEVELOPER))

    def query(*args):
        try:
            return subprocess.run(args, check=True, capture_output=True, text=True,
                                  env=env).stdout.strip()
        except (OSError, subprocess.CalledProcessError) as error:
            raise Error(f'XNU-only SDK tool failed: {args[0]}: {error}') from error

    if query('/usr/bin/xcodebuild', '-version').splitlines() != ['Xcode 26.5', 'Build version 17F42']:
        raise Error('XNU-only requires Xcode 26.5 build 17F42')
    sdk = Path(query('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-path'))
    expected = DEVELOPER / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS26.5.sdk'
    if not sdk.is_dir() or not expected.is_dir() or sdk.resolve() != expected.resolve():
        raise Error('XNU-only SDK path does not match selected Xcode')
    if query('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-version') != '26.5':
        raise Error('XNU-only requires iPhoneOS SDK 26.5')
    if query('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version') != '23F73':
        raise Error('XNU-only requires iPhoneOS SDK build 23F73')
    if not (sdk / 'SDKSettings.plist').is_file():
        raise Error('XNU-only requires the real SDKSettings.plist')
    sdk_metadata.verify_iphoneos_sdk(sdk, '26.5')
    for relative in ('usr/include/stdio.h', 'usr/include/TargetConditionals.h', 'usr/lib/libSystem.tbd'):
        if not (sdk / relative).is_file():
            raise Error(f'XNU-only SDK input missing: {relative}')
    clang = Path(query('/usr/bin/xcrun', '--sdk', 'iphoneos', '--find', 'clang'))
    if not clang.is_file() or not clang.resolve().is_relative_to(DEVELOPER.resolve()):
        raise Error('XNU-only clang is not from the selected Xcode')
    clang_version = query(str(clang), '--version')
    if not clang_version.startswith('Apple clang version 21.0.0 '):
        raise Error('XNU-only requires Apple clang 21.0.0')
    resolved_sdk = sdk.resolve()
    return {'sdkroot': str(resolved_sdk), 'sdk_version': '26.5', 'sdk_build': '23F73',
            'clang': str(clang.resolve()), 'clang_version': clang_version,
            'overlay_provenance': 'NOT_CHECKED', 'make_binding': 'NOT_CHECKED'}


def inspect(overlay):
    """Also check the distinct kernel header overlay after header installation."""
    result = inspect_sdk()
    overlay = Path(overlay).resolve()
    resolved_sdk = Path(result['sdkroot'])
    if (overlay == resolved_sdk or overlay.is_relative_to(resolved_sdk)
            or resolved_sdk.is_relative_to(overlay)):
        raise Error('XNU-only kernel overlay must be separate from the Apple SDK')
    if not overlay.is_dir() or not (overlay / 'usr/local/include/kernel').is_dir():
        raise Error('XNU-only kernel header overlay is missing')
    if not (overlay / '.xnu-12377.121.6-headers').is_file():
        raise Error('XNU-only kernel header installation marker is missing')
    result['kernel_sdkroot'] = str(overlay)
    return result


def verify_make_record(path, sdk, overlay):
    """Validate the SDK binding in XNU's actual -MJ compile record."""
    try:
        record = json.loads(Path(path).read_text().strip().rstrip(','))
        args = record['arguments']
        cwd = Path(record['directory'])
        if not isinstance(args, list) or not args or not all(isinstance(a, str) for a in args):
            raise ValueError('invalid argument vector')
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise Error(f'XNU-only compile record unavailable or invalid: {path}: {error}') from error

    def values(option):
        indices = [i for i, value in enumerate(args) if value == option]
        if any(i + 1 == len(args) for i in indices):
            raise Error(f'XNU-only compile record has an incomplete {option}')
        return [args[i + 1] for i in indices]

    def resolved(value):
        value = Path(value)
        return (value if value.is_absolute() else cwd / value).resolve()

    if resolved(args[0]) != Path(sdk['clang']).resolve():
        raise Error('XNU-only compile record uses an unexpected clang')
    if [resolved(v) for v in values('-isysroot')] != [Path(sdk['sdkroot']).resolve()]:
        raise Error('XNU-only compile record does not bind the verified SDK')
    if any(a.startswith('--sysroot') or a.startswith('-isysroot=') for a in args):
        raise Error('XNU-only compile record contains an additional sysroot')
    targets = values('-target') + values('--target') + [a.split('=', 1)[1] for a in args if a.startswith('--target=')]
    if targets and any(t not in ('arm64e-apple-ios26.0', 'arm64e-apple-ios26.0.0') for t in targets):
        raise Error('XNU-only compile record has a conflicting target triple')
    arch = values('-arch')
    deployment = [a for a in args if a.startswith('-miphoneos-version-min=')]
    # Clang's -MJ normalizes the deployment option into --target, so either
    # representation must prove iOS 26.0; reject any conflicting declaration.
    if deployment not in ([], ['-miphoneos-version-min=26.0']):
        raise Error('XNU-only compile record has a conflicting deployment target')
    if (arch != ['arm64e'] or (not targets and not deployment)
            or '-mkernel' not in args):
        raise Error('XNU-only compile record does not use the expected arm64e/iOS kernel flags')
    includes = values('-I') + [a[2:] for a in args if a.startswith('-I') and len(a) > 2]
    required = (Path(overlay) / 'usr/local/include/kernel').resolve()
    if required not in [resolved(v) for v in includes]:
        raise Error('XNU-only compile record does not include the kernel header overlay')
    return {'record': str(Path(path)), 'clang': str(resolved(args[0])),
            'sdkroot': sdk['sdkroot'], 'kernel_sdkroot': str(Path(overlay).resolve()),
            'target': 'arm64e-apple-ios26.0', 'make_binding': 'RECORD_VERIFIED'}
