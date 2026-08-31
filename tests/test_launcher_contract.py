from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "native/launcher.c"


class LauncherContractTests(unittest.TestCase):
    def test_zip_bounds_use_compressed_size(self) -> None:
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("data_offset + compressed_size > archive_size", source)
        self.assertNotIn("data_offset + uncompressed_size > archive_size", source)

    def test_system_dependencies_are_preloaded_before_libxul(self) -> None:
        source = LAUNCHER.read_text(encoding="utf-8")
        preload = source.index("if (!PreloadAndroidSystemLibraries())")
        libxul = source.index("dlopen(libxul_path, RTLD_NOW | RTLD_GLOBAL)")
        self.assertLess(preload, libxul)

    def test_runtime_failure_diagnostics_do_not_log_config(self) -> None:
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn('"NaiveFoxNetworkStartup:5"', source)
        self.assertIn("NaiveFox exited with status %d", source)

    def test_transport_uses_four_argument_public_embedded_abi(self) -> None:
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("__typeof__(&NaiveFoxRunEmbedded)", source)
        self.assertIn(
            "run(config, profile_path, runtime_path, NAIVEFOX_PLUGIN_TRANSPORT)",
            source,
        )
        self.assertNotIn("SelectTransport", source)


if __name__ == "__main__":
    unittest.main()
