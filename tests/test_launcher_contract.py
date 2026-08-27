from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "native/launcher.c"


class LauncherContractTests(unittest.TestCase):
    def test_zip_bounds_use_compressed_size(self) -> None:
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("data_offset + compressed_size > archive_size", source)
        self.assertNotIn("data_offset + uncompressed_size > archive_size", source)


if __name__ == "__main__":
    unittest.main()
