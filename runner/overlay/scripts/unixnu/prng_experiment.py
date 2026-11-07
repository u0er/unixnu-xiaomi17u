"""Fail-closed preflight for the restricted Candidate A PRNG experiment.

The executor is deliberately unavailable until its complete source, command,
and isolation boundary have been validated. This module cannot invoke make.
"""
import os
from pathlib import Path
import platform
import subprocess

from . import sdk_metadata
from .config import ROOT, load_toml
from .util import Error

DEVELOPER = Path('/Applications/Xcode_26.5.app/Contents/Developer')
PROFILE = ROOT / 'profiles/os/ipados-26.5-23F77-nezha/profile.toml'


def preflight():
    """Validate the declared SDK identity before permitting any future executor."""
    profile = load_toml(PROFILE).get('os', {})
    version = profile.get('sdk_version')
    if not isinstance(version, str) or not version:
        raise Error('prng experiment requires a verified os.sdk_version in the candidate profile')
    if profile.get('arch') != 'arm64e' or profile.get('ios_version') != '26.0':
        raise Error('prng experiment requires the Candidate A arm64e/iOS 26.0 profile')
    if platform.system() != 'Darwin' or os.environ.get('DEVELOPER_DIR') != str(DEVELOPER):
        raise Error('prng experiment requires the isolated Xcode 26.5 macOS runner')
    env = dict(os.environ, DEVELOPER_DIR=str(DEVELOPER))
    try:
        def command(*args):
            return subprocess.run(args, check=True, text=True, capture_output=True, env=env).stdout.strip()
        if command('/usr/bin/xcodebuild', '-version').splitlines() != ['Xcode 26.5', 'Build version 17F42']:
            raise Error('prng experiment requires Xcode 26.5 build 17F42')
        sdk = Path(command('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-path'))
        expected = DEVELOPER / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS26.5.sdk'
        if not sdk.is_dir() or not expected.is_dir() or sdk.resolve() != expected.resolve():
            raise Error('prng experiment SDK path is not the selected Xcode SDK')
        if version != '26.5' or command('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-version') != version:
            raise Error('prng experiment SDK version differs from the candidate profile')
        if command('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version') != '23F73':
            raise Error('prng experiment requires the verified iPhoneOS SDK build 23F73')
        clang = Path(command('/usr/bin/xcrun', '--sdk', 'iphoneos', '--find', 'clang'))
        if not clang.is_file() or not clang.resolve().is_relative_to(DEVELOPER.resolve()):
            raise Error('prng experiment clang is not inside the selected Xcode')
        if not command(str(clang), '--version').startswith('Apple clang version 21.0.0 '):
            raise Error('prng experiment requires Apple clang 21.0.0')
        sdk_metadata.verify_iphoneos_sdk(sdk, version)
        if not all((sdk / name).is_file() for name in ('SDKSettings.plist', 'usr/include/stdio.h',
                                                       'usr/include/TargetConditionals.h', 'usr/lib/libSystem.tbd')):
            raise Error('prng experiment SDK is missing required preflight files')
    except (OSError, subprocess.CalledProcessError) as error:
        raise Error(f'prng experiment SDK preflight failed: {error}') from error
    # Identity alone does not prove sysroot binding, input closure or no-link execution.
    raise Error('prng experiment unavailable: isolated executor and SDK binding are not validated')
