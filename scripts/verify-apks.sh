#!/usr/bin/env bash
set -euo pipefail

readelf="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
for transport in classic no-connect no-connect-hybrid; do
  case "$transport" in
    classic) flavor=classic; label='NaiveFox Plugin — Classic' ;;
    no-connect) flavor=noConnect; label='NaiveFox Plugin — No-connect' ;;
    no-connect-hybrid) flavor=noConnectHybrid; label='NaiveFox Plugin — No-connect Hybrid' ;;
  esac
  source_apk="app/build/outputs/apk/$flavor/release/app-$flavor-release.apk"
  test -f "$source_apk"
  mkdir -p "dist/$transport"
  apk="dist/$transport/naivefox-plugin-${RUNTIME_RELEASE_TAG}-${transport}-arm64-v8a.apk"
  cp "$source_apk" "$apk"

  "$ANDROID_BUILD_TOOLS_HOME/apksigner" verify --verbose --print-certs "$apk" \
    | tee "$RUNNER_TEMP/apk-signing.txt"
  signing_digest=$(sed -n 's/^Signer #1 certificate SHA-256 digest: //p' "$RUNNER_TEMP/apk-signing.txt")
  test "$signing_digest" = "$ANDROID_SIGNING_CERT_SHA256"
  ! grep -q '^Signer #2 certificate' "$RUNNER_TEMP/apk-signing.txt"
  "$ANDROID_BUILD_TOOLS_HOME/zipalign" -c -P 16 -v 4 "$apk"
  "$ANDROID_BUILD_TOOLS_HOME/aapt2" dump badging "$apk" >"$RUNNER_TEMP/apk-badging.txt"
  grep -F "package: name='com.github.incident201.naivefox.plugin'" "$RUNNER_TEMP/apk-badging.txt"
  grep -F "native-code: 'arm64-v8a'" "$RUNNER_TEMP/apk-badging.txt"
  grep -F "application-label:'$label'" "$RUNNER_TEMP/apk-badging.txt"
  test "$(apkanalyzer manifest min-sdk "$apk")" = "26"
  test "$(apkanalyzer manifest version-code "$apk")" = "$GITHUB_RUN_NUMBER"
  test "$(apkanalyzer manifest version-name "$apk")" = "$RUNTIME_RELEASE_TAG-$transport"
  apkanalyzer manifest print "$apk" >"$RUNNER_TEMP/apk-manifest.xml"

  python3 scripts/verify_apk.py \
    --apk "$apk" \
    --runtime-root "$RUNTIME_ROOT" \
    --launcher "build/native-$transport/naivefox_launcher" \
    --transport "$transport" \
    --android-manifest "$RUNNER_TEMP/apk-manifest.xml"

  unzip -p "$apk" lib/arm64-v8a/libnaivefox_launcher.so >"$RUNNER_TEMP/apk-launcher"
  "$readelf" --file-header "$RUNNER_TEMP/apk-launcher" >"$RUNNER_TEMP/apk-launcher-header.txt"
  grep -Eq '^  Type:[[:space:]]+DYN' "$RUNNER_TEMP/apk-launcher-header.txt"
  grep -Eq '^  Machine:[[:space:]]+AArch64$' "$RUNNER_TEMP/apk-launcher-header.txt"
  (
    cd "dist/$transport"
    sha256sum "$(basename "$apk")" >"$(basename "$apk").sha256"
  )
  jq -n \
    --arg transport "$transport" \
    --arg release_tag "$RUNTIME_RELEASE_TAG" \
    --arg runtime_version "$RUNTIME_VERSION" \
    --arg archive "$RUNTIME_ARCHIVE" \
    --arg archive_sha256 "$RUNTIME_ARCHIVE_SHA256" \
    --arg source_sha "$GITHUB_SHA" \
    --arg signing_cert_sha256 "$ANDROID_SIGNING_CERT_SHA256" \
    --argjson version_code "$GITHUB_RUN_NUMBER" \
    '{transport: $transport, release_tag: $release_tag, runtime_version: $runtime_version, source_archive: $archive, source_archive_sha256: $archive_sha256, plugin_source_sha: $source_sha, signing_certificate_sha256: $signing_cert_sha256, version_code: $version_code}' \
    >"dist/$transport/build-metadata-$transport.json"
done
