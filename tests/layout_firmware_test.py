"""The optional NVIDIA firmware on C: (scripts/layout.py): what was chosen is recorded whole,
and a fill list never names firmware that changed after it was chosen."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
LAYOUT = ROOT / "scripts/layout.py"
VERSION = "615.78.08"
FOLDER = "C:\\Nocturne\\Firmware\\NVIDIA\\" + VERSION + "\\"


class FirmwareSelectionTest(unittest.TestCase):
    def setUp(self):
        self.build = (ROOT / "build").resolve()
        self.build.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="firmware-test-", dir=self.build)
        self.base = Path(self.temporary.name).resolve()
        self.assertTrue(self.base.is_relative_to(self.build))
        # layout.py works in the current folder: an empty build/programs there, no product files.
        (self.base / "build/programs").mkdir(parents=True)
        self.source = self.base / "optional source"
        (self.source / "firmware").mkdir(parents=True)
        (self.source / "firmware/gsp_ga10x.bin").write_bytes(b"test GSP")
        (self.source / "firmware/ucodes_ga10x.bin").write_bytes(b"test ucodes")
        (self.source / "LICENSE").write_text("fixture licence", encoding="utf-8")
        self.state = self.base / "state.json"
        self.list = self.base / "c.list"

    def tearDown(self):
        # Only this test's newly created directory, never a product image or user data.
        self.assertTrue(self.base.resolve().is_relative_to(self.build))
        self.temporary.cleanup()

    def run_layout(self, *args, success=True):
        result = subprocess.run([sys.executable, "-X", "utf8", str(LAYOUT), *map(str, args)], cwd=self.base,
                                capture_output=True, text=True, encoding="utf-8")
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def select(self, source, success=True):
        return self.run_layout("--firmware-state", self.state, "--firmware-source", source,
                               "--firmware-version", VERSION, success=success)

    def firmware(self):
        """the firmware lines of the system's fill list: {name on C:: (host file, attributes)}"""
        self.run_layout("--firmware-manifest", self.state, "--system", self.list)
        entries = {}
        for line in self.list.read_text(encoding="utf-8").splitlines():
            if line.startswith("#"):
                continue
            path, host, attributes = line.split("\t")
            if path.startswith(FOLDER):
                entries[path[len(FOLDER):]] = (Path(host).read_bytes(), attributes)
        return entries

    def test_enabled_disabled_enabled(self):
        self.select(self.source)
        first = self.firmware()
        self.assertEqual(first, {"gsp_ga10x.bin": (b"test GSP", "--system"),
                                 "ucodes_ga10x.bin": (b"test ucodes", "--system"),
                                 "LICENSE": (b"fixture licence", "--system")})
        before = self.state.stat().st_mtime_ns
        self.select(self.source)
        self.assertEqual(self.state.stat().st_mtime_ns, before, "the same choice must not look changed to make")
        self.select("")
        self.assertEqual(self.firmware(), {})
        self.select(self.source)
        self.assertEqual(self.firmware(), first)

    def test_content_not_only_timestamp_is_an_input(self):
        self.select(self.source)
        old = self.state.read_bytes()
        image = self.source / "firmware/gsp_ga10x.bin"
        stamp = image.stat()
        image.write_bytes(b"next GSP")
        os.utime(image, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.select(self.source)
        self.assertNotEqual(self.state.read_bytes(), old)
        self.assertEqual(self.firmware()["gsp_ga10x.bin"][0], b"next GSP")

    def test_missing_optional_source_and_incomplete_installation(self):
        self.select(self.base / "not installed")
        self.assertEqual(self.firmware(), {})
        before = self.state.read_bytes()
        (self.source / "firmware/ucodes_ga10x.bin").unlink()
        self.select(self.source, success=False)
        self.assertEqual(self.state.read_bytes(), before)

    def test_missing_gsp_is_not_an_uninstalled_optional_directory(self):
        self.select(self.source)
        before = self.state.read_bytes()
        (self.source / "firmware/gsp_ga10x.bin").unlink()
        self.select(self.source, success=False)
        self.assertEqual(self.state.read_bytes(), before)

    def test_changed_after_selection_is_not_listed(self):
        self.select(self.source)
        (self.source / "LICENSE").write_text("changed", encoding="utf-8")
        self.run_layout("--firmware-manifest", self.state, "--system", self.list, success=False)
        self.assertFalse(self.list.exists())


if __name__ == "__main__":
    unittest.main()
