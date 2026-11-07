#!/usr/bin/env python3
"""Validate a configured firehose input from the baseline shell."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from unixnu.frozen_firehose import check_tree
from unixnu.util import Error

if len(sys.argv) != 3:
    raise SystemExit('usage: check-frozen-firehose.py TREE HEADER_BUNDLE')
try:
    check_tree(sys.argv[1], sys.argv[2])
except (Error, OSError) as error:
    raise SystemExit(f'firehose validation failed: {error}') from error
