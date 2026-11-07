#!/bin/bash
set -euo pipefail
work="$RUNNER_TEMP/native-replay"
ev="$RUNNER_TEMP/native-replay-evidence"
xnu="$GITHUB_WORKSPACE/xnu"
parent="$GITHUB_WORKSPACE/parent"
mkdir -p "$work" "$ev"
exec > >(tee "$ev/runner.log") 2>&1
set -x
sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
clang="$(xcrun --find clang)"
xcodebuild -version
"$clang" --version
test "$(xcrun --sdk iphoneos --show-sdk-version)" = 26.5
test "$(xcrun --sdk iphoneos --show-sdk-build-version)" = 23F73
test "$(git -C "$xnu" rev-parse HEAD)" = ac9718fb1af618d5ce8678d0dc6e8a58f252216f
test "$(git -C "$parent" rev-parse HEAD)" = dc98673cad1d7741ca03cea979c577bb81bc58db
patchdir="$parent/patches/xnu-12377"
for item in bcm2837-machine-config local-availability cxx-flag-dependencies external-archive-dependencies public-entitlements-header kernel-sdk-root source-machine-layer; do
  shasum -a 256 "$patchdir/$item.patch"
  git -C "$xnu" apply --check "$patchdir/$item.patch"
  git -C "$xnu" apply "$patchdir/$item.patch"
done
for item in public-missing-definitions-policy; do
  path="$GITHUB_WORKSPACE/runner/device/patches/xnu/$item.patch"
  shasum -a 256 "$path"
  git -C "$xnu" apply --check "$path"
  git -C "$xnu" apply "$path"
done
for item in source-corecrypto14-bootstrap source-pthread-bootstrap; do
  shasum -a 256 "$patchdir/$item.patch"
  git -C "$xnu" apply --check "$patchdir/$item.patch"
  git -C "$xnu" apply "$patchdir/$item.patch"
done
path="$GITHUB_WORKSPACE/runner/device/patches/xnu/native-arm-pan-independent.patch"
shasum -a 256 "$path"
git -C "$xnu" apply --check "$path"
git -C "$xnu" apply "$path"
tar -xzf "$GITHUB_WORKSPACE/frozen-headers/ci/firehose-apple-clang/candidate-headers.tar.gz" -C "$work"
test "$(git -C "$GITHUB_WORKSPACE/availability" rev-parse HEAD)" = 149b1777f3e8c2133042d8e188f72e3adf6a7110
mkdir -p "$work/availability-obj" "$work/.cache/fakeroot"
make -C "$GITHUB_WORKSPACE/availability" install -j4 \
  OBJROOT="$work/availability-obj" SYMROOT="$work/availability-obj" \
  DSTROOT="$work/.cache/fakeroot" CMAKE="$(command -v cmake)" NINJA="$(command -v ninja)"
export XNU_AVAILABILITY_PL="$work/.cache/fakeroot/usr/local/libexec/availability.pl"
test -x "$XNU_AVAILABILITY_PL"
export SRCROOT="$xnu" OBJROOT="$work/obj" SYMROOT="$work/sym" DSTROOT="$work/dst"
mkdir -p "$OBJROOT" "$SYMROOT" "$DSTROOT"
for file in config/MASTER config/MASTER.arm64 config/MASTER.arm64.bcm2837 osfmk/conf/files osfmk/conf/files.arm64 bsd/conf/files bsd/conf/files.arm64; do
  shasum -a 256 "$xnu/$file"
done
args=(SDKROOT="$sdk" KERNEL_SDKROOT="$work/.cache/fakeroot" ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=BCM2837 KERNEL_CONFIGS=DEVELOPMENT
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY
  ARM64_LINK_ADDR=0xfffffff007004000 ARM64_PLKSEG_ADDR=0xfffffff004004000
  'DEPLOYMENT_TARGET_FLAGS=-miphoneos-version-min=26.0 -DXNU_TARGET_OS_IOS'
  RC_DARWIN_KERNEL_VERSION=25.5.0 RC_ProjectSourceVersion=12377.121.6
  BUILD_LTO=0 BOUND_CHECKS=0 BUILD_WERROR=0 MAKEJOBS=-j4)
echo 'CONFIGURATION ONLY: setup/exporthdrs, no build or config_all target'
make -C "$xnu" setup "${args[@]}"
make -C "$xnu" exporthdrs "${args[@]}"
test -x "$OBJROOT/SETUP/config/config"
for component in osfmk bsd; do
  out="$OBJROOT/DEVELOPMENT_ARM64_BCM2837/$component/DEVELOPMENT"
  mkdir -p "$out"
  "$xnu/SETUP/config/doconf" -c -cpu arm64 -soc bcm2837 -platform iPhoneOS \
    -d "$out" -s "$xnu/$component/conf" -m "$xnu/config" DEVELOPMENT
  test -s "$out/Makefile"
  test -s "$out/meta_features.h"
  cp "$out/Makefile" "$ev/$component-Makefile.txt"
  cp "$out/meta_features.h" "$ev/$component-meta-features.txt"
  shasum -a 256 "$out/Makefile" "$out/meta_features.h"
done
echo 'Native config generated; preprocessing must use actual generated commands and complete KDK/header closure.'
echo 'NOT_RUN: no verified ubc_subr compile command or complete original runner output.' > "$ev/preprocess-status.txt"
