#!/usr/bin/env python3
"""Tests actual GLB bundle publication without requiring Blender's GUI/Python runtime."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import sys
sys.dont_write_bytecode = True
import unittest
import uuid

MODULE = Path(__file__).resolve().parents[1] / "tools/blender_addon/bundle.py"
spec = importlib.util.spec_from_file_location("faset_bundle", MODULE)
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


def glb(path, identity, name="Door", duplicate=False):
    nodes = [{"name": name, "extras": {"faset_id": identity}}]
    if duplicate:
        nodes.append(nodes[0].copy())
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": nodes}
    raw = json.dumps(doc).encode()
    raw += b" " * (-len(raw) % 4)
    path.write_bytes(struct.pack("<IIIII", 0x46546C67, 2, 20 + len(raw), len(raw), 0x4E4F534A) + raw)


class BundleTests(unittest.TestCase):
    def test_atomic_roundtrip_and_duplicate_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            identity, asset_id = str(uuid.uuid4()), str(uuid.uuid4())
            source, out = root / "scene.glb", root / "published"
            glb(source, identity)
            first = bundle.publish_bundle(source, out, asset_id, "fixture")
            payload = out / first["files"][0]["path"]
            self.assertEqual(hashlib.sha256(payload.read_bytes()).hexdigest(), first["files"][0]["sha256"])
            self.assertEqual(json.loads((out / "manifest.json").read_text()), first)
            glb(source, identity, name="Renamed door")
            second = bundle.publish_bundle(source, out, asset_id, "fixture")
            self.assertNotEqual(first["generation"], second["generation"])
            self.assertEqual(first["outputs"][0]["source_id"], second["outputs"][0]["source_id"])
            self.assertTrue(payload.exists(), "old immutable payload prematurely destroyed")
            previous = (out / "manifest.json").read_bytes()
            glb(source, identity, duplicate=True)
            with self.assertRaisesRegex(ValueError, "DuplicateSourceId"):
                bundle.publish_bundle(source, out, asset_id, "fixture")
            self.assertEqual((out / "manifest.json").read_bytes(), previous)
            source.write_bytes(b"broken")
            with self.assertRaises(ValueError):
                bundle.publish_bundle(source, out, asset_id, "fixture")
            self.assertEqual((out / "manifest.json").read_bytes(), previous)

    def test_rejects_external_uri_before_publication(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw = json.dumps({"asset": {"version": "2.0"}, "buffers": [{"uri": "missing.bin", "byteLength": 4}]}).encode()
            raw += b" " * (-len(raw) % 4)
            source = root / "source.glb"
            source.write_bytes(struct.pack("<IIIII", 0x46546C67, 2, 20 + len(raw), len(raw), 0x4E4F534A) + raw)
            with self.assertRaisesRegex(ValueError, "embedded"):
                bundle.publish_bundle(source, root / "out", str(uuid.uuid4()), "fixture")
            self.assertFalse((root / "out" / "manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
