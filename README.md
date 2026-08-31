# NaiveFox Android Plugin for Exclave

An ARM64 Android plugin that lets
[Exclave](https://github.com/ExclaveNetwork/Exclave) run
[NaiveFox](https://github.com/incident201/naivefox) as the standard native
`naive-plugin`. This project does not build Firefox or NaiveFox from source.
On every build, GitHub Actions downloads the latest non-prerelease Android
embedded runtime from NaiveFox releases, verifies it, and packages it into an
three installable APKs, one for each transport.

The plugin supports `arm64-v8a` devices running Android API 26 or newer.

## Choose a transport

- **Classic** (`*-classic-arm64-v8a.apk`): standard Naive-compatible CONNECT,
  for example Caddy with `forwardproxy@naive`.
- **No-connect** (`*-no-connect-arm64-v8a.apk`): NaiveFox's GET/POST transport;
  requires the [NaiveFox transport server module](https://github.com/incident201/naivefox-transport).
  An ordinary NaiveProxy server alone cannot serve this transport. There is no
  automatic fallback to classic.
- **No-connect Hybrid** (`*-no-connect-hybrid-arm64-v8a.apk`): experimental
  H2/H3 startup followed by one native WebSocket over TLS/TCP per carrier. It
  requires matching hybrid server support and TCP access even after H3 startup;
  it is not WebSocket over QUIC and does not fall back to another transport.

All variants keep the same `naive-plugin` id, Android package ID, and permanent
signing key. Install only one variant. Installing another APK from the same
or a newer build switches transport in place, without uninstalling or recreating
the Exclave profile. Android rejects downgrades to older build version codes.

The APK determines the transport. `https://` and `quic://` in the Exclave
profile still select H2 and H3 independently; all transports use the existing
proxy URI username and password. See the upstream
[transport contract](https://github.com/incident201/naivefox/blob/naivefox-minimal-source/netwerk/naivefox/NO-CONNECT.md).

## How it works

The plugin implements Exclave's current native plugin contract and follows the
discovery contract used by the original
[NaiveProxy APK](https://github.com/klzgrad/naiveproxy/tree/master/apk):

1. The APK registers an exported `ContentProvider` with the
   `io.nekohasekai.sagernet.plugin.ACTION_NATIVE_PLUGIN` action and
   `io.nekohasekai.sagernet.plugin.id=naive-plugin` metadata.
2. Exclave receives the extracted ARM64 launcher from the provider APK's
   `nativeLibraryDir`, using the same executable fast path as the original
   NaiveProxy plugin. This is required on current Android releases, whose
   SELinux policy does not allow executing a binary copied into app data.
3. On startup, the launcher reads the signed runtime assets from its own APK
   and extracts the complete manifest-driven package into a private temporary
   directory owned by Exclave. The resulting layout is:

   ```text
   .naivefox-runtime-*/
     manifest.json
     include/NaiveFoxAPI.h
     lib/arm64-v8a/
       libxul.so
       omni.ja
       ...every other file listed by manifest.json
   ```

4. Exclave runs `naive-plugin <config-file-path>` and passes its environment,
   including `SSL_CERT_FILE`.
5. The launcher leaves the JSON bytes unchanged and passes the APK variant as
   the fourth `NaiveFoxRunEmbedded` argument (`classic`, `no-connect`, or
   `no-connect-hybrid`). This explicitly overrides any JSON transport using
   NaiveFox's public ABI. It creates a temporary writable Gecko profile,
   re-executes itself with the extracted
   runtime on `LD_LIBRARY_PATH`, loads `libxul.so`, and calls
   `NaiveFoxRunEmbedded`.
6. The launcher translates `SIGTERM`, used by Exclave to stop a plugin process,
   into `NaiveFoxRequestStop`. It removes the temporary profile after a normal
   runtime shutdown, then removes both temporary directories.

The launcher does not contain a fixed list of shared libraries. It extracts
every runtime asset under the package prefix, while file names, sizes, and
hashes are verified against `manifest.json` during the build. NaiveFox itself
parses and validates the config. The plugin does not implement either proxy
transport. `NaiveFoxRunEmbedded` itself parses and validates the unchanged
Exclave JSON.

## GitHub Actions build

This repository intentionally does not use a local Android build. The workflow
has no push, pull-request, tag, or scheduled trigger: it runs only through
`workflow_dispatch`.

Run [`build-apk.yml`](.github/workflows/build-apk.yml) manually:

```bash
gh workflow run build-apk.yml --repo incident201/naivefox-android-plugin --ref main
gh run watch --repo incident201/naivefox-android-plugin --exit-status
```

On a clean `ubuntu-24.04` runner, the workflow:

- queries the GitHub API endpoint
  `repos/incident201/naivefox/releases/latest`;
- finds exactly one matching `*-android-aarch64.tar.xz` archive and its
  `.sha256` asset;
- verifies the archive checksum before extraction;
- rejects unsafe archive entries and verifies every manifest path, hash, size,
  and mode;
- verifies that the downloaded header exposes the four-argument embedded ABI;
- builds three launchers with fixed transport arguments using Android NDK r29 for API 26;
- runs Android lint and builds the Classic, No-connect, and No-connect Hybrid release flavors;
- verifies signing, zip alignment, package/provider/plugin metadata, ABI/ELF,
  transport metadata and launcher markers, and the complete runtime payload in each APK;
- uploads `naivefox-plugin-<latest-release-tag>-<transport>-arm64-v8a` artifacts,
  each containing an APK, its SHA-256 file, and `build-metadata-<transport>.json`;
- creates or updates the GitHub Release
  `naivefox-plugin-<latest-release-tag>-<plugin-commit>` with all nine files attached,
  only after all three APKs pass verification.

No concrete NaiveFox version or release tag is stored in the repository. A
rebuild always follows the latest compatible release available at that time.

## Installation and use

1. Download the desired transport APK from the GitHub Release created by a successful manual
   workflow run (the Actions artifact is also retained for 30 days).
2. On older Exclave/SagerNet forks, uninstall the original NaiveProxy plugin
   first. Installing two applications that both publish the `naive-plugin` id
   can make provider selection ambiguous.
3. Install the APK on an ARM64 device running Android 8.0 or newer.
4. Use an ordinary Naive profile in Exclave and connect. Exclave requires no
   changes; transport selection is supplied by the installed APK.

Release APKs are signed with the project's persistent release key. Once a
release-signed APK is installed, later releases can update it without removing
the plugin. Builds published before persistent signing was introduced used
ephemeral CI debug keys and must be uninstalled once before this migration.

## License

The plugin source is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE).
The NaiveFox runtime is downloaded only in CI and is not stored in this
repository. Its source and public API are licensed under MPL-2.0. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
