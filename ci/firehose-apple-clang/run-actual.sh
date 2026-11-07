#!/bin/bash
set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/actual-out"
mkdir -p "$OUT"
exec > >(tee "$OUT/run.log") 2>&1
fail() { echo "ERROR: $*"; exit 1; }
echo "workflow commit: ${GITHUB_SHA:-unknown}"
sw_vers || fail 'sw_vers'
xcodebuild -version || fail 'xcodebuild'
CLANG="$(xcrun --sdk iphoneos --find clang)" || fail 'clang path'
IOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)" || fail 'iPhoneOS SDK'
IOS_SDK_VER="$(xcrun --sdk iphoneos --show-sdk-version)" || fail 'SDK version'
echo "clang=$CLANG sdk=$IOS_SDK sdk_version=$IOS_SDK_VER"
"$CLANG" --version || fail 'clang version'
[ "$IOS_SDK_VER" = 26.5 ] || fail 'requires the frozen Xcode 26.5 SDK'
DISPATCH="$ROOT/libdispatch"
HEAD="$(git -C "$DISPATCH" rev-parse HEAD)" || fail 'libdispatch HEAD'
[ "$HEAD" = 701f4d1a24ae9c6863901bbbb22624b7d1b87321 ] || fail 'unexpected libdispatch'
echo "libdispatch=$HEAD"
INPUT="$RUNNER_TEMP/candidate-firehose-input"
mkdir -p "$INPUT" || fail 'input directory'
tar -xzf "$ROOT/candidate-headers.tar.gz" -C "$INPUT" || fail 'candidate headers extraction'
FAKEROOT="$INPUT/.cache/fakeroot"
XNU="$INPUT/.cache/xnu-work"
SOURCE="$DISPATCH/src/firehose/firehose_buffer.c"
[ -f "$FAKEROOT/usr/include/TargetConditionals.h" ] || fail 'candidate TargetConditionals.h missing'
[ -f "$FAKEROOT/usr/local/include/firehose/firehose_types_private.h" ] || fail 'candidate firehose types missing'
[ -f "$FAKEROOT/System/Library/Frameworks/Kernel.framework/PrivateHeaders/sys/ucred.h" ] || fail 'candidate ucred missing'
FLAGS=(-target arm64e-apple-ios26.0 -arch arm64e -isysroot "$IOS_SDK"
       -mllvm -disable-atexit-based-global-dtor-lowering -std=gnu11 -Os -g -mkernel -nostdinc -Wno-packed
       -DKERNEL=1 -DDISPATCH_USE_DTRACE=0 -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1
       -DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0
       -I "$DISPATCH" -I "$FAKEROOT/System/Library/Frameworks/Kernel.framework/PrivateHeaders"
       -I "$FAKEROOT/System/Library/Frameworks/Kernel.framework/Headers" -I "$FAKEROOT/usr/local/include/os"
       -I "$FAKEROOT/usr/local/include/firehose" -I "$FAKEROOT/usr/include" -I "$FAKEROOT/usr/local/include"
       -I "$XNU/EXTERNAL_HEADERS")
printf 'compiler command: %q ' "$CLANG" "${FLAGS[@]}" -c "$SOURCE" -o "$OUT/A.o"; echo
"$CLANG" "${FLAGS[@]}" -### -c "$SOURCE" -o "$OUT/driver.o" > "$OUT/driver-plan.txt" 2>&1 || fail 'driver plan'
compile_variant() {
  local name="$1"; shift
  local result=0
  if ! "$CLANG" "${FLAGS[@]}" "$@" -E -P "$SOURCE" -o "$OUT/$name.i" 2> "$OUT/$name.pp.stderr"; then result=1; fi
  if ! "$CLANG" "${FLAGS[@]}" "$@" -fsyntax-only -Xclang -ast-dump "$SOURCE" > "$OUT/$name.ast" 2> "$OUT/$name.ast.stderr"; then result=1; fi
  if ! "$CLANG" "${FLAGS[@]}" "$@" -S -emit-llvm "$SOURCE" -o "$OUT/$name.ll" 2> "$OUT/$name.ir.stderr"; then result=1; fi
  if ! "$CLANG" "${FLAGS[@]}" "$@" -S "$SOURCE" -o "$OUT/$name.s" 2> "$OUT/$name.asm.stderr"; then result=1; fi
  if ! "$CLANG" "${FLAGS[@]}" "$@" -c "$SOURCE" -o "$OUT/$name.o" 2> "$OUT/$name.obj.stderr"; then result=1; fi
  echo "$name result=$result"
  if [ -f "$OUT/$name.o" ]; then
    xcrun otool -hv "$OUT/$name.o" > "$OUT/$name.header.txt" || result=1
    xcrun otool -l "$OUT/$name.o" > "$OUT/$name.loadcommands.txt" || result=1
    xcrun otool -rv "$OUT/$name.o" > "$OUT/$name.relocations.txt" || result=1
    xcrun otool -tvV "$OUT/$name.o" > "$OUT/$name.disassembly.txt" || result=1
    xcrun nm -m "$OUT/$name.o" > "$OUT/$name.symbols.txt" || result=1
  fi
  return "$result"
}
if compile_variant A; then A_OK=1; else A_OK=0; fi
if compile_variant B -include "$ROOT/unstable-off.h"; then B_OK=1; else B_OK=0; fi
echo "actual A=$A_OK diagnostic B=$B_OK"
if [ -f "$OUT/A.i" ] && [ -f "$OUT/B.i" ]; then
  if diff -u "$OUT/A.i" "$OUT/B.i" > "$OUT/preprocessed.diff"; then echo 'preprocessed outputs identical'; else echo 'preprocessed outputs differ'; fi
fi
if [ "$A_OK" != 1 ]; then
  cat "$OUT/A.obj.stderr"
  fail 'actual Candidate A did not compile; B is diagnostic only'
fi
xcrun libtool -static -o "$OUT/libfirehose_kernel.a" "$OUT/A.o" || fail 'archive'
xcrun lipo -info "$OUT/libfirehose_kernel.a" > "$OUT/archive-arch.txt" || fail 'lipo'
xcrun ar -t "$OUT/libfirehose_kernel.a" > "$OUT/archive-members.txt" || fail 'ar'
xcrun nm -m "$OUT/libfirehose_kernel.a" > "$OUT/archive-symbols.txt" || fail 'nm'
xcrun otool -l "$OUT/libfirehose_kernel.a" > "$OUT/archive-loadcommands.txt" || fail 'load commands'
shasum -a 256 "$OUT/libfirehose_kernel.a" > "$OUT/archive-sha256.txt" || fail 'archive SHA256'
echo "source=$HEAD target=arm64e-apple-ios26.0 sdk=$IOS_SDK_VER workflow=$GITHUB_SHA" > "$OUT/manifest.txt"
echo 'Actual A archive produced; final link, PAC semantics and kernel ABI remain unverified.'
