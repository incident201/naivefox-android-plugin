#!/usr/bin/env python3
"""Verify that an APK contains the exact staged NaiveFox plugin payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import zipfile
from pathlib import Path, PurePosixPath

from verify_runtime import verify_runtime


LAUNCHER_APK_PATH = "lib/arm64-v8a/libnaivefox_launcher.so"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--launcher", type=Path, required=True)
    arguments = parser.parse_args()

    apk = arguments.apk.resolve(strict=True)
    runtime_root = arguments.runtime_root.resolve(strict=True)
    launcher = arguments.launcher.resolve(strict=True)
    verification = verify_runtime(runtime_root)
    manifest = json.loads((runtime_root / "manifest.json").read_text(encoding="utf-8"))

    with zipfile.ZipFile(apk) as package:
        names = set(package.namelist())
        native_entries = sorted(
            name for name in names if name.startswith("lib/") and not name.endswith("/")
        )
        if native_entries != [LAUNCHER_APK_PATH]:
            parser.error(f"unexpected APK native entries: {native_entries}")

        launcher_bytes = package.read(LAUNCHER_APK_PATH)
        expected_launcher = launcher.read_bytes()
        if launcher_bytes != expected_launcher:
            parser.error("APK launcher differs from the verified NDK output")

        manifest_apk_path = "assets/plugin/runtime/manifest.json"
        if package.read(manifest_apk_path) != (runtime_root / "manifest.json").read_bytes():
            parser.error("APK runtime manifest differs from the verified source")

        expected_assets = {manifest_apk_path}
        for item in manifest["files"]:
            relative = PurePosixPath(item["path"])
            apk_path = "assets/plugin/runtime/" + relative.as_posix()
            expected_assets.add(apk_path)
            data = package.read(apk_path)
            if package.getinfo(apk_path).compress_type not in (
                zipfile.ZIP_STORED,
                zipfile.ZIP_DEFLATED,
            ):
                parser.error(f"unsupported APK runtime compression: {relative}")
            if len(data) != item["size"]:
                parser.error(f"APK runtime size mismatch: {relative}")
            if digest(data) != item["sha256"]:
                parser.error(f"APK runtime SHA-256 mismatch: {relative}")

        actual_runtime_assets = {
            name
            for name in names
            if name.startswith("assets/plugin/runtime/") and not name.endswith("/")
        }
        if actual_runtime_assets != expected_assets:
            parser.error(
                "APK runtime file set mismatch: "
                f"missing={sorted(expected_assets - actual_runtime_assets)}, "
                f"extra={sorted(actual_runtime_assets - expected_assets)}"
            )

    print(
        "Verified APK payload: "
        f"runtime={verification['version']} files={verification['file_count']} "
        f"launcher_sha256={digest(expected_launcher)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
