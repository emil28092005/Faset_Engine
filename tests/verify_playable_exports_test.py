"""Small contract checks for the cross-platform export verifier's Lua mode."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from verify_playable_exports import (project_matrix, project_workspace, verify_capture,
                                     verify_lua_export, vulkan_device_details)


class ExportProjectMatrixTests(unittest.TestCase):
    def test_native_project_workspace_is_outside_engine(self):
        with tempfile.TemporaryDirectory() as directory:
            engine = Path(directory) / "engine"
            engine.mkdir()
            internal_output = engine / ".cache" / "exports"
            workspace = project_workspace(engine, internal_output)
            try:
                self.assertFalse(workspace.is_relative_to(engine))
            finally:
                workspace.rmdir()
            external_output = Path(directory) / "evidence"
            self.assertEqual(project_workspace(engine, external_output), external_output)

    def test_lua_is_explicit_and_can_run_alone(self):
        self.assertEqual([name for name, _, _ in project_matrix(False, False)],
                         ["collect-2d", "collect-3d"])
        self.assertEqual([name for name, _, _ in project_matrix(True, False)],
                         ["collect-2d", "collect-3d", "lua"])
        self.assertEqual(project_matrix(False, True), [("lua", 2, "lua")])

    def test_lua_package_requires_source_and_license_without_cpp_stub(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Scripts").mkdir()
            (root / "Notices/lua").mkdir(parents=True)
            (root / "Scripts/player.lua").write_text("return {}\n", encoding="utf-8")
            (root / "Notices/lua/LICENSE.txt").write_text("License\n", encoding="utf-8")
            manifest = {"lua_enabled": True, "files": [
                {"path": "Scripts/player.lua"}, {"path": "Notices/lua/LICENSE.txt"}]}
            verify_lua_export(root, manifest)
            with self.assertRaisesRegex(RuntimeError, "license"):
                verify_lua_export(root, {**manifest, "files": manifest["files"][:1]})
            (root / "Scripts/Gameplay.cpp").write_text("// stray\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "C\\+\\+"):
                verify_lua_export(root, manifest)
            (root / "Scripts/Gameplay.cpp").unlink()
            (root / "Notices/lua/LICENSE.txt").unlink()
            with self.assertRaisesRegex(RuntimeError, "license"):
                verify_lua_export(root, manifest)

    def test_lua_capture_requires_its_small_scene_palette(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.ppm"
            header = b"P6\n4 1\n255\n"
            path.write_bytes(header + bytes.fromhex("0e1116334c6633bfe5ffb233"))
            self.assertEqual(verify_capture(path, language="lua")["sampled_colors"], 4)
            with self.assertRaisesRegex(RuntimeError, "geometry/colors"):
                verify_capture(path)
            path.write_bytes(header + bytes.fromhex("0e1116334c6633bfe5334c66"))
            with self.assertRaisesRegex(RuntimeError, "geometry/colors"):
                verify_capture(path, language="lua")

    def test_driver_probe_selects_the_rendering_device(self):
        summary = ("GPU0:\n deviceName = llvmpipe\n driverName = Mesa\n"
                   "GPU1:\n deviceName = Discrete GPU\n driverVersion = 123\n"
                   " driverName = Vendor\n driverInfo = 1.2.3\n")
        self.assertEqual(vulkan_device_details(summary, "Discrete GPU"),
                         {"deviceName": "Discrete GPU", "driverVersion": "123",
                          "driverName": "Vendor", "driverInfo": "1.2.3"})
        self.assertEqual(vulkan_device_details(summary, "Unknown"), {})


if __name__ == "__main__":
    unittest.main()
