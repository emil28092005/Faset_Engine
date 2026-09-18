"""Pure-Python GLB bundle publication; no Blender import, usable in unit tests."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import uuid


def read_glb(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) < 20:
        raise ValueError("Truncated GLB")
    magic, version, size = struct.unpack_from("<III", raw)
    if magic != 0x46546C67 or version != 2 or size != len(raw):
        raise ValueError("Invalid GLB 2 header")
    cursor, document = 12, None
    while cursor < len(raw):
        if cursor + 8 > len(raw):
            raise ValueError("Truncated GLB chunk header")
        length, kind = struct.unpack_from("<II", raw, cursor)
        cursor += 8
        if length % 4 or cursor + length > len(raw):
            raise ValueError("Invalid GLB chunk")
        if kind == 0x4E4F534A:
            if document is not None:
                raise ValueError("Duplicate GLB JSON chunk")
            document = json.loads(raw[cursor:cursor + length])
        cursor += length
    if document is None or document.get("asset", {}).get("version") != "2.0":
        raise ValueError("Missing glTF 2 document")
    return document


def _write_atomic(path: Path, raw: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".faset-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def publish_bundle(glb: Path, directory: Path, asset_id: str,
                   blender_version: str, source_hint: str = "") -> dict:
    """Immutable payload first, atomic manifest last; failures preserve previous manifest."""
    uuid.UUID(asset_id)
    document = read_glb(glb)
    for collection in ("buffers", "images"):
        for entry in document.get(collection, []):
            uri = entry.get("uri", "")
            if uri and not uri.startswith("data:"):
                raise ValueError("Bundle exporter requires embedded GLB dependencies")
    outputs, seen = [], set()
    for kind, collection in (("node", "nodes"), ("mesh", "meshes"), ("material", "materials")):
        for index, item in enumerate(document.get(collection, [])):
            identity = item.get("extras", {}).get("faset_id")
            # Some generated exporter subresources have no persistent datablock.
            if identity is None:
                if kind == "node" and "mesh" in item:
                    raise ValueError("Exported mesh node has no faset_id; enable custom properties")
                continue
            uuid.UUID(identity)
            key = (kind, identity)
            if key in seen:
                raise ValueError(f"DuplicateSourceId: {kind} {identity}")
            seen.add(key)
            outputs.append({"source_id": identity, "kind": kind, "name": item.get("name", ""),
                            "locator": f"/{collection}/{index}"})
    raw = glb.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    payload = f"payload/{digest}.glb"
    manifest = {
        "schema_version": 1, "asset_id": asset_id,
        "source": {"path_hint": source_hint},
        "exporter": {"blender_version": blender_version, "addon_version": "0.1.0"},
        "recipe": {"profile": "faset-gltf-static-v1", "export_extras": True,
                   "export_yup": True, "export_animations": False},
        "files": [{"path": payload, "sha256": digest, "size": len(raw)}],
        "outputs": outputs,
    }
    canonical = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    manifest["generation"] = hashlib.sha256(canonical).hexdigest()
    destination = directory / payload
    if destination.exists():
        if hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
            raise ValueError("Existing immutable payload is corrupt")
    else:
        _write_atomic(destination, raw)
    _write_atomic(directory / "manifest.json", json.dumps(manifest, sort_keys=True, indent=2).encode())
    return manifest
