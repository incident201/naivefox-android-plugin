# NaiveFox Android Plugin for Exclave

An ARM64 Android plugin that lets
[Exclave](https://github.com/ExclaveNetwork/Exclave) run
[NaiveFox](https://github.com/incident201/naivefox) as the standard native
`naive-plugin`. This project does not build Firefox or NaiveFox from source.
On every build, GitHub Actions downloads the latest non-prerelease Android
embedded runtime from NaiveFox releases, verifies it, and packages it into an
installable APK.

The plugin supports `arm64-v8a` devices running Android API 26 or newer.

## Compatibility

NaiveFox uses one current application protocol with explicit delivery schemes.
Update its native runtime and the matching
[naivefox-transport](https://github.com/incident201/naivefox-transport) server
together. Older wire contracts and compatibility profiles are not supported.

| Protocol field | Delivery | Username |
| --- | --- | --- |
| https | Default packet delivery over H2, direct or through a compatible CDN | user~PIN; PIN is mandatory |
| wss | WebSocket after H2 startup | user or user~PIN; the runtime strips and ignores the suffix |
| quic | Native HTTP/3 | Ordinary authentication username |

PIN is the 64-hex SHA-256 SPKI fingerprint of the origin's dedicated inner-TLS
certificate. Keep it in the existing username field; no new JSON field or
plugin setting is needed. Enter the selected scheme in the host application's
protocol field. A host with a free-text protocol field can pass wss unchanged;
the plugin does not supply or restrict the host's protocol picker.

Leave unsupported host extensions such as extra headers and insecure
concurrency unset. NaiveFox validates the JSON itself.

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
5. The launcher leaves the JSON bytes unchanged, creates a temporary writable
   Gecko profile, re-executes itself with the extracted
   runtime on `LD_LIBRARY_PATH`, loads `libxul.so`, and calls
   the current three-argument `NaiveFoxRunEmbedded` API.
6. The launcher translates `SIGTERM`, used by Exclave to stop a plugin process,
   into `NaiveFoxRequestStop`. It removes the temporary profile after a normal
   runtime shutdown, then removes both temporary directories.

The launcher does not contain a fixed list of shared libraries. It extracts
every runtime asset under the package prefix, while file names, sizes, and
hashes are verified against `manifest.json` during the build. NaiveFox itself
parses and validates the config. The plugin does not implement the proxy
transport.

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
- verifies that the downloaded header exposes the current three-argument ABI;
- builds the launcher using Android NDK r29 for API 26;
- runs Android lint and builds one signed release APK;
- verifies signing, zip alignment, package/provider/plugin metadata, ABI/ELF,
  and the complete runtime payload inside the APK;
- uploads `naivefox-plugin-<latest-release-tag>-arm64-v8a`, containing the APK,
  its SHA-256 file, and `build-metadata.json`;
- creates or updates the GitHub Release
  `naivefox-plugin-<latest-release-tag>-<plugin-commit>` with those three files.

No concrete NaiveFox version or release tag is stored in the repository. A
rebuild always follows the latest compatible release available at that time.

## Installation and use

1. Download the APK from the GitHub Release created by a successful manual
   workflow run (the Actions artifact is also retained for 30 days).
2. On older Exclave/SagerNet forks, uninstall the original NaiveProxy plugin
   first. Installing two applications that both publish the `naive-plugin` id
   can make provider selection ambiguous.
3. Install the APK on an ARM64 device running Android 8.0 or newer.
4. Use a Naive profile in Exclave or a compatible host. For the default
   transport, select https and set the username to user~PIN. Keep the domain,
   port and password in their existing fields. The launcher forwards the
   resulting JSON unchanged.

Release APKs are signed with the project's persistent release key. Once a
release-signed APK is installed, later releases can update it without removing
the plugin. Builds published before persistent signing was introduced used
ephemeral CI debug keys and must be uninstalled once before this migration.

## License

The plugin source is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE).
The NaiveFox runtime is downloaded only in CI and is not stored in this
repository. Its source and public API are licensed under MPL-2.0. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
