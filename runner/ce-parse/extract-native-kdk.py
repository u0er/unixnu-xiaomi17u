#!/usr/bin/env python3
"""Select unmodified platform headers from signed KDK PBZX payloads."""
import pathlib
import stat
import struct
import subprocess
import sys


def chunks(path):
    with path.open('rb') as f:
        if f.read(4) != b'pbzx':
            raise ValueError('expected PBZX')
        f.read(8)
        while header := f.read(16):
            if len(header) != 16:
                raise ValueError('truncated PBZX header')
            _, size = struct.unpack('>QQ', header)
            if size > 32 * 1024 * 1024:
                raise ValueError('invalid PBZX chunk length')
            data = f.read(size)
            if len(data) != size:
                raise ValueError('truncated PBZX chunk')
            if data.startswith(b'\xfd7zXZ\x00'):
                data = subprocess.run(['xz', '-dc', '--ignore-check'], input=data,
                                      stdout=subprocess.PIPE, check=True).stdout
            yield data


class Reader:
    def __init__(self, iterator):
        self.iterator, self.buf = iter(iterator), bytearray()

    def read(self, size):
        while len(self.buf) < size:
            self.buf.extend(next(self.iterator))
        data = bytes(self.buf[:size])
        del self.buf[:size]
        return data


def extract(payload, output, prefixes):
    stream = Reader(chunks(payload))
    found = []
    while True:
        header = stream.read(76)
        if header[:6] != b'070707':
            raise ValueError('expected odc CPIO')
        mode = int(header[18:24], 8)
        n, size = int(header[59:65], 8), int(header[65:76], 8)
        name = stream.read(n)[:-1].decode().removeprefix('./')
        if name == 'TRAILER!!!':
            break
        path = pathlib.PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts:
            raise ValueError('unsafe CPIO path')
        selected = any(name.startswith(p) for p in prefixes) and name.endswith('.h') and stat.S_ISREG(mode)
        target = output / path
        if selected:
            target.parent.mkdir(parents=True, exist_ok=True)
        dest = target.open('xb') if selected else None
        try:
            while size:
                block = stream.read(min(size, 1024 * 1024))
                size -= len(block)
                if dest:
                    dest.write(block)
        finally:
            if dest:
                dest.close()
        if selected:
            found.append(name)
            print(f'KDK header: {name}', flush=True)
    return found


kernel, sdk, root = map(pathlib.Path, sys.argv[1:])
base = 'System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/'
found = extract(sdk, root, tuple(base + p + '/' for p in
                                 ('CoreEntitlements', 'TrustCache', 'CodeSignature', 'TrustedExecutionMonitor')))
found += extract(kernel, root, ('System/Library/KernelSupport/CoreEntitlements/V2/',))
required = {base + 'CoreEntitlements/V2/API.h', 'System/Library/KernelSupport/CoreEntitlements/V2/Kernel.h'}
if not required.issubset(found):
    raise ValueError(f'missing required KDK headers: {required - set(found)}')
