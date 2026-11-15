#!/usr/bin/env python3
"""Assemble a host-only Bluetooth trial from a kernel and an observed boot image.

Reuse the base image's ADT, trust-cache bytes, memory bases and boot arguments.
An explicit --fw-extents can replace a stale firmware mapping with an existing
pinned container's recorded map. Require a 90-second trial.
The loader is rebuilt by the existing build_nezha.py in --build-root, which
must be the loader source used for the selected baseline. No device commands
or firmware-container writes are part of this tool.
"""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import adt
import mkpayload
import mkadt_pandora


@dataclass
class Payload:
    """Validated SWEETXNU v1 fields; segment offsets are payload-relative."""
    virt: int
    entry: int
    adt_off: int
    adt_size: int
    kernel_end: int
    phys: int
    mem: int
    segments: list
    command: bytes


def find_payload(image):
    """Locate a v1 payload, ignoring the loader's diagnostic magic string.

    Use the existing sideload/update_trustcache.py locator's version/count
    convention. That helper only exists in the external baseline tree, so
    this tool keeps the small format-specific locator with its own parser.
    """
    cursor = 0
    found = []
    while True:
        cursor = image.find(b'SWEETXNU', cursor)
        if cursor < 0:
            break
        if cursor + 16 <= len(image):
            version, count = struct.unpack_from('<II', image, cursor + 8)
            if version == 1 and 0 < count <= 64:
                found.append(cursor)
        cursor += 1
    if len(found) != 1:
        raise ValueError(f'expected one SWEETXNU v1 payload, found {len(found)}')
    return found[0]


def parse_payload(data):
    if len(data) < mkpayload.SEG_TABLE or data[:8] != b'SWEETXNU':
        raise ValueError('not a complete SWEETXNU payload')
    version, count = struct.unpack_from('<II', data, 8)
    if version != 1 or not 0 < count <= 64:
        raise ValueError('unsupported payload version or segment count')
    table_end = mkpayload.SEG_TABLE + count * 32
    if table_end > len(data):
        raise ValueError('truncated payload segment table')
    fields = struct.unpack_from('<7Q', data, 16)
    segments = [struct.unpack_from('<4Q', data, mkpayload.SEG_TABLE + 32 * i)
                for i in range(count)]
    result = Payload(*fields, segments, data[72:mkpayload.SEG_TABLE])
    if result.adt_off < table_end or result.adt_off + result.adt_size > len(data):
        raise ValueError('ADT is outside the payload')
    previous_end = 0
    for va, size, off, length in sorted(segments):
        if size == 0 or length > size or off < table_end or off + length > result.adt_off:
            raise ValueError('invalid payload segment bounds')
        if va < previous_end or va < result.virt:
            raise ValueError('overlapping or out-of-range virtual segments')
        if va + size > result.virt + result.mem:
            raise ValueError('segment exceeds kernel-managed memory')
        previous_end = va + size
    if previous_end > result.kernel_end:
        raise ValueError('kernel_end does not cover all segments')
    if not any(va <= result.entry < va + size for va, size, _, _ in segments):
        raise ValueError('entry is outside the segment map')
    return result


def memory_map(tree):
    chosen = [node for node in tree.children if node.name == 'chosen']
    if len(chosen) != 1:
        raise ValueError('expected one ADT chosen node')
    maps = [node for node in chosen[0].children if node.name == 'memory-map']
    if len(maps) != 1:
        raise ValueError('expected one ADT memory-map node')
    return maps[0]


def trust_cache(data, payload, tree):
    """Resolve the ADT range instead of assuming the last segment is the cache."""
    prop = memory_map(tree).prop('TrustCache')
    if prop is None or len(prop) != 16:
        raise ValueError('baseline needs a TrustCache physical range')
    physical, length = struct.unpack('<2Q', prop)
    virtual = physical - payload.phys + payload.virt
    matches = [(va, off) for va, _, off, count in payload.segments
               if va <= virtual and virtual + length <= va + count]
    if length == 0 or len(matches) != 1:
        raise ValueError('TrustCache is not backed by exactly one file segment')
    va, off = matches[0]
    return data[off + virtual - va:off + virtual - va + length]


