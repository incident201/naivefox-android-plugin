#!/usr/bin/env python3
"""Verify an extracted NaiveFox Android embedded runtime package."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
from pathlib import Path, PurePosixPath
from typing import Any


FORMAT_VERSION = 1
PRODUCT = "naivefox-android-embedded"
TARGET = "android-aarch64"
ABI = "arm64-v8a"
TARGET_TRIPLE = "aarch64-linux-android"
MAXIMUM_ANDROID_API = 26
REQUIRED_SYMBOLS = {
    "NaiveFoxRunEmbedded",
    "NaiveFoxRequestStop",
    "NaiveFoxVersion",
}
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
MODE_PATTERN = re.compile(r"^[0-7]{4}$")


class VerificationError(RuntimeError):
    """Raised when a downloaded runtime violates the package contract."""


def _fail(message: str) -> None:
    raise VerificationError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative_path(value: Any, description: str) -> str:
    if not isinstance(value, str) or not value:
        _fail(f"{description} must be a non-empty string")
    if "\\" in value or value.startswith("/") or value.endswith("/") or "//" in value:
        _fail(f"unsafe {description}: {value!r}")
    candidate = PurePosixPath(value)
    if candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts):
        _fail(f"unsafe {description}: {value!r}")
    if candidate.as_posix() != value:
        _fail(f"non-canonical {description}: {value!r}")
    return value


def _require_equal(manifest: dict[str, Any], name: str, expected: Any) -> None:
    actual = manifest.get(name)
    if actual != expected:
        _fail(f"manifest {name} must be {expected!r}, got {actual!r}")


def _load_manifest(root: Path) -> dict[str, Any]:
    manifest_path = root / "manifest.json"
    if not manifest_path.is_file() or manifest_path.is_symlink():
        _fail("runtime manifest.json is missing or is not a regular file")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        _fail(f"cannot read runtime manifest: {error}")
    if not isinstance(manifest, dict):
        _fail("runtime manifest root must be an object")
    return manifest


def verify_runtime(root_value: str | os.PathLike[str] | Path) -> dict[str, Any]:
    root = Path(root_value).resolve(strict=True)
    if not root.is_dir() or root.is_symlink():
        _fail("runtime package root must be a real directory")

    for path in root.rglob("*"):
        if path.is_symlink():
            _fail(f"runtime package contains a symbolic link: {path.relative_to(root)}")
        if not path.is_dir() and not path.is_file():
            _fail(f"runtime package contains a special file: {path.relative_to(root)}")

    manifest = _load_manifest(root)
    _require_equal(manifest, "format_version", FORMAT_VERSION)
    _require_equal(manifest, "product", PRODUCT)
    _require_equal(manifest, "target", TARGET)
    _require_equal(manifest, "abi", ABI)
    _require_equal(manifest, "target_triple", TARGET_TRIPLE)

    minimum_api = manifest.get("min_android_api")
    if not isinstance(minimum_api, int) or isinstance(minimum_api, bool):
        _fail("manifest min_android_api must be an integer")
    if minimum_api < 1 or minimum_api > MAXIMUM_ANDROID_API:
        _fail(
            f"runtime requires Android API {minimum_api}; plugin supports runtime API up to "
            f"{MAXIMUM_ANDROID_API}"
        )

    version = manifest.get("version")
    if not isinstance(version, str) or not version.strip():
        _fail("manifest version must be a non-empty string")

    runtime_path_text = safe_relative_path(manifest.get("runtime_path"), "runtime_path")
    runtime_path = root.joinpath(*PurePosixPath(runtime_path_text).parts)
    if not runtime_path.is_dir():
        _fail(f"manifest runtime_path is not a directory: {runtime_path_text}")

    symbols = manifest.get("exported_symbols")
    if (
        not isinstance(symbols, list)
        or not all(isinstance(symbol, str) and symbol for symbol in symbols)
        or len(set(symbols)) != len(symbols)
    ):
        _fail("manifest exported_symbols must be a unique non-empty string list")
    missing_symbols = sorted(REQUIRED_SYMBOLS.difference(symbols))
    if missing_symbols:
        _fail(f"manifest is missing required symbols: {missing_symbols}")

    system_libraries = manifest.get("required_android_system_libraries")
    if (
        not isinstance(system_libraries, list)
        or not all(
            isinstance(name, str)
            and name == Path(name).name
            and name.startswith("lib")
            and name.endswith(".so")
            for name in system_libraries
        )
        or len(set(system_libraries)) != len(system_libraries)
    ):
        _fail("manifest required_android_system_libraries is invalid")

    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        _fail("manifest files must be a non-empty list")

    expected_paths: set[str] = set()
    total_bytes = 0
    for index, item in enumerate(files):
        if not isinstance(item, dict):
            _fail(f"manifest files[{index}] must be an object")
        relative = safe_relative_path(item.get("path"), f"files[{index}].path")
        if relative == "manifest.json" or relative in expected_paths:
            _fail(f"duplicate or reserved manifest file path: {relative}")
        expected_paths.add(relative)

        mode_text = item.get("mode")
        if not isinstance(mode_text, str) or not MODE_PATTERN.fullmatch(mode_text):
            _fail(f"invalid mode for {relative}: {mode_text!r}")
        expected_mode = int(mode_text, 8)
        if expected_mode & ~0o777:
            _fail(f"unsafe mode for {relative}: {mode_text}")

        expected_hash = item.get("sha256")
        if not isinstance(expected_hash, str) or not SHA256_PATTERN.fullmatch(expected_hash):
            _fail(f"invalid sha256 for {relative}")
        expected_size = item.get("size")
        if not isinstance(expected_size, int) or isinstance(expected_size, bool) or expected_size < 0:
            _fail(f"invalid size for {relative}")

        path = root.joinpath(*PurePosixPath(relative).parts)
        if not path.is_file() or path.is_symlink():
            _fail(f"manifest file is missing or not regular: {relative}")
        actual_size = path.stat().st_size
        if actual_size != expected_size:
            _fail(f"size mismatch for {relative}: expected {expected_size}, got {actual_size}")
        actual_mode = stat.S_IMODE(path.stat().st_mode)
        if actual_mode != expected_mode:
            _fail(
                f"mode mismatch for {relative}: expected {mode_text}, got {actual_mode:04o}"
            )
        actual_hash = sha256_file(path)
        if actual_hash != expected_hash:
            _fail(f"sha256 mismatch for {relative}")
        total_bytes += actual_size

    actual_paths = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path.name != "manifest.json"
    }
    if actual_paths != expected_paths:
        missing = sorted(expected_paths - actual_paths)
        extra = sorted(actual_paths - expected_paths)
        _fail(f"manifest file set mismatch; missing={missing}, extra={extra}")

    manifest_total = manifest.get("total_bytes")
    if manifest_total != total_bytes:
        _fail(f"manifest total_bytes must be {total_bytes}, got {manifest_total!r}")

    required_files = [
        root / "include" / "NaiveFoxAPI.h",
        runtime_path / "libxul.so",
        runtime_path / "omni.ja",
    ]
    for required in required_files:
        if not required.is_file():
            _fail(f"required runtime file is missing: {required.relative_to(root).as_posix()}")

    return {
        "root": str(root),
        "version": version,
        "runtime_path": runtime_path_text,
        "minimum_android_api": minimum_api,
        "file_count": len(files),
        "total_bytes": total_bytes,
        "shared_libraries": sorted(
            relative for relative in expected_paths if relative.endswith(".so")
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime_root", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json")
    arguments = parser.parse_args()
    try:
        result = verify_runtime(arguments.runtime_root)
    except (OSError, VerificationError) as error:
        parser.error(str(error))
    if arguments.as_json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(
            "Verified NaiveFox Android runtime: "
            f"version={result['version']} files={result['file_count']} "
            f"bytes={result['total_bytes']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
