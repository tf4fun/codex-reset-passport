#!/usr/bin/env python3
"""Host tests for the pinned SDK fragment patch; ESP-IDF is not required."""
from __future__ import annotations
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("blufi_patch", ROOT / "tools/apply_blufi_safety_patch.py")
assert spec and spec.loader
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


def fixture() -> str:
    source = (ROOT / "tests/blufi_stubs/recv_handler_5_5_3.c").read_text(encoding="utf-8")
    return source[source.index("void btc_blufi_recv_handler("):]


class BlufiSafetyPatchTests(unittest.TestCase):
    def test_fixture_and_patch_are_pinned(self):
        self.assertEqual(patch.digest(fixture().encode()), patch.MANIFEST["receiver_upstream_sha256"])
        result = patch.apply_unified(fixture(), patch.PATCH,
                                     line_offset=patch.MANIFEST["receiver_line_offset"])
        self.assertEqual(patch.digest(result.encode()), patch.MANIFEST["receiver_patched_sha256"])

    def test_context_changes_are_rejected(self):
        with self.assertRaises(ValueError):
            patch.apply_unified(fixture().replace("blufi_env.total_len = hdr", "blufi_env.total_len = 1 + hdr"),
                                patch.PATCH, line_offset=patch.MANIFEST["receiver_line_offset"])

    def test_sdk_version_and_unknown_source_refused_without_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            version = root / "tools/cmake/version.cmake"
            version.parent.mkdir(parents=True)
            source = root / patch.MANIFEST["source_path"]
            source.parent.mkdir(parents=True)
            source.write_bytes(b"user changes must remain\n")
            for version_number in ("4", "3"):
                version.write_text("set(IDF_VERSION_MAJOR 5)\nset(IDF_VERSION_MINOR 5)\n"
                                   f"set(IDF_VERSION_PATCH {version_number})\n")
                for apply in (True, False):
                    with self.assertRaises(ValueError):
                        patch.process(root, apply=apply)
                    self.assertEqual(source.read_bytes(), b"user changes must remain\n")

    def test_sdk_check_and_apply_are_idempotent(self):
        # Exercise filesystem behavior using a tiny exact unified patch. Real
        # receiver integrity is separately pinned and compiled below.
        original_manifest, original_patch = patch.MANIFEST, patch.PATCH
        baseline = b"line one\nold\nline three\n"
        expected = b"line one\nnew\nline three\n"
        try:
            patch.MANIFEST = dict(original_manifest,
                upstream_sha256=patch.digest(baseline), patched_sha256=patch.digest(expected))
            patch.PATCH = "--- a/source\n+++ b/source\n@@ -1,3 +1,3 @@\n line one\n-old\n+new\n line three\n"
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                version = root / "tools/cmake/version.cmake"
                version.parent.mkdir(parents=True)
                version.write_text("set(IDF_VERSION_MAJOR 5)\nset(IDF_VERSION_MINOR 5)\nset(IDF_VERSION_PATCH 3)\n")
                source = root / patch.MANIFEST["source_path"]
                source.parent.mkdir(parents=True)
                source.write_bytes(baseline)
                with self.assertRaises(ValueError): patch.process(root, apply=False)
                self.assertEqual(source.read_bytes(), baseline)
                patch.process(root, apply=True)
                self.assertEqual(source.read_bytes(), expected)
                patch.process(root, apply=True)
                patch.process(root, apply=False)
                self.assertEqual(source.read_bytes(), expected)
        finally:
            patch.MANIFEST, patch.PATCH = original_manifest, original_patch

    def test_patched_real_receiver_negative_and_positive_frames(self):
        receiver = patch.apply_unified(fixture(), patch.PATCH,
                                       line_offset=patch.MANIFEST["receiver_line_offset"])
        self.assertEqual(patch.digest(receiver.encode()), patch.MANIFEST["receiver_patched_sha256"])
        harness = (ROOT / "tests/blufi_stubs/recv_host_harness.c").read_text(encoding="utf-8")
        source_text = harness.replace("/* INSERT_PINNED_RECEIVER */", receiver)
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "test.c"
            binary = Path(temporary) / "test"
            source.write_text(source_text)
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare"]
            # Enable sanitizers explicitly when supported by the execution
            # environment. LeakSanitizer cannot operate in ptrace sandboxes.
            if os.environ.get("BLUFI_TEST_SANITIZERS") == "1":
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run([os.environ.get("CC", "cc"), *flags, str(source), "-o", str(binary)], check=True)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
            subprocess.run([str(binary)], check=True, env=env)


if __name__ == "__main__":
    unittest.main()
