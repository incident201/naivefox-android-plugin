# NaiveFox Android Plugin for Exclave

An ARM64 Android plugin that lets
[Exclave](https://github.com/ExclaveNetwork/Exclave) run
[NaiveFox](https://github.com/incident201/naivefox) as the standard native
`naive-plugin`. This project does not build Firefox or NaiveFox from source.
On every build, GitHub Actions downloads the latest non-prerelease Android
embedded runtime from NaiveFox releases, verifies it, and packages it into an
installable APK.

The plugin supports `arm64-v8a` devices running Android API 26 or newer.

## How it works

The plugin implements Exclave's current native plugin contract and follows the
discovery contract used by the original
[NaiveProxy APK](https://github.com/klzgrad/naiveproxy/tree/master/apk):

1. The APK registers an exported `ContentProvider` with the
   `io.nekohasekai.sagernet.plugin.ACTION_NATIVE_PLUGIN` action and
   `io.nekohasekai.sagernet.plugin.id=naive-plugin` metadata.
2. The single-file executable fast path is deliberately not advertised.
   Exclave uses the `NativePluginProvider`/`PathProvider` slow path: it requests
   the file list, copies every file into its own `noBackupFilesDir/plugin`
   directory, and applies the published file modes.
3. After Exclave copies the files, the layout is:

   ```text
   plugin/
     naive-plugin                         # ARM64 PIE launcher, mode 0755
     runtime/
       manifest.json
       include/NaiveFoxAPI.h
       lib/arm64-v8a/
         libxul.so
         omni.ja
         ...every other file listed by manifest.json
   ```

4. Exclave runs `naive-plugin <config-file-path>` and passes its environment,
   including `SSL_CERT_FILE`.
5. The launcher reads the original JSON without translating it, creates a
   temporary writable Gecko profile, re-executes itself with the runtime on
   `LD_LIBRARY_PATH`, loads `libxul.so`, and calls `NaiveFoxRunEmbedded`.
6. The launcher translates `SIGTERM`, used by Exclave to stop a plugin process,
   into `NaiveFoxRequestStop`. It removes the temporary profile after a normal
   runtime shutdown.

The provider does not contain a fixed list of shared libraries. File names,
paths, modes, sizes, and hashes come from the verified `manifest.json` in the
current release package. NaiveFox itself parses the unmodified Exclave config.

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
- builds the launcher with Android NDK r29 for Android API 26;
- runs Android lint and builds a release-mode APK with Gradle;
- verifies signing, zip alignment, package/provider/plugin metadata, ABI/ELF,
  and the complete runtime payload inside the APK;
- uploads `naivefox-plugin-<latest-release-tag>-arm64-v8a`, containing the APK,
  its SHA-256 file, and build metadata.
- creates or updates the GitHub Release
  `naivefox-plugin-<latest-release-tag>` with the same three files attached.

No concrete NaiveFox version or release tag is stored in the repository. A
rebuild always follows the latest compatible release available at that time.

## Installation and use

1. Download the APK from the GitHub Release created by a successful manual
   workflow run (the Actions artifact is also retained for 30 days).
2. On older Exclave/SagerNet forks, uninstall the original NaiveProxy plugin
   first. Installing two applications that both publish the `naive-plugin` id
   can make provider selection ambiguous.
3. Install the APK on an ARM64 device running Android 8.0 or newer.
4. Create an ordinary Naive profile in Exclave and connect. Exclave and its
   generated config require no changes.

Until signing secrets are configured, the release-mode APK is signed with a CI
debug key created during the build. The APK is installable, but an artifact from
a different run can have a different signature. Android may therefore require
the previously installed plugin to be removed before installing the new APK.

## Verification boundary

GitHub Actions verifies a clean build, dynamic release selection, checksums,
manifest-driven packaging, the ARM64 launcher, and Android plugin metadata. The
following integration tests still require a real ARM64 Android device with
Exclave installed:

- discovery of the APK as `naive-plugin`;
- H2 (`https://`) and H3 (`quic://`) traffic;
- SOCKS5 listener authentication;
- `extra-headers`, `host-resolver-rules`, `no-post-quantum`, and an empty side
  of upstream credentials;
- a custom CA passed through `SSL_CERT_FILE`;
- graceful shutdown within Exclave's interval before it sends `SIGKILL`;
- removal of the temporary profile after a normal disconnect.

## License

The plugin source is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE).
The NaiveFox runtime is downloaded only in CI and is not stored in this
repository. Its source and public API are licensed under MPL-2.0. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
