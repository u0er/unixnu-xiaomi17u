#!/usr/bin/env bash
set -euo pipefail

workspace=$GITHUB_WORKSPACE
inputs="$RUNNER_TEMP/candidate-a-inputs"
evidence="$RUNNER_TEMP/candidate-a-evidence"
output="$RUNNER_TEMP/candidate-a-output"
mkdir -p "$inputs" "$evidence"
exec > >(tee "$evidence/runner.log") 2>&1
finish() {
  local result=$?
  local tree="$output/nezha/ipados-26.5-23F77-nezha/nezha/tree"
  local bundle="$inputs/firehose-repository/ci/firehose-apple-clang/candidate-headers.tar.gz"
  local object_root="$tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
  if [ -d "$object_root" ]; then
    find "$object_root" -type f \( -name '*.o' -o -name '*.cpo' \) -print > "$evidence/compiled-objects.txt"
    mkdir -p "$evidence/sample-objects"
    local count=0 object
    while IFS= read -r object && [ "$count" -lt 8 ]; do
      count=$((count + 1))
      cp "$object" "$evidence/sample-objects/$count-$(basename "$object")"
      if ! /usr/bin/xcrun otool -hv "$object" >> "$evidence/sample-object-headers.txt" 2>&1; then
        echo "Mach-O inspection failed: $object" >&2
        result=1
      fi
    done < "$evidence/compiled-objects.txt"
  fi
  if [ -d "$tree/.cache/fakeroot" ] && [ -f "$bundle" ]; then
    if ! python3 -B "$workspace/runner/compare-headers.py" "$bundle" "$tree" "$evidence/header-differences.json"; then
      echo 'Frozen header diagnostic failed' >&2
      result=1
    fi
  fi
  exit "$result"
}
trap finish EXIT
python3 -B "$workspace/runner/find-coreentitlements.py"

# The source tree is pinned by the workflow checkout. Only the listed local
# candidate files are overlaid; no SDK, IPSW or device image is published.
cp -R "$workspace/runner/overlay/." "$workspace/source/"
cp -R "$workspace/runner/device" "$inputs/device"
cp -R "$workspace/runner/frozen" "$inputs/frozen"
cp -R "$workspace/runner/soc" "$inputs/soc"

fetch_pin() {
  local url=$1 revision=$2 destination=$3 sparse=${4:-}
  git init -q "$destination"
  git -C "$destination" remote add origin "$url"
  if [ "$sparse" = profile ]; then
    git -C "$destination" sparse-checkout set --cone compiler-rt/lib/profile compiler-rt/include/profile
  fi
  git -C "$destination" -c protocol.version=2 fetch -q --filter=blob:none --depth=1 --no-tags origin "$revision"
  git -C "$destination" checkout -q --detach FETCH_HEAD
  test "$(git -C "$destination" rev-parse HEAD)" = "$revision"
  printf '%s %s\n' "$destination" "$revision" >> "$evidence/pinned-sources.txt"
}

fetch_pin https://github.com/apple-oss-distributions/xnu.git ac9718fb1af618d5ce8678d0dc6e8a58f252216f "$inputs/xnu"
fetch_pin https://github.com/apple-oss-distributions/libplatform.git b7ed7cf5cf7dd12b98672435db2225a860f199d8 "$inputs/libplatform"
fetch_pin https://github.com/apple-oss-distributions/libdispatch.git 701f4d1a24ae9c6863901bbbb22624b7d1b87321 "$inputs/libdispatch"
fetch_pin https://github.com/apple-oss-distributions/AvailabilityVersions.git 149b1777f3e8c2133042d8e188f72e3adf6a7110 "$inputs/AvailabilityVersions"
fetch_pin https://github.com/apple-oss-distributions/libpthread.git 1f4f5265b319111142f1bf3a27d4484ef5a98314 "$inputs/libpthread"
fetch_pin https://github.com/ONLYA/corecrypto-2023.git 11fba65b12d8830630e248dfa8344caf8b065f26 "$inputs/corecrypto-2023"
fetch_pin https://github.com/apple-oss-distributions/IOStorageFamily.git 7edb88fbae296fb7c8ce2f64e115e116e566d51c "$inputs/IOStorageFamily"
fetch_pin https://github.com/UniXNU/ArmCpu.git 93a5030c5d983d0ef90f0fca93879d6e36ad1ba8 "$inputs/ArmCpu"
fetch_pin https://github.com/UniXNU/ArmDTPlatform.git 417ff95d6a6e8607ddea9309ec45c3bfd4a67c60 "$inputs/ArmDTPlatform"
fetch_pin https://github.com/u0er/unixnu-xiaomi17u.git 49a37562c9c1e57963d558d4c322036602e3280e "$inputs/firehose-repository"
mkdir -p "$inputs/cc-sources"
fetch_pin https://github.com/llvm/llvm-project.git 58c46bae21181fb4240cf964461bea738d96d50e "$inputs/cc-sources/llvm-project" profile
fetch_pin https://github.com/apple-oss-distributions/Libc.git 899a3b2d52d95d75e05fb286a5e64975ec3de757 "$inputs/cc-sources/Libc"
export UNIXNU_CC_KEXT_SOURCE_ROOT="$inputs/cc-sources"

cd "$workspace/source"
printf 'EXAMPLE_CONFIG / NOT_DEVICE_VALIDATED\n' | tee "$evidence/status.txt"
printf 'runner=%s image=%s\n' "${RUNNER_OS:-unknown}" "${ImageVersion:-unknown}" > "$evidence/environment.txt"
printf 'source=%s\n' "$(git rev-parse HEAD)" >> "$evidence/environment.txt"
python3 -B ./unixnu build kernel --mode xnu-only -D "$inputs/device" --out "$output" \
  --local-module "soc:sm8850-ac=$inputs/soc" \
  --local-module "driver:ArmCpu=$inputs/ArmCpu" \
  --local-module "driver:ArmDTPlatform=$inputs/ArmDTPlatform" \
  --local-module "refer:xnu=$inputs/xnu" \
  --local-module "refer:libplatform=$inputs/libplatform" \
  --local-module "refer:libdispatch=$inputs/libdispatch" \
  --local-module "refer:AvailabilityVersions=$inputs/AvailabilityVersions" \
  --local-module "refer:libpthread=$inputs/libpthread" \
  --local-module "refer:corecrypto-2023=$inputs/corecrypto-2023" \
  --local-module "refer:IOStorageFamily=$inputs/IOStorageFamily"
