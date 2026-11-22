import hashlib
import sys
from pathlib import Path
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/aosp"))
from build_upstream_framework_dex import apply_patch, profile_sources


class StrictPatchTest(unittest.TestCase):
    name = "frameworks/base/core/java/android/example/Target.java"
    content = b"first\nold\nlast\n"

    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.patch = Path(work.name) / "example.patch"
        self.sources = {self.name: self.content}

    def write(self, hunk):
        headers = f"--- a/{self.name}\n+++ b/{self.name}\n".encode()
        self.patch.write_bytes(headers + hunk)

    def test_applies_byte_exact_context_and_counts(self):
        self.write(b"@@ -1,3 +1,4 @@\n first\n-old\n+new\n+inserted\n last\n")
        report = apply_patch(self.patch, self.sources)
        self.assertEqual(self.sources[self.name], b"first\nnew\ninserted\nlast\n")
        self.assertEqual(report["files"], {self.name: {"added": 2, "removed": 1}})
        self.assertEqual(report["sha256"], hashlib.sha256(self.patch.read_bytes()).hexdigest())

    def test_rejects_shifted_hunk(self):
        self.write(b"@@ -1 +1 @@\n-old\n+new\n")
        with self.assertRaisesRegex(ValueError, "exact hunk match failed"):
            apply_patch(self.patch, self.sources)
        self.assertEqual(self.sources[self.name], self.content)

    def test_rejects_wrong_context(self):
        self.write(b"@@ -1,2 +1,2 @@\n wrong\n-old\n+new\n")
        with self.assertRaisesRegex(ValueError, "exact hunk match failed"):
            apply_patch(self.patch, self.sources)

    def test_rejects_unlisted_target(self):
        self.patch.write_bytes(b"--- a/other.java\n+++ b/other.java\n@@ -1 +1 @@\n-old\n+new\n")
        with self.assertRaisesRegex(ValueError, "invalid or repeated Java target"):
            apply_patch(self.patch, self.sources)


class SourceProfileTest(unittest.TestCase):
    def test_directory_includes_subpackages_and_tracks_additions(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            directory = "frameworks/base/core/java/android/content/res"
            first = directory + "/Resources.java"
            nested = directory + "/loader/ResourcesLoader.java"
            excluded = directory + "/DeviceOnly.java"
            for name in (first, nested, excluded):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.touch()
            manifest = root / "sources.txt"
            manifest.write_text(
                f"[framework]\n@dir {directory}\n@exclude {excluded}\n"
                "[services]\nframeworks/base/services/Resolver.java\n")
            inputs = set()
            groups = profile_sources(root, inputs, manifest)
            self.assertEqual(groups["framework"], [first, nested])
            self.assertEqual(inputs, {root / directory, (root / nested).parent})

            with manifest.open("a") as sources:
                sources.write(f"[framework]\n{nested}\n")
            with self.assertRaisesRegex(ValueError, "duplicate source"):
                profile_sources(root, set(), manifest)


if __name__ == "__main__":
    unittest.main()
