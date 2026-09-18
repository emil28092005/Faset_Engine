#!/usr/bin/env python3
"""Verify the prepared Windows Vulkan DLL/ICD before spending time on engine builds.

Use VK_LOADER_DEBUG=all to retain the loader's own driver-discovery diagnostics.
This probe uses ctypes and the Vulkan ABI, and needs neither the engine nor Slang.
"""
from __future__ import annotations

import argparse
import ctypes as ct
import json
import os
from pathlib import Path
import sys


class Application(ct.Structure):
    _fields_ = [("sType", ct.c_uint32), ("pNext", ct.c_void_p), ("pApplicationName", ct.c_char_p),
                ("applicationVersion", ct.c_uint32), ("pEngineName", ct.c_char_p),
                ("engineVersion", ct.c_uint32), ("apiVersion", ct.c_uint32)]


class InstanceCreate(ct.Structure):
    _fields_ = [("sType", ct.c_uint32), ("pNext", ct.c_void_p), ("flags", ct.c_uint32),
                ("pApplicationInfo", ct.POINTER(Application)), ("enabledLayerCount", ct.c_uint32),
                ("ppEnabledLayerNames", ct.POINTER(ct.c_char_p)), ("enabledExtensionCount", ct.c_uint32),
                ("ppEnabledExtensionNames", ct.POINTER(ct.c_char_p))]


def version(value: int) -> str:
    return f"{value >> 22}.{(value >> 12) & 1023}.{value & 4095}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(".cache/windows-graphics"))
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("This probe verifies the native Windows DLL loader")
    cache = args.cache.resolve()
    manifest_path = cache / "driver/vk_swiftshader_icd.json"
    loader_path = cache / "sdk/bin/vulkan-1.dll"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    library = (manifest_path.parent / manifest["ICD"]["library_path"]).resolve()
    report = {"format": "faset.windows-vulkan-probe", "version": 1, "status": "running",
              "loader": str(loader_path), "driver_manifest": str(manifest_path),
              "driver_library": str(library), "manifest": manifest,
              "loader_debug": os.environ.get("VK_LOADER_DEBUG"), "pointer_bits": ct.sizeof(ct.c_void_p) * 8}
    instance = ct.c_void_p()
    loader = None
    try:
        if not loader_path.is_file() or not library.is_file():
            raise RuntimeError("Prepared Vulkan loader or driver DLL is missing")
        if not manifest["ICD"]["library_path"].startswith(".\\"):
            raise RuntimeError("Windows ICD relative library path must use a native backslash")
        os.environ["VK_DRIVER_FILES"] = str(manifest_path)
        os.environ["VK_ICD_FILENAMES"] = str(manifest_path)
        # Direct DLL loading separates Windows dependency/architecture errors from
        # manifest discovery and Vulkan API compatibility errors in the next step.
        driver = ct.WinDLL(str(library))
        negotiate = driver.vk_icdNegotiateLoaderICDInterfaceVersion
        negotiate.argtypes = [ct.POINTER(ct.c_uint32)]
        negotiate.restype = ct.c_int32
        interface_version = ct.c_uint32(7)
        report["icd_negotiate_result"] = negotiate(ct.byref(interface_version))
        report["icd_interface_version"] = interface_version.value
        if report["icd_negotiate_result"] != 0:
            raise RuntimeError("SwiftShader rejected the loader interface negotiation")
        loader = ct.WinDLL(str(loader_path))
        enumerate_version = loader.vkEnumerateInstanceVersion
        enumerate_version.argtypes = [ct.POINTER(ct.c_uint32)]
        enumerate_version.restype = ct.c_int32
        api = ct.c_uint32()
        report["enumerate_version_result"] = enumerate_version(ct.byref(api))
        report["loader_api_version"] = version(api.value)
        create = loader.vkCreateInstance
        create.argtypes = [ct.POINTER(InstanceCreate), ct.c_void_p, ct.POINTER(ct.c_void_p)]
        create.restype = ct.c_int32
        app = Application(0, None, b"Faset Windows Vulkan probe", 1, b"Faset", 1, (1 << 22) | (3 << 12))
        info = InstanceCreate(1, None, 0, ct.pointer(app), 0, None, 0, None)
        result = create(ct.byref(info), None, ct.byref(instance))
        report["create_instance_result"] = result
        if result != 0:
            raise RuntimeError(f"vkCreateInstance for Vulkan 1.3 failed: {result}")
        enumerate_devices = loader.vkEnumeratePhysicalDevices
        enumerate_devices.argtypes = [ct.c_void_p, ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_void_p)]
        enumerate_devices.restype = ct.c_int32
        count = ct.c_uint32()
        if enumerate_devices(instance, ct.byref(count), None) != 0 or count.value == 0:
            raise RuntimeError("The loader found no usable Vulkan physical device")
        devices = (ct.c_void_p * count.value)()
        if enumerate_devices(instance, ct.byref(count), devices) != 0:
            raise RuntimeError("Cannot enumerate Vulkan physical devices")
        properties = loader.vkGetPhysicalDeviceProperties
        properties.argtypes = [ct.c_void_p, ct.c_void_p]
        properties.restype = None
        report["devices"] = []
        for device in devices:
            # VkPhysicalDeviceProperties starts with five uint32 values and a
            # 256-byte deviceName. Reserve ample room for the limits that follow.
            storage = (ct.c_uint64 * 512)()
            properties(device, ct.byref(storage))
            raw = bytes(storage)
            values = [int.from_bytes(raw[index:index + 4], "little") for index in range(0, 20, 4)]
            name = raw[20:276].split(b"\0", 1)[0].decode("utf-8")
            report["devices"].append({"name": name, "api_version": version(values[0]),
                                      "driver_version": values[1], "vendor_id": values[2],
                                      "device_id": values[3], "device_type": values[4]})
        if not any("SwiftShader" in device["name"] for device in report["devices"]):
            raise RuntimeError("Pinned SwiftShader was not among the enumerated devices")
        report["status"] = "passed"
    except Exception as error:
        report.update({"status": "failed", "error": str(error)})
        raise
    finally:
        if loader is not None and instance.value:
            destroy = loader.vkDestroyInstance
            destroy.argtypes = [ct.c_void_p, ct.c_void_p]
            destroy.restype = None
            destroy(instance, None)
        (cache / "probe.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
