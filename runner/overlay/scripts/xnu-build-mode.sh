#!/usr/bin/env bash
# XNU-only produces an unsealed experimental native kernel.
unixnu_finish_source_crypto() {
  local mode=$1 kernel=$2
  case "$mode" in
    standard) python3 scripts/seal-source-crypto-kernel.py "$kernel" ;;
    xnu-only) return 0 ;;
    *) echo "unknown XNU build mode: $mode" >&2; return 1 ;;
  esac
}
