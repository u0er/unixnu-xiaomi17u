#!/usr/bin/env python3
"""Inspect the runner SDK and Clang driver without compiling an object."""
import json
import os
from pathlib import Path
import plistlib
import shlex
import subprocess
import sys

DEVELOPER = Path('/Applications/Xcode_26.5.app/Contents/Developer')
OUT = Path(os.environ['RUNNER_TEMP']) / 'candidate-a-sdk-binding'
OUT.mkdir(parents=True, exist_ok=True)
report = {'status': 'FAIL', 'checks': {}, 'commands': [],
          'scope': 'SDK/Clang driver probe only; no XNU make or object compilation',
          'xnu_make_sdk_binding': 'NOT_CHECKED', 'errors': []}


def run(label, argv, stdin=None):
    result = subprocess.run(argv, input=stdin, text=True, capture_output=True, check=False)
    (OUT / (label + '.stdout')).write_text(result.stdout)
    (OUT / (label + '.stderr')).write_text(result.stderr)
    report['commands'].append({'label': label, 'argv': argv, 'cwd': os.getcwd(), 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError(f'{label} exited {result.returncode}; see diagnostics')
    return result


try:
    if os.environ.get('DEVELOPER_DIR') != str(DEVELOPER):
        raise RuntimeError('unexpected DEVELOPER_DIR')
    xcode = run('xcode', ['/usr/bin/xcodebuild', '-version']).stdout.strip()
    if xcode.splitlines() != ['Xcode 26.5', 'Build version 17F42']:
        raise RuntimeError('unexpected Xcode version')
    sdk = Path(run('sdk-path', ['/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).stdout.strip())
    expected = DEVELOPER / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS26.5.sdk'
    if not sdk.is_dir() or not expected.is_dir() or sdk.resolve() != expected.resolve():
        raise RuntimeError('SDK path differs from selected Xcode')
    version = run('sdk-version', ['/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-version']).stdout.strip()
    build = run('sdk-build', ['/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version']).stdout.strip()
    with (sdk / 'SDKSettings.plist').open('rb') as stream:
        settings = plistlib.load(stream)
    if version != '26.5' or build != '23F73' or settings.get('CanonicalName') != 'iphoneos26.5' or settings.get('Version') != version:
        raise RuntimeError('SDK version/build/plist mismatch')
    clang = Path(run('clang-path', ['/usr/bin/xcrun', '--sdk', 'iphoneos', '--find', 'clang']).stdout.strip())
    if not clang.is_file() or not clang.resolve().is_relative_to(DEVELOPER.resolve()):
        raise RuntimeError('clang is not in selected Xcode')
    clang_version = run('clang-version', [str(clang), '--version']).stdout.strip()
    if not clang_version.startswith('Apple clang version 21.0.0 '):
        raise RuntimeError('unexpected clang version')
    args = [str(clang), '-target', 'arm64e-apple-ios26.0.0', '-mkernel', '-isysroot', str(sdk)]
    driver = run('clang-driver', [*args, '-###', '-x', 'c', '-c', '-'], '').stderr
    commands = []
    for line in driver.splitlines():
        try:
            commands.append(shlex.split(line))
        except ValueError:
            continue
    cc1 = [parts for parts in commands if '-cc1' in parts]
    if len(cc1) != 1 or '-triple' not in cc1[0] or '-isysroot' not in cc1[0]:
        raise RuntimeError('could not identify a unique cc1 triple/sysroot')
    parts = cc1[0]
    triple, actual_sysroot = parts[parts.index('-triple') + 1], parts[parts.index('-isysroot') + 1]
    if not triple.startswith('arm64e-apple-ios26.0') or Path(actual_sysroot).resolve() != sdk.resolve():
        raise RuntimeError('cc1 triple or sysroot mismatch')
    includes = run('clang-includes', [*args, '-E', '-v', '-x', 'c', '-'], '').stderr
    start = '#include <...> search starts here:'
    if start not in includes or 'End of search list.' not in includes:
        raise RuntimeError('clang did not report its include search list')
    paths = [line.strip().removesuffix(' (framework directory)') for line in
             includes.split(start, 1)[1].split('End of search list.', 1)[0].splitlines() if line.strip()]
    forbidden = [path for path in paths if path.startswith(('/mnt/', '/usr/include', '/usr/local/include', '/opt/'))]
    if forbidden or not any(Path(path).resolve().is_relative_to(sdk.resolve()) for path in paths):
        raise RuntimeError('SDK search path missing or unexpected host include path')
    report.update(status='PASS_DRIVER_ONLY', sdk_path=str(sdk), sdk_resolved=str(sdk.resolve()),
                  sdk_version=version, sdk_build=build, clang=str(clang), clang_version=clang_version,
                  cc1_triple=triple, cc1_sysroot=actual_sysroot, include_search_paths=paths)
except (OSError, RuntimeError, ValueError, plistlib.InvalidFileException) as error:
    report['errors'].append(str(error))
finally:
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
sys.exit(0 if report['status'] == 'PASS_DRIVER_ONLY' else 1)
