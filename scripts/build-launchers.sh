#!/usr/bin/env bash
set -euo pipefail

readelf="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
for transport in classic no-connect; do
  build_dir="build/native-$transport"
  cmake -S native -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=none \
    -DNAIVEFOX_PACKAGE_ROOT="$RUNTIME_ROOT" \
    -DNAIVEFOX_PLUGIN_TRANSPORT="$transport"
  cmake --build "$build_dir" --target naivefox_launcher --verbose
  launcher="$build_dir/naivefox_launcher"
  test -x "$launcher"
  "$readelf" --file-header "$launcher" >"$RUNNER_TEMP/launcher-header.txt"
  grep -Eq '^  Class:[[:space:]]+ELF64$' "$RUNNER_TEMP/launcher-header.txt"
  grep -Eq '^  Type:[[:space:]]+DYN' "$RUNNER_TEMP/launcher-header.txt"
  grep -Eq '^  Machine:[[:space:]]+AArch64$' "$RUNNER_TEMP/launcher-header.txt"
  "$readelf" --program-headers "$launcher" | grep -F '/system/bin/linker64'
  "$readelf" --dynamic "$launcher" | grep -F 'Shared library: [libz.so]'
  python3 scripts/stage_apk_inputs.py \
    --runtime-root "$RUNTIME_ROOT" \
    --launcher "$launcher" \
    --output "build/plugin-inputs/$transport"
done
