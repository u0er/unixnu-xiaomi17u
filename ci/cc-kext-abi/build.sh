#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT" "$RUNNER_TEMP/cc-kext-input"
exec > >(tee "$OUT/build.log") 2>&1
echo "workflow=${GITHUB_SHA:-unknown}"
sw_vers
xcodebuild -version
CLANG="$(xcrun --sdk iphoneos --find clang)"
"$CLANG" --version
echo "iphoneos_sdk=$(xcrun --sdk iphoneos --show-sdk-version)"
test "$(xcrun --sdk iphoneos --show-sdk-version)" = 26.5
for pair in "llvm 58c46bae21181fb4240cf964461bea738d96d50e" "libc 899a3b2d52d95d75e05fb286a5e64975ec3de757" "xnu ac9718fb1af618d5ce8678d0dc6e8a58f252216f"; do
  set -- $pair
  actual="$(git -C "$ROOT/$1" rev-parse HEAD)"
  echo "$1=$actual"
  test "$actual" = "$2"
done
tar -xzf "$ROOT/../firehose-apple-clang/candidate-headers.tar.gz" -C "$RUNNER_TEMP/cc-kext-input"
TREE="$RUNNER_TEMP/cc-kext-input"
SDK="$TREE/.cache/fakeroot"
RT="$ROOT/llvm/compiler-rt"
COMMON=(-target arm64e-apple-ios26.0 -arch arm64e -isysroot "$SDK" -mkernel -DKERNEL_USE -O3 -fPIC -w
        -mllvm -disable-atexit-based-global-dtor-lowering
        -I "$RT/include" -I "$SDK/System/Library/Frameworks/Kernel.framework/Headers"
        -idirafter "$ROOT/libc/include" -idirafter "$ROOT/xnu/libsyscall/wrappers")
NAMES=(InstrProfiling InstrProfilingBuffer InstrProfilingPlatformDarwin InstrProfilingWriter InstrProfilingInternal InstrProfilingVersionVar)
OBJECTS=()
for NAME in "${NAMES[@]}"; do
  SOURCE="$RT/lib/profile/$NAME.c"
  OBJECT="$OUT/$NAME.o"
  printf 'command:'; printf ' %q' "$CLANG" "${COMMON[@]}" -c "$SOURCE" -o "$OBJECT"; echo
  "$CLANG" "${COMMON[@]}" -### -c "$SOURCE" -o "$OBJECT" > "$OUT/$NAME.driver.txt" 2>&1
  grep -- '-fptrauth-kernel-abi-version' "$OUT/$NAME.driver.txt" >/dev/null
  "$CLANG" "${COMMON[@]}" -c "$SOURCE" -o "$OBJECT"
  xcrun otool -h "$OBJECT" > "$OUT/$NAME.header.txt"
  xcrun otool -hv "$OBJECT" >> "$OUT/$NAME.header.txt"
  xcrun otool -l "$OBJECT" > "$OUT/$NAME.loadcommands.txt"
  xcrun nm -m "$OBJECT" > "$OUT/$NAME.symbols.txt"
  OBJECTS+=("$OBJECT")
done
xcrun libtool -static -o "$OUT/libcc_kext.a" "${OBJECTS[@]}"
xcrun lipo -info "$OUT/libcc_kext.a" > "$OUT/archive-arch.txt"
xcrun ar -t "$OUT/libcc_kext.a" > "$OUT/archive-members.txt"
xcrun otool -hv "$OUT/libcc_kext.a" > "$OUT/archive-headers.txt"
shasum -a 256 "$OUT/libcc_kext.a" "${OBJECTS[@]}" > "$OUT/SHA256SUMS"
