#!/usr/bin/env bash
# Build only XNU's own host helper using its pinned Makefile; never build XNU.
set -euo pipefail
src="$GITHUB_WORKSPACE/xnu-source"
out="$RUNNER_TEMP/candidate-a-replacecontents"
mkdir -p "$out/obj" "$out/home" "$out/tmp"
exec > >(tee "$out/build.log") 2>&1
set -x
test "$(git -C "$src" rev-parse HEAD)" = ac9718fb1af618d5ce8678d0dc6e8a58f252216f
test -z "$(git -C "$src" status --porcelain)"
test "$(/usr/bin/xcodebuild -version)" = $'Xcode 26.5\nBuild version 17F42'
host_sdk=$(/usr/bin/xcrun --sdk macosx --show-sdk-path)
host_cc=$(/usr/bin/xcrun --sdk macosx --find clang)
"$host_cc" --version
sw_vers
shasum -a 256 "$src/SETUP/replacecontents/replacecontents.c" "$src/SETUP/replacecontents/Makefile"
# Its only build goal is replacecontents: one host object, host executable and
# the native recipe's ad-hoc codesign. It cannot request a kernel build target.
env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME="$out/home" TMPDIR="$out/tmp" \
  DEVELOPER_DIR="$DEVELOPER_DIR" LC_ALL=C \
  /usr/bin/make -C "$out/obj" -f "$src/SETUP/replacecontents/Makefile" replacecontents \
  "SRCROOT=$src" "SOURCE=$src/SETUP/replacecontents" "OBJROOT=$out" "TARGET=$out/obj" \
  RELATIVE_SOURCE_PATH=SETUP/replacecontents CURRENT_ARCH_CONFIG=ARM64 \
  CURRENT_MACHINE_CONFIG=BCM2837 CURRENT_KERNEL_CONFIG=DEVELOPMENT \
  SDKROOT=macosx HOST_SDKROOT=macosx "HOST_CC=$host_cc" RC_XBS=YES LOGCOLORS=n
test -x "$out/obj/replacecontents"
file "$out/obj/replacecontents"
/usr/bin/otool -hv "$out/obj/replacecontents"
/usr/bin/codesign -v "$out/obj/replacecontents"
shasum -a 256 "$out/obj/replacecontents"
"$out/obj/replacecontents" "$out/result.txt" alpha beta
python3 - "$out/result.txt" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
if data != b'alpha beta\n\0':
    raise SystemExit(f'unexpected helper output: {data!r}')
print('helper invocation produced expected bytes')
PY
printf 'PASS: native XNU host helper built and invoked; no kernel object built\n'
