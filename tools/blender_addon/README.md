# Faset GLB helper

Optional add-on for the **unmodified official Blender**. Ordinary `.gltf`/`.glb` imports do not need it.

Zip this directory as `blender_addon/` and install the ZIP through Blender's add-on preferences. Enable **Faset GLB Export**, then use **File → Export → Faset GLB Bundle**. The chosen directory receives immutable `payload/<sha256>.glb` files and `manifest.json`, replaced only after a complete export. Save the `.blend` after the first export to persist assigned custom IDs. The first profile exports static geometry/PBR, without animation playback.

Objects, mesh datablocks and materials receive `faset_id` custom properties. Renaming an object preserves its ID. Ambiguous duplicate IDs stop publication. After deliberately duplicating an object, select the new copy and run **Faset: New IDs for Selected**. This changes object identity and duplicated mesh datablock identity; shared meshes stay shared. Material duplicates can be repaired explicitly in Custom Properties. Linked-library/generated data without persistent identity is outside this first profile.

The engine imports `manifest.json` or ordinary GLB/glTF. Gameplay components, physics settings and instance overrides are engine-owned data. The exporter does not write them. Without IDs, the engine does not promise reliable matching after renaming internal parts. Arbitrary procedural Blender materials require baking or an explicit engine material; this profile does not claim pixel-identical shading.

`bundle.py` is independent of Blender and has executable fixture tests. The actual helper was also executed in unmodified Blender 4.5.3 LTS, followed by real Editor imports checking stable rename, geometry changes, deleted-output conflict and failure preservation. Reproduce with `python tools/verify_blender_roundtrip.py --blender PATH --editor PATH`. This background integration check does not substitute for testing every Blender UI/platform combination. See `docs/manual/editor/assets.md` for the user workflow and supported content profile.
