#!/usr/bin/env python3
"""Compile Slang and atomically publish SPIR-V with Faset reflection v1.

The layout fingerprint contains the declared shader interface and actual SPIR-V
push-constant decorations, never compiler formatting or generated ID numbers.
A failed compiler invocation or reflection conversion leaves previous files intact.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def value_type(value: dict) -> str:
    kind = value["kind"]
    if kind == "scalar":
        return value["scalarType"]
    if kind == "vector":
        return f"{value_type(value['elementType'])}x{value['elementCount']}"
    if kind == "matrix":
        return f"{value_type(value['elementType'])}x{value['rowCount']}x{value['columnCount']}"
    if kind == "array":
        return f"{value_type(value['elementType'])}[{value['elementCount']}]"
    raise ValueError(f"Unsupported reflected value type: {kind}")


def interface(fields: list[dict], category: str) -> tuple[list[dict], list[dict]]:
    locations, builtins = [], []
    def visit(field: dict, offset: int = 0) -> None:
        ty, binding = field["type"], field.get("binding", {})
        if ty["kind"] == "struct":
            for child in ty["fields"]:
                visit(child, offset + binding.get("index", 0))
        elif binding.get("kind") == category:
            locations.append({"location": offset + binding["index"], "type": value_type(ty)})
        elif field.get("semanticName", "").startswith("SV_"):
            builtins.append({"semantic": field["semanticName"], "type": value_type(ty)})
        else:
            raise ValueError("Shader interface field lacks a supported location or builtin")
    for field in fields:
        visit(field)
    return sorted(locations, key=lambda item: item["location"]), sorted(builtins, key=lambda item: item["semantic"])


def spirv_push_layout(data: bytes) -> list[dict]:
    if len(data) < 20 or len(data) % 4:
        raise ValueError("Malformed SPIR-V byte length")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203 or words[1] > 0x00010600 or not words[3] or words[4]:
        raise ValueError("Unsupported SPIR-V header")
    pointers, variables, decorations = {}, [], {}
    position = 5
    while position < len(words):
        count, opcode = words[position] >> 16, words[position] & 0xFFFF
        if not count or position + count > len(words):
            raise ValueError("Malformed SPIR-V instruction")
        operands = words[position + 1:position + count]
        if opcode == 32 and len(operands) == 3:  # OpTypePointer
            pointers[operands[0]] = (operands[1], operands[2])
        elif opcode == 59 and len(operands) >= 3 and operands[2] == 9:  # PushConstant OpVariable
            variables.append(operands[0])
        elif opcode == 72 and len(operands) >= 3:  # OpMemberDecorate
            member = decorations.setdefault(operands[0], {}).setdefault(operands[1], {})
            if operands[2] in (4, 5):
                member["matrix_layout"] = "row-major" if operands[2] == 4 else "column-major"
            elif operands[2] in (7, 35):
                if len(operands) != 4:
                    raise ValueError("Malformed SPIR-V member decoration")
                member["matrix_stride" if operands[2] == 7 else "offset"] = operands[3]
        position += count
    result = []
    for pointer in variables:
        storage, structure = pointers[pointer]
        if storage != 9:
            raise ValueError("Invalid SPIR-V push-constant pointer")
        members = [{"member": index, **layout} for index, layout in sorted(decorations.get(structure, {}).items())]
        result.append({"members": members})
    return result


def normalize(raw: dict, bytecode: bytes, entry_name: str) -> dict:
    entry = next(item for item in raw["entryPoints"] if item["name"] == entry_name)
    used = {item["name"]: bool(item["binding"].get("used", True)) for item in entry.get("bindings", [])}
    descriptors, constants = [], []
    for parameter in raw.get("parameters", []):
        binding, ty = parameter["binding"], parameter["type"]
        if binding["kind"] == "pushConstantBuffer":
            block = ty["elementType"]
            size = next(item["value"] for item in block["sizes"] if item["kind"] == "uniform")
            members = [{"name": member["name"], "offset": member["binding"]["offset"], "size": member["binding"]["size"], "type": value_type(member["type"])} for member in block["fields"]]
            constants.append({"name": parameter["name"], "offset": 0, "size": size, "members": sorted(members, key=lambda item: item["offset"])})
        elif binding["kind"] == "descriptorTableSlot":
            count = binding.get("count", 1)
            if ty["kind"] == "array":
                count = ty["elementCount"]
                ty = ty["elementType"]
            if ty["kind"] == "samplerState":
                descriptor_type = "sampler"
            elif ty["kind"] == "resource" and ty.get("baseShape") == "texture2D":
                descriptor_type = "sampled_image_2d"
            else:
                raise ValueError(f"Unsupported descriptor kind: {ty}")
            descriptors.append({"name": parameter["name"], "set": binding.get("space", 0), "binding": binding["index"], "type": descriptor_type, "count": count, "used": used.get(parameter["name"], True)})
        else:
            raise ValueError(f"Unsupported global shader binding: {binding['kind']}")
    inputs, input_builtins = interface(entry.get("parameters", []), "varyingInput")
    outputs, output_builtins = interface([entry["result"]] if "result" in entry else [], "varyingOutput")
    layout = {"stage": entry["stage"], "descriptors": sorted(descriptors, key=lambda item: (item["set"], item["binding"])), "push_constants": constants, "inputs": inputs, "outputs": outputs, "input_builtins": input_builtins, "output_builtins": output_builtins, "spirv_push_constants": spirv_push_layout(bytecode)}
    return {"format": "faset.shader-reflection", "version": 1, "source_entry": entry_name, "entry_point": "main", "matrix_convention": "column-major host matrices; Slang SPIR-V decorations recorded explicitly", "spirv_sha256": digest(bytecode), "layout_fingerprint": digest(canonical(layout)), "layout": layout}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--entry", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".shader-", dir=args.output) as temporary:
        directory = Path(temporary)
        spirv = directory / f"{args.entry}.spv"
        raw = directory / f"{args.entry}.slang-reflection.json"
        process = subprocess.run([args.compiler, str(args.source), "-entry", args.entry, "-target", "spirv", "-profile", "spirv_1_6", "-matrix-layout-column-major", "-o", str(spirv), "-reflection-json", str(raw)])
        if process.returncode:
            return process.returncode
        normalized = normalize(json.loads(raw.read_text(encoding="utf-8")), spirv.read_bytes(), args.entry)
        manifest = directory / f"{args.entry}.reflection.json"
        manifest.write_text(json.dumps(normalized, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        # Reflection is the commit record. A reader racing these replacements rejects
        # a hash mismatch and keeps its existing pipelines until the complete pair arrives.
        for artifact in (raw, spirv, manifest):
            os.replace(artifact, args.output / artifact.name)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f"Shader reflection error: {error}", file=sys.stderr)
        raise SystemExit(1)