def repack(data, payload, tree, cache, baseline):
    """Keep the observed physmap base and relocate the unchanged trust cache.

    mkpayload derives virt_base from kernel segments. The observed Nezha lane
    uses an older, lower base independently of __TEXT; preserve it so physmap
    and MMIO mappings follow the same boot contract as the baseline.
    """
    if (payload.phys, payload.mem) != (baseline.phys, baseline.mem):
        raise ValueError('new kernel payload changed the ADT memory contract')
    cache_va = payload.kernel_end
    cache_size = (len(cache) + mkpayload.PAGE - 1) & ~(mkpayload.PAGE - 1)
    cache_pa = cache_va - baseline.virt + baseline.phys
    mmap = memory_map(tree)
    hits = [i for i, prop in enumerate(mmap.props) if prop[0] == 'TrustCache']
    if len(hits) != 1:
        raise ValueError('expected exactly one inherited TrustCache property')
    i = hits[0]
    mmap.props[i] = ('TrustCache', mmap.props[i][1], struct.pack('<2Q', cache_pa, len(cache)))
    pieces = [(va, size, data[off:off + count]) for va, size, off, count in payload.segments]
    pieces.append((cache_va, cache_size, cache))
    output = bytearray(mkpayload.SEG_TABLE + 32 * len(pieces))
    table = []
    for va, size, contents in pieces:
        off = len(output)
        output += contents
        output += b'\0' * (-len(output) & 15)
        table.append((va, size, off, len(contents)))
    tree_data = adt.serialize(tree)
    tree_off = len(output)
    output += tree_data
    output += b'\0' * (-len(output) & 15)
    struct.pack_into('<8sII', output, 0, b'SWEETXNU', 1, len(pieces))
    struct.pack_into('<7Q', output, 16, baseline.virt, payload.entry, tree_off,
                     len(tree_data), cache_va + cache_size, payload.phys, payload.mem)
    output[72:mkpayload.SEG_TABLE] = payload.command
    for i, values in enumerate(table):
        struct.pack_into('<4Q', output, mkpayload.SEG_TABLE + i * 32, *values)
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--base-boot', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, required=True)
    parser.add_argument('--llvm', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--level', type=int, choices=range(1, 11), default=2)
    parser.add_argument('--ci-run', help='optional full-kernel CI run number')
    parser.add_argument('--fw-extents', type=Path,
                        help='optional current pinned firmware map; updates ADT only, never the container')
    args = parser.parse_args()
    for name in ('kernel', 'base_boot', 'build_root', 'llvm', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    if args.fw_extents:
        args.fw_extents = args.fw_extents.resolve()
    if args.out.exists():
        parser.error('--out must be a new directory')
    builder = args.build_root / 'tools/build_nezha.py'
    if not args.kernel.is_file() or not args.base_boot.is_file() or not builder.is_file():
        parser.error('kernel, base boot image and loader builder must exist')
    image = args.base_boot.read_bytes()
    base_off = find_payload(image)
    base_data = image[base_off:]
    baseline = parse_payload(base_data)
    base_tree, consumed = adt.parse(base_data[baseline.adt_off:baseline.adt_off + baseline.adt_size])
    if consumed != baseline.adt_size:
        raise ValueError('baseline ADT was not fully parsed')
    cache = trust_cache(base_data, baseline, base_tree)
    if args.fw_extents:
        # FIEMAP is collected separately with pinfile's read-only extents
        # operation. Preserve root/log mapping and change only these two
        # firmware properties; the existing producer validates 4 KiB units
        # and logical coverage instead of introducing a second encoder.
        chosen = next(node for node in base_tree.children if node.name == 'chosen')
        for key, value in mkadt_pandora.fw_extent_props(args.fw_extents):
            hits = [i for i, prop in enumerate(chosen.props) if prop[0] == key]
            if len(hits) != 1:
                raise ValueError(f'expected one inherited firmware property: {key}')
            i = hits[0]
            chosen.props[i] = (key, chosen.props[i][1], value)
    command = baseline.command.split(b'\0')[0].decode('ascii').split()
    if command.count('pandora_reboot_after=90') != 1 or sum(
            arg.startswith('pandora_reboot_after=') for arg in command) != 1:
        raise ValueError('baseline must contain exactly one 90-second automatic return argument')
    if sum(arg.startswith('pandora_bt=') for arg in command) != 1:
        raise ValueError('baseline must contain exactly one pandora_bt argument')
    command = [f'pandora_bt={args.level}' if arg.startswith('pandora_bt=') else arg for arg in command]
    command_line = ' '.join(command)
    args.out.mkdir(parents=True)
    tree_file = args.out / 'baseline.adt'
    tree_file.write_bytes(adt.serialize(base_tree))
    new_data, _ = mkpayload.build(str(args.kernel), str(tree_file), command_line)
    new_header = parse_payload(new_data)
    new_tree, _ = adt.parse(new_data[new_header.adt_off:new_header.adt_off + new_header.adt_size])
    trusted = repack(new_data, new_header, new_tree, cache, baseline)
    checked = parse_payload(trusted)
    checked_tree, _ = adt.parse(trusted[checked.adt_off:checked.adt_off + checked.adt_size])
    if trust_cache(trusted, checked, checked_tree) != cache:
        raise ValueError('trust-cache transplant changed its contents')
    payload_path = args.out / 'trusted.payload'
    payload_path.write_bytes(trusted)
    # Embedded Python does not add the script directory under -I. Use the same
    # bootstrap as tools/turnip/device_trial.py, retaining sibling imports.
    bootstrap = ('import runpy,sys; from pathlib import Path; '
                 'script=sys.argv.pop(1); sys.path.insert(0,str(Path(script).parent)); '
                 'runpy.run_path(script,run_name="__main__")')
    log_path = args.out / 'loader-build.log'
    with log_path.open('w', encoding='utf-8') as log:
        subprocess.run([sys.executable, '-I', '-c', bootstrap, str(builder), 'loader',
                        '--llvm', str(args.llvm), '--payload', str(payload_path),
                        '--out', str(args.out / 'loader')], cwd=args.build_root,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    result_path = args.out / 'loader/nezha-loader-boot.img'
    result = result_path.read_bytes()
    at = find_payload(result)
    if result[at:at + len(trusted)] != trusted:
        raise ValueError('loader does not contain the prepared payload')
    record = dict(kernel=str(args.kernel), base_boot=str(args.base_boot),
                  build_root=str(args.build_root), ci_run=args.ci_run,
                  boot_image=str(result_path), bootargs=command_line, runtime_seconds=90,
                  kernel_bytes=args.kernel.stat().st_size, boot_image_bytes=len(result),
                  virtual_base=hex(checked.virt), physical_base=hex(checked.phys),
                  trust_cache_bytes=len(cache), firmware_container_written=False,
                  firmware_extents=str(args.fw_extents) if args.fw_extents else None,
                  device_booted=False)
    (args.out / 'trial.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(f'Prepared {result_path} ({len(result)} bytes), pandora_bt={args.level}, return=90 s.')
    print('Host preparation only; no device operation performed.')


if __name__ == '__main__':
    main()
