#!/usr/bin/env python3
"""Stage verified runtime and launcher inputs for the Android Gradle build."""

from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path

from verify_runtime import verify_runtime


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    runtime_root = arguments.runtime_root.resolve(strict=True)
    launcher = arguments.launcher.resolve(strict=True)
    if not launcher.is_file():
        parser.error(f"launcher is not a file: {launcher}")
    verification = verify_runtime(runtime_root)

    output = arguments.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    asset_root = output / "assets" / "plugin" / "runtime"
    jni_root = output / "jniLibs" / "arm64-v8a"
    asset_root.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(runtime_root, asset_root, symlinks=False)
    jni_root.mkdir(parents=True, exist_ok=True)
    staged_launcher = jni_root / "libnaivefox_launcher.so"
    shutil.copy2(launcher, staged_launcher)
    staged_launcher.chmod(0o755)

    metadata = {
        "runtime": verification,
        "launcher": {
            "apk_path": "lib/arm64-v8a/libnaivefox_launcher.so",
            "size": staged_launcher.stat().st_size,
        },
    }
    (output / "stage-metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"Staged plugin inputs below {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
