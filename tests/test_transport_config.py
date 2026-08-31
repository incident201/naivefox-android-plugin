"""Exercise the production C selector on the GitHub runner (no local builds)."""
from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest


class TransportConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        source = Path(__file__).resolve().parents[1] / "native" / "transport_config.c"
        output = Path(cls.temporary.name) / "transport_config.so"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-shared", "-fPIC",
            "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wsign-conversion",
            "-Wshadow", "-fsanitize=undefined", "-fno-sanitize-recover=all",
            str(source), "-o", str(output),
        ], check=True)
        cls.library = ctypes.CDLL(str(output))
        cls.select = cls.library.SelectTransport
        cls.select.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                              ctypes.POINTER(ctypes.c_void_p)]
        cls.select.restype = ctypes.c_bool
        cls.libc = ctypes.CDLL(None)
        cls.libc.free.argtypes = [ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def rewrite(self, raw: bytes, transport="no-connect"):
        result = ctypes.c_void_p()
        success = self.select(raw, transport.encode(), ctypes.byref(result))
        if not success:
            self.assertIsNone(result.value)
            return None
        try:
            return ctypes.string_at(result)
        finally:
            self.libc.free(result)

    def test_inserts_only_transport(self):
        raw = b' \n{ "listen":"socks://u:p@127.0.0.1:1080", "proxy":"https://u%3A:p%25@example", "x":1.00e+02 }\t'
        for mode in ("classic", "no-connect"):
            result = self.rewrite(raw, mode)
            insertion = ('"transport":"' + mode + '",').encode()
            self.assertEqual(result, raw[:3] + insertion + raw[3:])

    def test_replaces_only_existing_value(self):
        for raw_key in (b'"transport"', b'"\\u0074ransport"', b'"transp\\u006frt"'):
            raw = b'{"x":{"transport":"classic"}, ' + raw_key + b' : "classic" , "proxy":"x"}'
            expected = raw.replace(b': "classic" ,', b': "no-connect" ,')
            self.assertEqual(self.rewrite(raw), expected)

    def test_all_mode_combinations(self):
        for old in ("classic", "no-connect"):
            for new in ("classic", "no-connect"):
                self.assertEqual(self.rewrite(json.dumps({"transport": old}).encode(), new),
                                 json.dumps({"transport": new}).encode())

    def test_escaped_value(self):
        self.assertEqual(self.rewrite(b'{"transport":"no-\\u0063onnect"}', "classic"),
                         b'{"transport":"classic"}')

    def test_empty_object(self):
        self.assertEqual(self.rewrite(b'{ \n }'), b'{"transport":"no-connect" \n }')

    def test_nested_values_and_string_decoys(self):
        raw = b'{"x":[{"transport":"x"},true,false,null,-2.30E-4,[]],"y":"\\\"transport\\\":{}[]\\\\"}'
        result = self.rewrite(raw)
        self.assertEqual(result, b'{"transport":"no-connect",' + raw[1:])
        self.assertEqual(json.loads(result)["x"], json.loads(raw)["x"])

    def test_rejects_malformed_json_and_ambiguous_transport(self):
        cases = [b'', b'[]', b'null', b'{} trailing', b'{', b'{"x"}',
                 b'{"x":}', b'{"x":01}', b'{"x":1.}', b'{"x":1e}',
                 b'{"x":true false}', b'{"x":tru}', b'{"x":+1}',
                 b'{"x":[1,]}', b'{"x":1,}', b'{"x":"\\u12"}',
                 b'{"x":"\\q"}', b'{"x":"raw\nnewline"}',
                 b'{"transport":null}', b'{"transport":{}}',
                 b'{"transport":"invalid"}',
                 b'{"transport":"classic","transport":"no-connect"}',
                 b'{"transport":"classic","\\u0074ransport":"classic"}']
        for raw in cases:
            with self.subTest(raw=raw):
                self.assertIsNone(self.rewrite(raw))

    def test_rejects_unknown_build_mode(self):
        self.assertIsNone(self.rewrite(b'{}', "auto"))

    def test_size_and_nesting_limits(self):
        limit = 1024 * 1024
        raw = b'{"x":"' + b'a' * (limit - 8) + b'"}'
        self.assertEqual(len(raw), limit)
        self.assertIsNone(self.rewrite(raw))  # inserted field must also fit
        self.assertIsNone(self.rewrite(b'{"x":' + b'[' * 129 + b'0' + b']' * 129 + b'}'))
        fitting = b'{"x":"' + b'a' * (limit - 33) + b'"}'
        self.assertIsNotNone(self.rewrite(fitting))

    def test_preserves_arbitrary_unrelated_json(self):
        randomizer = random.Random(41)
        for index in range(200):
            obj = {"listen": "socks://u:@127.0.0.1:1080", "proxy": "quic://:p@example",
                   "x": [index, randomizer.random(), None, True, {"transport": "not-root"}],
                   "unicode": "кириллица / \\ \" { }", "extra-headers": "x: value\\r\\n"}
            raw = json.dumps(obj, ensure_ascii=index % 2 == 0, indent=index % 4).encode()
            for mode in ("classic", "no-connect"):
                result = json.loads(self.rewrite(raw, mode))
                self.assertEqual(result.pop("transport"), mode)
                self.assertEqual(result, obj)


if __name__ == "__main__":
    unittest.main()
