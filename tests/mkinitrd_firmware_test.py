"""Optional NVIDIA firmware selection must invalidate archives without parse-time deletion."""
import os
import importlib.util
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
PACKER = ROOT / "scripts/mkinitrd.py"
PREFIX = "usr/share/firmware/nvidia/615.78.08"


class FirmwareSelectionTest(unittest.TestCase):
    def setUp(self):
        self.build = (ROOT / "build").resolve()
        self.build.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="firmware-test-", dir=self.build)
        self.base = Path(self.temporary.name).resolve()
        self.assertTrue(self.base.is_relative_to(self.build))
        self.source = self.base / "optional source"
        (self.source / "firmware").mkdir(parents=True)
        (self.source / "firmware/gsp_ga10x.bin").write_bytes(b"test GSP")
        (self.source / "firmware/ucodes_ga10x.bin").write_bytes(b"test ucodes")
        (self.source / "LICENSE").write_text("fixture licence", encoding="utf-8")
        self.tree = self.base / "tree"
        stale = self.tree / "usr/share/firmware/nvidia/old"
        stale.mkdir(parents=True)
        (stale / "stale.bin").write_bytes(b"must not be packed")
        (self.tree / "keep.txt").write_text("unrelated", encoding="utf-8")
        self.state = self.base / "state.json"
        self.archive = self.base / "initrd.tar"

    def tearDown(self):
        # Only this test's newly created directory, never a product image or user data.
        self.assertTrue(self.base.resolve().is_relative_to(self.build))
        self.temporary.cleanup()

    def run_packer(self, *args, success=True):
        result = subprocess.run([sys.executable, "-X", "utf8", str(PACKER), *map(str, args)],
                                capture_output=True, text=True, encoding="utf-8")
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def select(self, source):
        self.run_packer("--firmware-state", self.state, "--firmware-source", source,
                        "--firmware-prefix", PREFIX)

    def pack(self):
        self.run_packer("--firmware-manifest", self.state, self.archive, self.tree)
        with tarfile.open(self.archive) as archive:
            return {member.name: archive.extractfile(member).read()
                    for member in archive.getmembers() if member.isfile()}

    def test_enabled_disabled_enabled_and_stale_staging(self):
        self.select(self.source)
        first = self.pack()
        self.assertEqual(first[PREFIX + "/gsp_ga10x.bin"], b"test GSP")
        self.assertIn(PREFIX + "/LICENSE", first)
        self.assertFalse(any("/old/" in name for name in first))
        before = self.state.stat().st_mtime_ns
        self.select(self.source)
        self.assertEqual(self.state.stat().st_mtime_ns, before, "no-op selection must not rebuild")
        self.select("")
        self.assertEqual(self.pack(), {"keep.txt": b"unrelated"})
        self.assertTrue((self.tree / "usr/share/firmware/nvidia/old/stale.bin").exists())
        self.select(self.source)
        self.assertEqual(self.pack(), first)

    def test_content_not_only_timestamp_is_an_input(self):
        self.select(self.source)
        old = self.state.read_bytes()
        image = self.source / "firmware/gsp_ga10x.bin"
        stamp = image.stat()
        image.write_bytes(b"next GSP")
        os.utime(image, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.select(self.source)
        self.assertNotEqual(self.state.read_bytes(), old)
        self.assertEqual(self.pack()[PREFIX + "/gsp_ga10x.bin"], b"next GSP")

    def test_missing_optional_source_and_incomplete_installation(self):
        self.select(self.base / "not installed")
        self.assertEqual(self.pack(), {"keep.txt": b"unrelated"})
        before = self.state.read_bytes()
        (self.source / "firmware/ucodes_ga10x.bin").unlink()
        self.run_packer("--firmware-state", self.state, "--firmware-source", self.source, success=False)
        self.assertEqual(self.state.read_bytes(), before)

    def test_changed_after_selection_is_not_silently_packed(self):
        self.select(self.source)
        (self.source / "LICENSE").write_text("changed", encoding="utf-8")
        self.run_packer("--firmware-manifest", self.state, self.archive, self.tree, success=False)
        self.assertFalse(self.archive.exists())

    def test_missing_gsp_is_not_an_uninstalled_optional_directory(self):
        self.select(self.source)
        before = self.state.read_bytes()
        (self.source / "firmware/gsp_ga10x.bin").unlink()
        self.run_packer("--firmware-state", self.state, "--firmware-source", self.source, success=False)
        self.assertEqual(self.state.read_bytes(), before)

    def test_change_between_preflight_and_pack_preserves_previous_archive(self):
        self.select(self.source)
        self.pack()
        previous = self.archive.read_bytes()
        spec = importlib.util.spec_from_file_location("firmware_packer_test", PACKER)
        packer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(packer)
        original_digest = packer.digest

        def change_after_hash(path):
            result = original_digest(path)
            if Path(path).name == "gsp_ga10x.bin":
                Path(path).write_bytes(b"changed after preflight")
            return result

        argv = [str(PACKER), "--firmware-manifest", str(self.state), str(self.archive), str(self.tree)]
        with patch.object(sys, "argv", argv), patch.object(packer, "digest", side_effect=change_after_hash):
            with self.assertRaisesRegex(ValueError, "changed while packing"):
                packer.main()
        self.assertEqual(self.archive.read_bytes(), previous)
        self.assertFalse(list(self.base.glob("initrd.tar.*.tmp")))

    def test_legacy_positional_packing_remains_supported(self):
        self.run_packer(self.archive, self.tree)
        with tarfile.open(self.archive) as archive:
            self.assertEqual(archive.extractfile("keep.txt").read(), b"unrelated")

    def test_make_rebuilds_only_when_selection_changes(self):
        make = ROOT / "tools/msys64/usr/bin/make.exe" if os.name == "nt" else Path("make")
        env = dict(os.environ)
        if os.name == "nt":
            env["PATH"] = str(ROOT / "tools/msys64/usr/bin") + os.pathsep + env["PATH"]
        # The same force/content-stamp dependency pattern as the product Makefile, with
        # tiny inputs so this regression never rebuilds the entire OS or its media.
        (self.base / "fixture.mk").write_text(
            'all: initrd.tar\n.PHONY: force\nforce:\n'
            'state.json: force\n'
            '\t"$(PYTHON)" "$(PACKER)" --firmware-state "$@" --firmware-source "$(SOURCE)"\n'
            'initrd.tar: state.json\n'
            '\t"$(PYTHON)" "$(PACKER)" --firmware-manifest state.json "$@" tree\n',
            encoding="utf-8")

        def build(source):
            result = subprocess.run([str(make), "-f", "fixture.mk", "all",
                                     "PYTHON=" + Path(sys.executable).as_posix(),
                                     "PACKER=" + PACKER.as_posix(), "SOURCE=" + str(source)],
                                    cwd=self.base, env=env, capture_output=True,
                                    text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        build(self.source.as_posix())
        first = self.archive.stat().st_mtime_ns
        build(self.source.as_posix())
        self.assertEqual(self.archive.stat().st_mtime_ns, first)
        build("")
        with tarfile.open(self.archive) as archive:
            self.assertFalse(any(m.name.startswith("usr/share/firmware/nvidia") for m in archive))
        build(self.source.as_posix())
        with tarfile.open(self.archive) as archive:
            self.assertEqual(archive.extractfile(PREFIX + "/gsp_ga10x.bin").read(), b"test GSP")


if __name__ == "__main__":
    unittest.main()
