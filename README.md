# NaiveFox Android Plugin for Exclave

Android ARM64-плагин, который позволяет Exclave запускать
[NaiveFox](https://github.com/incident201/naivefox) как обычный native
`naive-plugin`. Проект не собирает Firefox или NaiveFox из исходников: GitHub
Actions каждый раз берёт latest non-prerelease Android embedded runtime из
релизов NaiveFox, проверяет его и упаковывает в устанавливаемый APK.

Поддерживается только `arm64-v8a` и Android API 26+.

## Как это работает

Плагин реализует актуальный native plugin contract
[Exclave](https://github.com/ExclaveNetwork/Exclave) и совместим с discovery
оригинального APK из
[NaiveProxy](https://github.com/klzgrad/naiveproxy/tree/master/apk):

1. APK регистрирует экспортируемый `ContentProvider` с action
   `io.nekohasekai.sagernet.plugin.ACTION_NATIVE_PLUGIN` и metadata
   `io.nekohasekai.sagernet.plugin.id=naive-plugin`.
2. Single-file fast path намеренно не публикуется. Exclave использует slow path
   `NativePluginProvider`/`PathProvider`: запрашивает список файлов, копирует их
   в собственный `noBackupFilesDir/plugin` и применяет указанные режимы.
3. После копирования структура имеет следующий вид:

   ```text
   plugin/
     naive-plugin                         # ARM64 PIE launcher, mode 0755
     runtime/
       manifest.json
       include/NaiveFoxAPI.h
       lib/arm64-v8a/
         libxul.so
         omni.ja
         ...все остальные файлы из manifest.json
   ```

4. Exclave запускает `naive-plugin <config-file-path>` и передаёт ему environment,
   включая `SSL_CERT_FILE`.
5. Launcher без изменения читает JSON, создаёт временный writable Gecko profile,
   делает linker re-exec с runtime в `LD_LIBRARY_PATH`, загружает `libxul.so` и
   вызывает `NaiveFoxRunEmbedded`.
6. `SIGTERM`, которым Exclave останавливает plugin process, преобразуется в
   `NaiveFoxRequestStop`. После штатного возврата runtime временный profile
   удаляется.

Provider не содержит фиксированного списка `.so`: имена, пути и modes берутся из
проверенного `manifest.json` текущего release package. Config не преобразуется —
его разбирает сам NaiveFox.

## Сборка в GitHub Actions

Локальная Android-сборка для этого репозитория не используется. Workflow
[`build-apk.yml`](.github/workflows/build-apk.yml) запускается вручную:

```bash
gh workflow run build-apk.yml --repo incident201/naivefox-android-plugin --ref main
gh run watch --repo incident201/naivefox-android-plugin --exit-status
```

Workflow на чистом `ubuntu-24.04` runner:

- запрашивает GitHub API `repos/incident201/naivefox/releases/latest`;
- находит единственную пару `*-android-aarch64.tar.xz` и `.sha256`;
- проверяет archive checksum до распаковки;
- запрещает небезопасные tar entries и проверяет все manifest hashes/modes;
- компилирует launcher NDK r29 для Android API 26;
- выполняет Android lint и собирает release-mode APK через Gradle;
- проверяет подпись, zip alignment, manifest/plugin id, ABI/ELF и полный runtime
  внутри APK;
- загружает artifact
  `naivefox-plugin-<latest-release-tag>-arm64-v8a` с APK, SHA-256 и metadata.

Конкретная версия или tag NaiveFox в репозитории не зашиты. Для повторной сборки
того же runtime необходимо, чтобы он по-прежнему был latest release; workflow по
замыслу всегда следует latest compatible package.

## Установка и использование

1. Скачайте APK из успешного GitHub Actions artifact.
2. На старых Exclave/SagerNet forks предварительно удалите оригинальный
   NaiveProxy plugin. Одновременная установка двух приложений с id
   `naive-plugin` может давать неоднозначный выбор provider.
3. Установите APK на ARM64-устройство с Android 8.0 или новее.
4. Создайте обычный Naive profile в Exclave и подключитесь. Изменять Exclave или
   его config не требуется.

Пока в репозитории не настроены signing secrets, release-mode APK подписывается
автоматически созданным CI debug key. APK устанавливаемый, но APK из другого run
может иметь другую подпись; тогда перед обновлением потребуется удалить ранее
установленный plugin.

## Границы проверки

GitHub Actions доказывает clean build, динамический выбор release, checksums,
manifest-driven упаковку, ARM64 launcher и корректный Android plugin manifest.
На реальном ARM64 Android device с установленным Exclave дополнительно нужно
проверить:

- обнаружение APK как `naive-plugin`;
- H2 (`https://`) и H3 (`quic://`) трафик;
- SOCKS5 listener authentication;
- `extra-headers`, `host-resolver-rules`, `no-post-quantum` и пустую сторону
  upstream credentials;
- custom CA через `SSL_CERT_FILE`;
- штатную остановку в интервале, который Exclave оставляет до SIGKILL;
- удаление временного profile после обычного disconnect.

## Лицензия

Исходный код этого плагина распространяется по GPL-3.0-or-later, см. [LICENSE](LICENSE).
NaiveFox runtime скачивается только во время CI и не хранится в репозитории; его
код и публичный API распространяются по MPL-2.0. Дополнительная информация — в
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
