"""Check SDK version metadata without inferring SDK provenance or completeness."""
import json
from pathlib import Path
import plistlib

from .util import Error


def verify_iphoneos_sdk(root, expected_version):
    """Require every present SDKSettings file to name the selected iPhoneOS SDK."""
    root = Path(root)
    settings = (root / 'SDKSettings.json', root / 'SDKSettings.plist')
    present = [path for path in settings if path.is_file()]
    if not present:
        raise Error(f'{root}: SDKSettings metadata missing; SDK source remains UNKNOWN')
    for path in present:
        try:
            with path.open('rb') as stream:
                metadata = plistlib.load(stream) if path.suffix == '.plist' else json.load(stream)
        except (OSError, ValueError, plistlib.InvalidFileException) as error:
            raise Error(f'{path}: invalid SDKSettings metadata: {error}') from error
        if not isinstance(metadata, dict):
            raise Error(f'{path}: SDKSettings must be a dictionary')
        canonical = metadata.get('CanonicalName')
        version = metadata.get('Version')
        if not isinstance(canonical, str) or canonical.lower() != f'iphoneos{expected_version}'.lower():
            raise Error(f'{path}: expected iPhoneOS {expected_version} SDK identity')
        if not isinstance(version, str) or version != expected_version:
            raise Error(f'{path}: SDK version mismatch (expected {expected_version})')
