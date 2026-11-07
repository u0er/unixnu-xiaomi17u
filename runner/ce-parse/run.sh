#!/bin/bash
set -euo pipefail
work="$RUNNER_TEMP/ce-parse-work"
evidence="$RUNNER_TEMP/ce-parse-evidence"
mkdir -p "$work" "$evidence"
exec > >(tee "$evidence/runner.log") 2>&1
sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
clang="$(xcrun --find clang)"
xcodebuild -version
"$clang" --version
echo "sdk=$sdk"
test "$(xcodebuild -version | sed -n '1p')" = 'Xcode 26.5'
test "$(xcodebuild -version | sed -n '2p')" = 'Build version 17F42'
/usr/libexec/PlistBuddy -c 'Print :Version' "$sdk/SDKSettings.plist"
/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$sdk/SDKSettings.plist" || true
curl -fL --retry 2 -o "$work/kdk.dmg" https://github.com/dortania/KdkSupportPkg/releases/download/25F71/Kernel_Debug_Kit_26.5_build_25F71.dmg
hdiutil attach -readonly -nobrowse -mountpoint "$work/mount" "$work/kdk.dmg"
trap 'hdiutil detach "$work/mount" || true' EXIT
pkg="$(find "$work/mount" -name KernelDebugKit.pkg -type f -print -quit)"
test -n "$pkg"
pkgutil --check-signature "$pkg" | tee "$evidence/pkg-signature.log"
xar -xf "$pkg" -C "$work" 'KDK.pkg/Payload' 'KDK_SDK.pkg/Payload'
python3 "$GITHUB_WORKSPACE/runner/ce-parse/extract.py" "$work/KDK.pkg/Payload" "$work/KDK_SDK.pkg/Payload" "$work/headers"
mkdir -p "$work/frozen"
tar -xzf "$GITHUB_WORKSPACE/frozen-headers/ci/firehose-apple-clang/candidate-headers.tar.gz" -C "$work/frozen"
frozen="$work/frozen/.cache/fakeroot/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders"
for item in arm64/proc_reg.h pexpert/arm64/board_config.h pexpert/arm64/BCM2837.h; do
  test -f "$frozen/$item"
  mkdir -p "$work/patched/$(dirname "$item")"
  cp "$frozen/$item" "$work/patched/$item"
  echo "frozen candidate header: $item"
done
python3 "$GITHUB_WORKSPACE/runner/ce-parse/run-tests.py" "$GITHUB_WORKSPACE/xnu" "$work/headers" "$work" "$evidence" "$sdk" "$clang" "$GITHUB_WORKSPACE/runner/ce-parse/record.json"
