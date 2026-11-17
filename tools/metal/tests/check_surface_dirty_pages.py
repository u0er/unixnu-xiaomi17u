#!/usr/bin/env python3
"""Check the injected log against its pinned runtime contract without linking Vulkan or Darwin.

Run on a host with Python and rustc:
  python3 tools/metal/tests/check_surface_dirty_pages.py /path/to/metal2vulkan

The Rust tests compile the injected implementation, not a Python reimplementation. CF/IOSurface
and the diagnostic device snapshot are mocked; actual GPU ordering, Darwin ABI and rendering
still require the normal macOS build and phone.
"""
import argparse
import ast
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--rustc', default='rustc')
    parser.add_argument('--adapt', type=Path, help='alternate adapter for a baseline comparison')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    tree = ast.parse((args.adapt or here.parent / 'adapt_ios16.py').read_text(encoding='utf-8'))
    replacements = []
    for node in tree.body:
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            call = node.value
            if isinstance(call.func, ast.Name) and call.func.id == 'edit':
                replacements.append(tuple(ast.literal_eval(a) for a in call.args))
    injected = next(new for _, _, new in replacements if 'struct SurfaceDirtyPages {' in new)
    block = injected[injected.index('unsafe extern "C" {'):
                     injected.index('/// `nmb_buffer_new_no_copy_releasing`')]
    # This isolated test needs no Rust 2024 FFI syntax or inline-const initialization. Keep those
    # syntax-only substitutions so older distro rustc can check the actual synchronization.
    block = block.replace('unsafe extern "C" {', 'extern "C" {')
    block = block.replace('#[unsafe(no_mangle)]', '#[no_mangle]')
    block = block.replace('[const { std::sync::atomic::AtomicU64::new(0) }; 4]',
                          '[' + ', '.join(['std::sync::atomic::AtomicU64::new(0)'] * 4) + ']')
    runtime = args.runtime / 'crates/metal2vulkan-runtime/src/dirty_pages.rs'
    source = runtime.read_text(encoding='utf-8')
    trait = source[source.index('#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]'):
                   source.index('/// A [`DirtyPages`] source of any watch type')]
    trait = '\n'.join(line for line in trait.splitlines() if not line.lstrip().startswith('///'))
    fields = list(dict.fromkeys(re.findall(r'\bw\.(\w+)', block)))
    work_source = (args.runtime / 'crates/metal2vulkan-runtime/src/work_counts.rs').read_text(encoding='utf-8')
    for field in fields:
        if f'pub {field}: u64,' not in work_source:
            raise AssertionError(f'counter absent from pinned WorkCounts: {field}')
    prelude = 'use std::{ffi::c_void, num::NonZeroU64, ops::Range};\n'
    prelude += 'struct MockCounts {' + ''.join(f'{f}: u64,' for f in fields) + '}\n'
    prelude += 'struct MockDevice; pub struct NmbDevice(MockDevice);\n'
    prelude += 'impl MockDevice { fn work_counts(&self) -> MockCounts { MockCounts {'
    prelude += ''.join(f'{f}: {i + 100},' for i, f in enumerate(fields)) + '} } }\n'
    prelude += '''
fn get<'a>(device: *const NmbDevice) -> Result<&'a NmbDevice, String> {
    unsafe { device.as_ref() }.ok_or_else(|| "null device".into())
}
fn guard<T>(fallback: T, body: impl FnOnce() -> Result<T, String>) -> T {
    body().unwrap_or(fallback)
}
'''
    tests = (here / 'surface_dirty_pages.rs').read_text(encoding='utf-8')
    # The implementation precedes the tests, so its file-level documentation becomes a comment.
    tests = tests.replace('//!', '//')
    with tempfile.TemporaryDirectory(prefix='surface-dirty-pages-') as directory:
        root = Path(directory)
        rust = root / 'checks.rs'
        binary = root / 'checks'
        rust.write_text(prelude + trait + block + tests, encoding='utf-8')
        command = [args.rustc, '--edition=2021', '--test', str(rust), '-o', str(binary)]
        if 'fn nmb_device_dirty_counts_v2(' in block:
            command.extend(['--cfg', 'surface_dirty_v2'])
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

    telemetry = next(new for _, _, new in replacements if 'row("DG gated' in new)
    for row in re.findall(r'row\("(D[^"\n]*)"', telemetry):
        # Test the actual bound that previously truncated rec/cons, including worst-case u64s.
        width = len(row.replace('%llu', str(2**64 - 1)))
        if width > 90:
            raise AssertionError(f'telemetry row exceeds 90 columns: {width}: {row}')
    print('dirty-page telemetry rows fit the 90-character limit at uint64 maximum')


if __name__ == '__main__':
    main()
