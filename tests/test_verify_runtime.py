from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path

from verify_runtime import VerificationError, verify_runtime


class RuntimeVerificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "naivefox-android-aarch64"
        self.root.mkdir()
        self._write(
            "include/NaiveFoxAPI.h",
            b"int NaiveFoxRunEmbedded(const char* aConfigJson, "
            b"const char* aProfilePath, const char* aRuntimePath, "
            b"const char* aTransport);\n",
            0o644,
        )
        self._write("lib/arm64-v8a/libxul.so", b"test-libxul", 0o755)
        self._write("lib/arm64-v8a/libdependency.so", b"test-dependency", 0o755)
        self._write("lib/arm64-v8a/omni.ja", b"test-omni", 0o644)
        self.manifest = {
            "abi": "arm64-v8a",
            "exported_symbols": [
                "NaiveFoxMain",
                "NaiveFoxRequestStop",
                "NaiveFoxRunEmbedded",
                "NaiveFoxVersion",
            ],
            "files": self._file_manifest(),
            "format_version": 1,
            "min_android_api": 26,
            "product": "naivefox-android-embedded",
            "required_android_system_libraries": ["libc.so", "libdl.so"],
            "runtime_path": "lib/arm64-v8a",
            "target": "android-aarch64",
            "target_triple": "aarch64-linux-android",
            "total_bytes": sum(item["size"] for item in self._file_manifest()),
            "version": "test-runtime",
        }
        self._save_manifest()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _write(self, relative: str, data: bytes, mode: int) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)

    def _file_manifest(self) -> list[dict[str, object]]:
        result = []
        for path in sorted(self.root.rglob("*")):
            if not path.is_file() or path.name == "manifest.json":
                continue
            data = path.read_bytes()
            result.append(
                {
                    "mode": f"{path.stat().st_mode & 0o777:04o}",
                    "path": path.relative_to(self.root).as_posix(),
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "size": len(data),
                }
            )
        return result

    def _save_manifest(self) -> None:
        (self.root / "manifest.json").write_text(
            json.dumps(self.manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    def test_valid_runtime(self) -> None:
        result = verify_runtime(self.root)
        self.assertEqual(result["runtime_path"], "lib/arm64-v8a")
        self.assertEqual(result["file_count"], 4)

    def test_rejects_checksum_mismatch(self) -> None:
        (self.root / "lib/arm64-v8a/libxul.so").write_bytes(b"tampered")
        with self.assertRaisesRegex(VerificationError, "size mismatch|sha256 mismatch"):
            verify_runtime(self.root)

    def test_rejects_unmanifested_file(self) -> None:
        self._write("lib/arm64-v8a/extra.so", b"extra", 0o755)
        with self.assertRaisesRegex(VerificationError, "file set mismatch"):
            verify_runtime(self.root)

    def test_rejects_unsafe_path(self) -> None:
        self.manifest["files"][0]["path"] = "../escape"
        self._save_manifest()
        with self.assertRaisesRegex(VerificationError, "unsafe"):
            verify_runtime(self.root)

    def test_rejects_newer_android_api(self) -> None:
        self.manifest["min_android_api"] = 27
        self._save_manifest()
        with self.assertRaisesRegex(VerificationError, "API 27"):
            verify_runtime(self.root)

    def test_rejects_missing_c_abi_symbol(self) -> None:
        self.manifest["exported_symbols"].remove("NaiveFoxRequestStop")
        self._save_manifest()
        with self.assertRaisesRegex(VerificationError, "missing required symbols"):
            verify_runtime(self.root)

    def test_rejects_obsolete_three_argument_embedded_abi(self) -> None:
        self._write(
            "include/NaiveFoxAPI.h",
            b"int NaiveFoxRunEmbedded(const char* aConfigJson, "
            b"const char* aProfilePath, const char* aRuntimePath);\n",
            0o644,
        )
        self.manifest["files"] = self._file_manifest()
        self.manifest["total_bytes"] = sum(
            item["size"] for item in self.manifest["files"]
        )
        self._save_manifest()
        with self.assertRaisesRegex(VerificationError, "four-argument"):
            verify_runtime(self.root)

    def test_allows_same_basename_in_different_directories(self) -> None:
        self._write("other/libdependency.so", b"other-dependency", 0o755)
        self.manifest["files"] = self._file_manifest()
        self.manifest["total_bytes"] = sum(
            item["size"] for item in self.manifest["files"]
        )
        self._save_manifest()
        result = verify_runtime(self.root)
        self.assertEqual(result["file_count"], 5)


if __name__ == "__main__":
    unittest.main()
