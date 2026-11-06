#!/bin/bash
# Build-only symbol reference from this Nezha input bundle. No donor firmware,
# device operation, missing-import stub or POST bypass is included.
set -euo pipefail
if [ "$(uname -s)" != Darwin ]; then
    echo 'This reference requires macOS and Apple Xcode clang.' >&2
    exit 1
fi
bundle_root=$(cd "$(dirname "$0")" && pwd)
reference_out=${NEZHA_APPLE_REFERENCE_OUT:-"$bundle_root/output"}
mkdir -p "$reference_out"
clang_path=$(xcrun --sdk iphoneos --find clang)
"$clang_path" --version > "$reference_out/compiler-version.txt"
if ! head -1 "$reference_out/compiler-version.txt" | grep -q 'Apple clang version 21\.'; then
    echo 'Select the reviewed Xcode Apple clang 21 toolchain; no compiler fallback is selected.' >&2
    exit 1
fi
xcodebuild -version > "$reference_out/xcode-version.txt"
export KERNEL_SDKROOT="$bundle_root/sdk"
export XNU_AVAILABILITY_PL="$bundle_root/sdk/usr/local/libexec/availability.pl"
reference_jobs=${NEZHA_REFERENCE_JOBS:-4}
archive_root="$bundle_root/archives"
source_root="$bundle_root/xnu"
required=(libcorecrypto-namespaced.a libpthread_kernel.a libNezhaPlatform.a)
for archive in "${required[@]}"; do
    test -f "$archive_root/$archive"
done
arguments=(
    build SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=NEZHA KERNEL_CONFIGS=RELEASE
    ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY
    ARM64_LINK_ADDR=0xfffffff007004000 ARM64_PLKSEG_ADDR=0xfffffff004004000
    RC_DARWIN_KERNEL_VERSION=25.5.0 RC_ProjectSourceVersion=12377.121.6
    "OBJROOT=$reference_out/obj" "SYMROOT=$reference_out/sym" "DSTROOT=$reference_out/dst"
    "MAKEJOBS=-j$reference_jobs" BUILD_LTO=1 BOUND_CHECKS=0 BUILD_WERROR=0 BUILD_DSYM=0
    XNU_KERNEL_CHAINED_FIXUPS=1
    # The actual Linux owning objects and all three input archives encode
    # macOS 26 in LC_BUILD_VERSION, while XNU_TARGET_OS_IOS selects the kernel
    # interface. Match that real object platform in both Apple link stages;
    # mixing iOS nonlto.o with these macOS objects is rejected by Apple ld.
    # This is an analysis reference, never a user-space or boot candidate.
    'LDFLAGS_KERNEL_LTO=-mmacosx-version-min=26.0 -Wl,-mllvm,-inline-threshold=100'
    'DEPLOYMENT_TARGET_FLAGS=-mmacosx-version-min=26.0 -DXNU_TARGET_OS_IOS'
    'DEPLOYMENT_LINKER_FLAGS=-Wl,-macosx_version_min,26.0'
    'RC_NONARCH_CFLAGS=-march=armv8.3-a+crypto'
    'prng_random.o_CFLAGS_ADD=-DXNU_SOURCE_CORECRYPTO14=1'
    'OSKext.cpo_CFLAGS_ADD=-DXNU_SOURCE_CORECRYPTO14=1'
    'sleh.o_CFLAGS_ADD=-DNEZHA_ARMCPU_IRQ_DISPATCH=1'
    'LDFLAGS_KERNEL_ONLY_CONFIG_RELEASEARM64='
    "LD_KERNEL_ARCHIVES=-L$bundle_root/sdk/usr/local/lib/kernel -lfirehose_kernel $archive_root/libcorecrypto-namespaced.a -Wl,-force_load,$archive_root/libpthread_kernel.a -Wl,-force_load,$archive_root/libNezhaPlatform.a"
    "XNU_EXTERNAL_KERNEL_ARCHIVES=$archive_root/libcorecrypto-namespaced.a $archive_root/libpthread_kernel.a $archive_root/libNezhaPlatform.a"
)
printf '%s\n' "${arguments[@]}" > "$reference_out/make-arguments.txt"
if ! make -C "$source_root" "${arguments[@]}" > "$reference_out/build.log" 2>&1; then
    tail -100 "$reference_out/build.log" >&2
    exit 1
fi
python3 -B "$bundle_root/inspect-reference.py" "$reference_out"
