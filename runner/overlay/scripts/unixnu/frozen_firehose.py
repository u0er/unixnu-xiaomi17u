"""Candidate-only preparation for a pinned external firehose archive."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
from .util import Error

PIN = '86afb7baa36658fd963b4c57a7f3bb8c9a9e0281e8f45213141eeb932ee64b8d'
DISPATCH = '701f4d1a24ae9c6863901bbbb22624b7d1b87321'
XNU = 'ac9718fb1af618d5ce8678d0dc6e8a58f252216f'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate_archive(data):
    if digest(data) != PIN or not data.startswith(b'!<arch>\n'):
        raise Error('firehose archive does not match the frozen SHA-256')
    offset, objects = 8, []
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:60] != b'`\n':
            raise Error('malformed firehose archive member')
        size = int(header[48:58].strip())
        body = data[offset + 60:offset + 60 + size]
        if len(body) != size:
            raise Error('truncated firehose archive member')
        name = header[:16].decode('ascii').strip()
        if name.startswith('#1/'):
            count = int(name[3:])
            name, body = body[:count].rstrip(b'\0').decode('ascii'), body[count:]
        else:
            name = name.rstrip('/')
        if not name.startswith('__.SYMDEF'):
            objects.append((name, body))
        offset += 60 + size + (size & 1)
    if offset != len(data) or len(objects) != 1 or objects[0][0] != 'A.o':
        raise Error('unexpected firehose archive members')
    obj = objects[0][1]
    if len(obj) < 32:
        raise Error('truncated firehose Mach-O')
    magic, cpu, subtype, filetype, ncmds, command_bytes = struct.unpack_from('<6I', obj)
    if (magic, cpu, subtype, filetype) != (0xfeedfacf, 0x100000c, 0xc0000002, 1):
        raise Error('firehose must be an ARM64E kernel ABI MH_OBJECT')
    pos, versions = 32, []
    for _ in range(ncmds):
        if pos + 8 > len(obj):
            raise Error('truncated Mach-O load command')
        cmd, size = struct.unpack_from('<II', obj, pos)
        if size < 8 or pos + size > 32 + command_bytes or pos + size > len(obj):
            raise Error('invalid Mach-O load command')
        if cmd == 0x32:
            if size < 24:
                raise Error('invalid LC_BUILD_VERSION')
            versions.append(struct.unpack_from('<III', obj, pos + 8))
        pos += size
    if pos != 32 + command_bytes or versions != [(2, 26 << 16, (26 << 16) | (5 << 8))]:
        raise Error('firehose platform, minimum version or SDK mismatch')


def verify_headers(bundle, tree):
    with tarfile.open(bundle, 'r:gz') as archive:
        for item in archive:
            path = Path(item.name)
            if path.is_absolute() or '..' in path.parts:
                raise Error(f'unsafe header bundle entry: {item.name}')
            target = tree / path
            if item.isdir():
                if not target.is_dir():
                    raise Error(f'missing candidate header directory: {target}')
            elif item.issym():
                if not target.is_symlink() or os.readlink(target) != item.linkname:
                    raise Error(f'candidate header symlink mismatch: {target}')
            elif item.isfile():
                src = archive.extractfile(item)
                if target.is_symlink() or not target.is_file() or src is None or target.read_bytes() != src.read():
                    raise Error(f'candidate header mismatch: {target}')
            else:
                raise Error(f'unsupported header bundle entry: {item.name}')


def verify_bundle(path):
    try:
        repo = path.parents[2]
        frozen = subprocess.check_output(['git', '-C', str(repo), 'cat-file', 'blob',
                                          '49a37562c9c1e57963d558d4c322036602e3280e:ci/firehose-apple-clang/candidate-headers.tar.gz'])
        if path.read_bytes() != frozen:
            raise Error('header bundle differs from the frozen source commit')
    except (OSError, subprocess.CalledProcessError) as error:
        raise Error(f'frozen header bundle unavailable: {error}') from error


def restore_candidate_overlay(bundle, tree, backup):
    """Restore the pinned build overlay while retaining macOS-generated headers.

    Only the isolated fakeroot is modified; the frozen archive, Apple SDK and
    XNU source remain unchanged. Later verify_headers checks the full bundle.
    """
    verify_bundle(bundle)
    tree = Path(tree).resolve()
    root = tree / '.cache/fakeroot'
    backup = Path(backup)
    if not root.is_dir() or root.is_symlink() or backup.exists():
        raise Error('frozen overlay requires an installed isolated fakeroot and a new backup')
    backup.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(root, backup, symlinks=True)
    changes = []
    prefix = Path('.cache/fakeroot')
    with tarfile.open(bundle, 'r:gz') as archive:
        for item in archive:
            path = Path(item.name)
            if path.is_absolute() or '..' in path.parts:
                raise Error(f'unsafe frozen overlay entry: {item.name}')
            if not path.is_relative_to(prefix):
                continue
            relative = path.relative_to(prefix)
            if not relative.parts:
                continue
            target = root / relative
            if not target.parent.resolve().is_relative_to(root):
                raise Error(f'frozen overlay parent escapes fakeroot: {item.name}')
            if item.isdir():
                if target.is_symlink() or (target.exists() and not target.is_dir()):
                    raise Error(f'frozen overlay directory conflict: {target}')
                target.mkdir(parents=True, exist_ok=True)
            elif item.issym():
                if Path(item.linkname).is_absolute() or not (target.parent / item.linkname).resolve().is_relative_to(root):
                    raise Error(f'frozen overlay symlink escapes fakeroot: {item.name}')
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.is_symlink():
                    if os.readlink(target) == item.linkname:
                        continue
                    target.unlink()
                elif target.exists():
                    raise Error(f'frozen overlay symlink conflict: {target}')
                target.symlink_to(item.linkname)
                changes.append({'path': str(relative), 'type': 'symlink', 'target': item.linkname})
            elif item.isfile():
                if target.is_symlink() or (target.exists() and not target.is_file()):
                    raise Error(f'frozen overlay file conflict: {target}')
                source = archive.extractfile(item)
                if source is None:
                    raise Error(f'frozen overlay entry unreadable: {item.name}')
                data = source.read()
                previous = target.read_bytes() if target.exists() else None
                if previous != data:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
                    changes.append({'path': str(relative),
                                    'generated_sha256': digest(previous) if previous is not None else None,
                                    'frozen_sha256': digest(data)})
            else:
                raise Error(f'unsupported frozen overlay entry: {item.name}')
    return changes


def install_exact(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        if path.is_symlink() or not path.is_file() or path.read_bytes() != data:
            raise Error(f'refusing to overwrite existing candidate input: {path}')
        return
    name = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.firehose-', delete=False) as tmp:
            name = tmp.name
            tmp.write(data)
        os.link(name, path)
    finally:
        if name and os.path.exists(name):
            os.unlink(name)


def preflight(ctx):
    """Validate configured immutable inputs before the normal build can run."""
    spec = ctx.board.get('kernel', {}).get('prebuilt_firehose')
    if spec is None:
        board = ctx.board.get('board', {})
        if board.get('name') == 'nezha' and board.get('os_profile') == 'ipados-26.5-23F77-nezha':
            raise Error('Nezha 23F77 requires [kernel.prebuilt_firehose]')
        return None
    if not isinstance(spec, dict) or set(spec) != {'archive', 'manifest', 'headers', 'sha256'}:
        raise Error('candidate [kernel.prebuilt_firehose] must specify exact inputs')
    if spec['sha256'] != PIN:
        raise Error('candidate firehose configuration digest mismatch')
    paths = {key: (ctx.device / spec[key]).resolve() for key in ('archive', 'manifest', 'headers')}
    try:
        manifest = json.loads(paths['manifest'].read_text())
    except (OSError, ValueError) as error:
        raise Error(f'cannot read frozen firehose manifest: {error}') from error
    if not isinstance(manifest, dict):
        raise Error('frozen firehose manifest must be a JSON object')
    expected = {'archive_sha256': PIN, 'libdispatch_commit': DISPATCH, 'xnu_public_source_commit': XNU,
                'target': 'arm64e-apple-ios26.0', 'minimum_os': '26.0', 'sdk': 'iPhoneOS 26.5',
                'xcode': '26.5 (17F42)', 'clang': 'Apple clang 21.0.0 (clang-2100.1.1.101)',
                'frozen_header_bundle_git_commit': '49a37562c9c1e57963d558d4c322036602e3280e',
                'frozen_header_bundle_git_blob': '5737bc2701916ef58917a4943f5b2e0cfbb4b0a2'}
    if any(manifest.get(k) != v for k, v in expected.items()):
        raise Error('frozen firehose manifest mismatch')
    profile = ctx.board.get('os', {})
    if profile.get('arch') != 'arm64e' or profile.get('ios_version') != '26.0':
        raise Error('resolved candidate architecture or deployment target mismatch')
    try:
        verify_bundle(paths['headers'])
        validate_archive(paths['archive'].read_bytes())
    except (OSError, subprocess.CalledProcessError) as error:
        raise Error(f'frozen firehose input unavailable: {error}') from error
    return paths


def prepare(ctx, modules, env):
    paths = preflight(ctx)
    if paths is None:
        return None
    if env.get('UNIXNU_XNU_ARCH') != 'arm64e' or env.get('UNIXNU_IOS_VERSION') != '26.0':
        raise Error('resolved candidate architecture or deployment target mismatch')
    if 'DARWIN_TRIPLE' in env and env['DARWIN_TRIPLE'] != 'arm64e-apple-ios26.0':
        raise Error('candidate Linux compiler triple does not match frozen firehose')
    for name, expected_commit in (('libdispatch', DISPATCH), ('xnu', XNU)):
        module = modules[('refer', name)]
        if module.source.get('commit') != expected_commit:
            raise Error(f'{name} feed commit mismatch')
        head = subprocess.check_output(['git', '-C', str(module.path), 'rev-parse', 'HEAD'], text=True).strip()
        if head != expected_commit:
            raise Error(f'{name} checkout commit mismatch')
    dispatch = modules[('refer', 'libdispatch')].path
    dirty = subprocess.check_output(['git', '-C', str(dispatch), 'status', '--porcelain', '--',
                                     'src/firehose/firehose_buffer.c', 'os/firehose_buffer_private.h'], text=True)
    if dirty:
        raise Error('frozen firehose source or private header was modified')
    tree = ctx.tree.resolve()
    sdk = tree / '.cache/fakeroot'
    if not (sdk / '.xnu-12377.121.6-headers').is_file():
        raise Error('candidate installhdrs completion marker missing; do not skip its generation')
    verify_headers(paths['headers'], tree)
    data = paths['archive'].read_bytes()
    validate_archive(data)
    source = modules[('refer', 'libdispatch')].path / 'os/firehose_buffer_private.h'
    result = subprocess.run(['unifdef', '-DKERNEL=1', '-DOS_FIREHOSE_SPI=1',
                             '-DOS_VOUCHER_ACTIVITY_SPI_TYPES=1', '-UOS_VOUCHER_ACTIVITY_SPI', str(source)],
                            env=env, capture_output=True)
    if result.returncode not in (0, 1) or not result.stdout:
        raise Error(f'firehose private header generation failed: {result.stderr.decode(errors="replace")}')
    install_exact(sdk / 'usr/local/include/kernel/os/firehose_buffer_private.h', result.stdout)
    target = sdk / 'usr/local/lib/kernel/libfirehose_kernel.a'
    install_exact(target, data)
    validate_archive(target.read_bytes())
    print(f'candidate firehose verified: {target} sha256={PIN}', flush=True)
    return target


def check_tree(tree, bundle):
    """Recheck after header preparation and immediately before kernel make."""
    tree = Path(tree).resolve()
    bundle = Path(bundle).resolve()
    verify_bundle(bundle)
    verify_headers(bundle, tree)
    target = tree / '.cache/fakeroot/usr/local/lib/kernel/libfirehose_kernel.a'
    try:
        validate_archive(target.read_bytes())
    except OSError as error:
        raise Error(f'prebuilt firehose missing before kernel build: {error}') from error
    print(f'firehose link input verified: {target}', flush=True)
