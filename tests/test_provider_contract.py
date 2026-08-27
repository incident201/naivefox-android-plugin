from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROVIDER = ROOT / "app/src/main/java/com/github/incident201/naivefox/plugin/RuntimeProvider.java"


class ProviderContractTests(unittest.TestCase):
    def test_fast_path_throws_to_reach_slow_path(self) -> None:
        source = PROVIDER.read_text(encoding="utf-8")
        call_method = source.split("public Bundle call(", 1)[1].split("\n    }", 1)[0]
        self.assertIn("METHOD_GET_EXECUTABLE.equals(method)", call_method)
        self.assertIn("throw new UnsupportedOperationException", call_method)
        self.assertNotIn("return null", call_method)


if __name__ == "__main__":
    unittest.main()
