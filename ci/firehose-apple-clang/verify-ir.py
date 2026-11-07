#!/usr/bin/env python3
"""Conservative evidence gate, not a substitute for an arm64e ABI review."""
from pathlib import Path
import re
import sys

def function(text, name):
    match = re.search(r'^define\b[^\n]*@' + re.escape(name) + r'\([^\n]*\)[^{]*\{(.*?)^\}', text, re.M | re.S)
    if not match:
        raise ValueError(f'missing IR function {name}')
    return match.group(1)

for arg in sys.argv[1:]:
    text = Path(arg).read_text()
    load = function(text, 'four_load')
    store = function(text, 'four_store')
    clear = function(text, 'four_clear')
    if 'llvm.ptrauth.auth' not in load or re.search(r'icmp\s+eq\b[^\n]*(?:null|\b0\b)', load):
        raise SystemExit(f'{arg}: four_load null authentication is not established; manual review required')
    if 'llvm.ptrauth.sign' not in store or 'llvm.ptrauth.sign' not in clear:
        raise SystemExit(f'{arg}: four_store/four_clear signing is not established; manual review required')
    print(f'{arg}: required auth/sign IR present; constant initialization and ABI still require review')
