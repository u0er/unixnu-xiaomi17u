#!/usr/bin/env python3
"""Extract only original CE headers from two KDK PBZX payloads."""
import lzma
import pathlib
import stat
import struct
import sys


def chunks(path):
    with path.open('rb') as f:
        if f.read(4) != b'pbzx':
            raise ValueError('expected PBZX')
        f.read(8)
        while header := f.read(16):
            if len(header) != 16:
                raise ValueError('short PBZX header')
            _, size = struct.unpack('>QQ', header)
            if size > 32 * 1024 * 1024:
                raise ValueError('invalid PBZX chunk size')
            data = f.read(size)
            if len(data) != size:
                raise ValueError('short PBZX chunk')
            yield lzma.decompress(data) if data.startswith(b'\xfd7zXZ\x00') else data


class Reader:
    def __init__(self, it):
        self.it, self.buf = iter(it), bytearray()

    def read(self, size):
        while len(self.buf) < size:
            self.buf.extend(next(self.it))
        out = bytes(self.buf[:size])
        del self.buf[:size]
        return out


def extract(payload, output, allow):
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
        selected = name in allow and stat.S_ISREG(mode)
        dest = (output / pathlib.PurePosixPath(name).name).open('xb') if selected else None
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
            print(f'extracted {name}', flush=True)
    if set(found) != set(allow):
        raise ValueError(f'headers missing: {set(allow) - set(found)}')


kernel, sdk, root = map(pathlib.Path, sys.argv[1:])
out = root / 'CoreEntitlements/V2'
out.mkdir(parents=True)
prefix = 'System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/CoreEntitlements/V2/'
extract(sdk, out, {prefix + x + '.h' for x in ('API', 'Acceleration', 'Closure', 'Context', 'Return', 'StackProtector')})
extract(kernel, out, {'System/Library/KernelSupport/CoreEntitlements/V2/Kernel.h'})
