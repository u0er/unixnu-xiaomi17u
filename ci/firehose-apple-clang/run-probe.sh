#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT"
exec > >(tee "$OUT/probe.log") 2>&1
echo "commit: ${GITHUB_SHA:-unknown}"
sw_vers
uname -a
test -d "$DEVELOPER_DIR"
xcodebuild -version
CLANG="$(xcrun --sdk iphoneos --find clang)"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
SDK_VERSION="$(xcrun --sdk iphoneos --show-sdk-version)"
echo "clang=$CLANG"
"$CLANG" --version
echo "sdk=$SDK version=$SDK_VERSION"
test "$SDK_VERSION" = 26.5
COMMON=(-target arm64e-apple-ios26.0 -arch arm64e -isysroot "$SDK" -mkernel -O0)
"$CLANG" "${COMMON[@]}" -x c -### -c "$ROOT/qualifier.c" -o "$OUT/driver.o"
for LANG_MODE in c c++; do
  if [ "$LANG_MODE" = c ]; then PREFIX=c; else PREFIX=cpp; fi
  "$CLANG" "${COMMON[@]}" -x "$LANG_MODE" -S -emit-llvm "$ROOT/qualifier.c" -o "$OUT/$PREFIX.ll"
  "$CLANG" "${COMMON[@]}" -x "$LANG_MODE" -S "$ROOT/qualifier.c" -o "$OUT/$PREFIX.s"
  "$CLANG" "${COMMON[@]}" -x "$LANG_MODE" -c "$ROOT/qualifier.c" -o "$OUT/$PREFIX.o"
  xcrun otool -hv "$OUT/$PREFIX.o" > "$OUT/$PREFIX.header.txt"
  xcrun otool -l "$OUT/$PREFIX.o" > "$OUT/$PREFIX.loadcommands.txt"
  xcrun otool -rv "$OUT/$PREFIX.o" > "$OUT/$PREFIX.relocations.txt"
  xcrun otool -tvV "$OUT/$PREFIX.o" > "$OUT/$PREFIX.disassembly.txt"
done
python3 "$ROOT/verify-ir.py" "$OUT/c.ll" "$OUT/cpp.ll"
