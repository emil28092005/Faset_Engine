"""The cooked shader interface must preserve GPU resource kinds and element strides."""

import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "compile_shader.py"
SPEC = importlib.util.spec_from_file_location("faset_compile_shader", SCRIPT)
assert SPEC and SPEC.loader
shader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(shader)


def parameter(name: str, index: int, shape: str, access: str, stride: int | None = None):
    result = {"kind": "scalar", "scalarType": "uint32", "sizes": []}
    if stride is not None:
        result["sizes"] = [{"kind": "uniform", "value": stride, "alignment": 16}]
    return {
        "name": name,
        "binding": {"kind": "descriptorTableSlot", "index": index, "space": 0},
        "type": {
            "kind": "resource",
            "baseShape": shape,
            "access": access,
            "resultType": result,
            "sizes": [{"kind": "descriptorTableSlot", "value": 1}],
        },
    }


class ReflectionTests(unittest.TestCase):
    def test_graphics_lighting_abi(self):
        compiler = os.environ["FASET_TEST_SLANGC"]
        with tempfile.TemporaryDirectory(prefix="faset-lighting-abi-") as directory:
            for source, entry, defines in (
                ("baseline.slang", "fragmentMain", []),
                ("gpu_scene.slang", "gpuVertexMain", ["--define", "FASET_GPU_GRAPHICS=1"]),
            ):
                process = subprocess.run(
                    [sys.executable, str(SCRIPT), "--compiler", compiler, "--source",
                     str(SCRIPT.parents[1] / "shaders" / source), "--entry", entry,
                     *defines, "--output", directory],
                    capture_output=True, text=True,
                )
                self.assertEqual(process.returncode, 0, process.stderr)
            fragment = json.loads((Path(directory) / "fragmentMain.reflection.json").read_text())
            gpu_vertex = json.loads((Path(directory) / "gpuVertexMain.reflection.json").read_text())
            lighting = {
                (d["set"], d["binding"]): (d["type"], d.get("element_stride"))
                for d in fragment["layout"]["descriptors"]
            }
            self.assertEqual([lighting[1, i] for i in range(4)],
                             [("storage_buffer", 80), ("storage_buffer", 80),
                              ("storage_buffer", 112), ("sampled_image_2d", None)])
            graphics = {
                (d["set"], d["binding"]): d["element_stride"]
                for d in gpu_vertex["layout"]["descriptors"]
            }
            self.assertEqual([graphics[2, i] for i in range(3)], [224, 4, 208])

    def test_gpu_vertex_paths_do_not_require_shader_draw_parameters(self):
        # SV_InstanceID makes Slang subtract BaseInstance and emit DrawParameters.
        # Our indirect commands always use firstInstance=0, so the Vulkan instance
        # index is sufficient and also runs on devices without that optional feature.
        compiler = os.environ["FASET_TEST_SLANGC"]
        with tempfile.TemporaryDirectory(prefix="faset-gpu-instance-index-") as directory:
            for entry in ("gpuVertexMain", "gpuShadowMain"):
                process = subprocess.run(
                    [sys.executable, str(SCRIPT), "--compiler", compiler, "--source",
                     str(SCRIPT.parents[1] / "shaders" / "gpu_scene.slang"), "--entry",
                     entry, "--define", "FASET_GPU_GRAPHICS=1", "--output", directory],
                    capture_output=True, text=True,
                )
                self.assertEqual(process.returncode, 0, process.stderr)
                bytecode = (Path(directory) / f"{entry}.spv").read_bytes()
                words = struct.unpack(f"<{len(bytecode) // 4}I", bytecode)
                capabilities = set()
                offset = 5
                while offset < len(words):
                    count, opcode = words[offset] >> 16, words[offset] & 0xffff
                    self.assertGreater(count, 0)
                    if opcode == 17:  # OpCapability
                        capabilities.add(words[offset + 1])
                    offset += count
                self.assertIn(1, capabilities)  # Shader
                self.assertNotIn(4427, capabilities)  # DrawParameters

    def test_gpu_storage_resources_keep_kind_and_stride(self):
        parameters = [
            parameter("instances", 0, "structuredBuffer", "read", 224),
            parameter("visibleIds", 1, "structuredBuffer", "readWrite", 4),
            parameter("depthOutput", 2, "texture2D", "readWrite"),
            parameter("depthInput", 3, "texture2D", "read"),
        ]
        raw = {
            "parameters": parameters,
            "entryPoints": [{
                "name": "gpuCullMain", "stage": "compute",
                "bindings": [{"name": p["name"], "binding": {"used": 1}} for p in parameters],
            }],
        }
        spirv = struct.pack("<5I", 0x07230203, 0x00010600, 0, 1, 0)
        layout = shader.normalize(raw, spirv, "gpuCullMain")["layout"]
        descriptors = layout["descriptors"]
        self.assertEqual(
            [(d["type"], d.get("element_stride")) for d in descriptors],
            [("storage_buffer", 224), ("storage_buffer", 4),
             ("storage_image_2d", None), ("sampled_image_2d", None)],
        )

    def test_real_slang_compute_interface(self):
        compiler = os.environ["FASET_TEST_SLANGC"]
        with tempfile.TemporaryDirectory(prefix="faset-shader-reflection-") as directory:
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "--compiler", compiler, "--source",
                 str(SCRIPT.parents[1] / "shaders" / "gpu_scene.slang"), "--entry",
                 "gpuCullMain", "--define", "FASET_GPU_CULL=1", "--output", directory],
                capture_output=True, text=True,
            )
            self.assertEqual(process.returncode, 0, process.stderr)
            metadata = json.loads((Path(directory) / "gpuCullMain.reflection.json").read_text())
            bindings = {item["binding"]: item for item in metadata["layout"]["descriptors"]}
            self.assertEqual([bindings[n]["element_stride"] for n in (0, 1, 2, 3, 4, 5, 6, 9)],
                             [224, 16, 16, 4, 16, 4, 4, 208])
            self.assertEqual([bindings[n]["type"] for n in (7, 8)],
                             ["sampled_image_2d", "sampled_image_2d"])

    def test_hzb_storage_image_has_explicit_r32f_format(self):
        compiler = os.environ["FASET_TEST_SLANGC"]
        with tempfile.TemporaryDirectory(prefix="faset-hzb-format-") as directory:
            process = subprocess.run(
                [sys.executable, str(SCRIPT), "--compiler", compiler, "--source",
                 str(SCRIPT.parents[1] / "shaders" / "gpu_scene.slang"), "--entry",
                 "gpuHzbMain", "--define", "FASET_GPU_HZB=1", "--output", directory],
                capture_output=True, text=True,
            )
            self.assertEqual(process.returncode, 0, process.stderr)
            bytecode = (Path(directory) / "gpuHzbMain.spv").read_bytes()
            words = struct.unpack(f"<{len(bytecode) // 4}I", bytecode)
            capabilities = set()
            image_formats = []
            offset = 5
            while offset < len(words):
                count, opcode = words[offset] >> 16, words[offset] & 0xffff
                self.assertGreater(count, 0)
                if opcode == 17:  # OpCapability
                    capabilities.add(words[offset + 1])
                elif opcode == 25:  # OpTypeImage; last operand is ImageFormat
                    image_formats.append((words[offset + 7], words[offset + 8]))
                offset += count
            self.assertNotIn(55, capabilities)  # StorageImageReadWithoutFormat
            self.assertNotIn(56, capabilities)  # StorageImageWriteWithoutFormat
            self.assertIn((2, 3), image_formats)  # storage image, R32f


if __name__ == "__main__":
    unittest.main()
