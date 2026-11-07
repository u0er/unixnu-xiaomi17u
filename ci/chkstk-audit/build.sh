#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT"
exec > >(tee "$OUT/build.log") 2>&1
echo "workflow=${GITHUB_SHA:-unknown}"
sw_vers
xcodebuild -version
CLANG="$(xcrun --sdk iphoneos --find clang)"
"$CLANG" --version
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
echo "sdk=$SDK version=$(xcrun --sdk iphoneos --show-sdk-version)"
test "$(xcrun --sdk iphoneos --show-sdk-version)" = 26.5
ARGS=(-target arm64e-apple-ios26.0 -arch arm64e -mkernel -isysroot "$SDK" -x assembler-with-cpp
      -DCHKSTK_AUDIT_ONLY=1 -DCHKSTK_AUDIT_STRIDE=4096 -c "$ROOT/chkstk-audit-only.S" -o "$OUT/chkstk-audit-only.o")
printf 'command:'; printf ' %q' "$CLANG" "${ARGS[@]}"; echo
"$CLANG" -### "${ARGS[@]}" > "$OUT/driver-plan.txt" 2>&1
"$CLANG" "${ARGS[@]}"
xcrun otool -h "$OUT/chkstk-audit-only.o" > "$OUT/header.txt"
xcrun otool -hv "$OUT/chkstk-audit-only.o" >> "$OUT/header.txt"
xcrun otool -l "$OUT/chkstk-audit-only.o" > "$OUT/loadcommands.txt"
xcrun otool -tvV "$OUT/chkstk-audit-only.o" > "$OUT/disassembly.txt"
xcrun otool -rv "$OUT/chkstk-audit-only.o" > "$OUT/relocations.txt"
xcrun nm -m "$OUT/chkstk-audit-only.o" > "$OUT/symbols.txt"
