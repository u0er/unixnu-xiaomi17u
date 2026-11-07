#!/usr/bin/env python3
"""Report the installed Xcode SDK without copying SDK contents or building."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys

DEVELOPER = Path('/Applications/Xcode_26.5.app/Contents/Developer')
EXPECTED_SDK = DEVELOPER / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS26.5.sdk'
REQUIRED = ('SDKSettings.plist', 'usr/include/stdio.h', 'usr/lib/libSystem.tbd',
            'System/Library/Frameworks/Foundation.framework/Headers/Foundation.h')


def command(*args):
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(f'{args[0]} exited {result.returncode}: {result.stderr.strip()}')
    return result.stdout.strip()


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def inventory(sdk, destination):
    """Record SDK entries without following directory links or copying content."""
    files = links = 0
    broken, external, unreadable, modulemaps = [], [], [], []
    with destination.open('w', encoding='utf-8') as output:
        def onerror(error):
            unreadable.append(str(error))

        for directory, dirs, names in os.walk(sdk, followlinks=False, onerror=onerror):
            parent = Path(directory)
            dirs.sort()
            names.sort()
            for name in list(dirs):
                child = parent / name
                if child.is_symlink():
                    names.append(name)
                    dirs.remove(name)
            for name in sorted(names):
                path = parent / name
                relative = path.relative_to(sdk).as_posix()
                try:
                    if path.is_symlink():
                        target = os.readlink(path)
                        # Some SDK links use an absolute target rooted at the SDK.
                        rooted = sdk / target.lstrip('/') if target.startswith('/') else path.parent / target
                        actual = path.exists()
                        sdk_rooted = rooted.exists()
                        resolved = rooted.resolve() if sdk_rooted else path.resolve()
                        contained = (actual or sdk_rooted) and resolved.is_relative_to(sdk.resolve())
                        entry = {'type': 'symlink', 'path': relative, 'target': target,
                                 'host_resolves': actual, 'sdk_rooted_resolves': sdk_rooted,
                                 'resolved_path': str(resolved), 'inside_sdk': contained}
                        links += 1
                        if not actual and not sdk_rooted:
                            broken.append(relative)
                        elif not contained:
                            external.append(relative)
                    elif path.is_file():
                        entry = {'type': 'file', 'path': relative, 'size': path.stat().st_size,
                                 'sha256': digest(path)}
                        files += 1
                        if name.endswith('.modulemap') or name == 'module.modulemap':
                            modulemaps.append(relative)
                    else:
                        unreadable.append(f'unsupported SDK entry: {relative}')
                        continue
                    output.write(json.dumps(entry, sort_keys=True) + '\n')
                except (OSError, RuntimeError) as error:
                    unreadable.append(f'{relative}: {error}')
    return {'files': files, 'symlinks': links, 'broken_symlinks': broken,
            'external_symlinks': external,
            'unreadable_entries': unreadable, 'modulemap_count': len(modulemaps),
            'modulemap_examples': modulemaps[:10]}


def probe(output):
    report = {'status': 'FAIL', 'checks': {}, 'errors': [],
              'runner': {name: os.environ.get(name) for name in
                         ('RUNNER_OS', 'RUNNER_ARCH', 'ImageOS', 'ImageVersion',
                          'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_SHA')},
              'developer_dir': os.environ.get('DEVELOPER_DIR'),
              'scope': 'Environment probe only; no SDK contents, kernel, archive or device operation.'}
    output.mkdir(parents=True, exist_ok=True)
    try:
        checks = report['checks']
        checks['developer_dir'] = Path(report['developer_dir'] or '') == DEVELOPER and DEVELOPER.is_dir()
        xcode = command('xcodebuild', '-version')
        report['xcodebuild_version'] = xcode
        checks['xcode_version_build'] = xcode.splitlines() == ['Xcode 26.5', 'Build version 17F42']
        sdk_path = Path(command('xcrun', '--sdk', 'iphoneos', '--show-sdk-path'))
        report['sdk_path'] = str(sdk_path)
        report['sdk_resolved_path'] = str(sdk_path.resolve())
        report['sdk_version'] = command('xcrun', '--sdk', 'iphoneos', '--show-sdk-version')
        report['sdk_build_version'] = command('xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version')
        checks['sdk_path'] = sdk_path.is_dir() and sdk_path.resolve() == EXPECTED_SDK.resolve() and EXPECTED_SDK.is_dir()
        checks['sdk_version'] = report['sdk_version'] == '26.5'
        clang = Path(command('xcrun', '--sdk', 'iphoneos', '--find', 'clang'))
        report['clang_path'] = str(clang)
        report['clang_resolved_path'] = str(clang.resolve())
        report['clang_version'] = command(str(clang), '--version')
        checks['apple_clang'] = (clang.is_file() and clang.resolve().is_relative_to(DEVELOPER.resolve())
                                 and report['clang_version'].startswith('Apple clang version '))
        if not all(checks.values()):
            raise RuntimeError('Xcode, SDK or compiler identity mismatch; SDK contents not scanned')
        settings = sdk_path / 'SDKSettings.plist'
        checks['sdk_settings_present'] = settings.is_file()
        if not settings.is_file():
            raise RuntimeError('SDKSettings.plist missing')
        with settings.open('rb') as stream:
            metadata = plistlib.load(stream)
        if not isinstance(metadata, dict):
            raise RuntimeError('SDKSettings.plist is not a dictionary')
        report['sdk_settings'] = {key: metadata.get(key) for key in
                                  ('CanonicalName', 'Version', 'DisplayName', 'ProductBuildVersion')}
        checks['sdk_settings_identity'] = (metadata.get('CanonicalName') == 'iphoneos26.5'
                                           and metadata.get('Version') == '26.5')
        report['required_paths'] = {name: (sdk_path / name).exists() for name in REQUIRED}
        checks['required_paths'] = all(report['required_paths'].values())
        checks['sdk_directories'] = all((sdk_path / name).is_dir() for name in
                                        ('usr/include', 'usr/lib', 'System/Library/Frameworks'))
        report['inventory'] = inventory(sdk_path, output / 'sdk-inventory.jsonl')
        report['inventory_sha256'] = digest(output / 'sdk-inventory.jsonl')
        checks['sdk_symlinks'] = not report['inventory']['broken_symlinks']
        checks['symlink_targets_contained'] = not report['inventory']['external_symlinks']
        checks['sdk_readable'] = not report['inventory']['unreadable_entries']
        checks['sdk_modules'] = report['inventory']['modulemap_count'] > 0
        report['os_version'] = command('sw_vers')
        if all(checks.values()):
            report['status'] = 'PASS'
    except (OSError, ValueError, RuntimeError, plistlib.InvalidFileException) as error:
        report['errors'].append(str(error))
    (output / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = probe(args.output)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
