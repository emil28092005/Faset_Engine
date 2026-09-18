"""Faset asset export helper for the unmodified official Blender application."""
bl_info = {
    "name": "Faset GLB Export", "author": "Faset Engine", "version": (0, 1, 0),
    "blender": (4, 2, 0), "location": "File > Export > Faset GLB Bundle",
    "description": "Publish GLB with persistent custom IDs and an atomic manifest", "category": "Import-Export",
}
from pathlib import Path
import tempfile
import uuid
import bpy
from bpy.props import StringProperty
from bpy_extras.io_utils import ExportHelper
from .bundle import publish_bundle


def ensure_persistent_ids(context):
    """Assign only missing IDs. Ambiguous duplicates require an explicit user operation."""
    objects = list(context.scene.objects)
    meshes = list({obj.data for obj in objects if obj.type == "MESH"})
    materials = list({slot.material for obj in objects for slot in obj.material_slots if slot.material})
    groups = [objects, meshes, materials]
    for group in groups:
        seen = {}
        for block in group:
            if block.library:
                raise ValueError(f"Linked data needs local IDs before export: {block.name}")
            identity = block.get("faset_id")
            if not identity:
                identity = str(uuid.uuid4())
                block["faset_id"] = identity
            try:
                uuid.UUID(identity)
            except (ValueError, TypeError, AttributeError) as error:
                raise ValueError(f"Invalid faset_id on {block.name}") from error
            if identity in seen:
                raise ValueError(f"DuplicateSourceId: {seen[identity]} / {block.name}. "
                                 "Select the new copy and run Faset: New IDs for Selected.")
            seen[identity] = block.name
    if not context.scene.get("faset_asset_id"):
        context.scene["faset_asset_id"] = str(uuid.uuid4())
    return context.scene["faset_asset_id"]


class FASET_OT_new_selected_ids(bpy.types.Operator):
    bl_idname = "faset.new_selected_ids"
    bl_label = "Faset: New IDs for Selected"
    bl_description = "Explicitly fork object identities; shared mesh/material identities remain shared"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return bool(context.selected_objects)

    def execute(self, context):
        selected = set(context.selected_objects)
        for obj in selected:
            if obj.library:
                self.report({"ERROR"}, "Linked objects must be made local first")
                return {"CANCELLED"}
            obj["faset_id"] = str(uuid.uuid4())
        # A copied mesh owns its new identity; a genuinely shared mesh stays shared.
        for obj in selected:
            mesh = obj.data if obj.type == "MESH" else None
            if mesh and not mesh.library:
                duplicate = any(other is not mesh and other.get("faset_id") == mesh.get("faset_id")
                                for other in bpy.data.meshes)
                if duplicate:
                    mesh["faset_id"] = str(uuid.uuid4())
        return {"FINISHED"}


class FASET_OT_export(bpy.types.Operator, ExportHelper):
    bl_idname = "export_scene.faset_bundle"
    bl_label = "Faset GLB Bundle"
    filename_ext = ".json"
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    def execute(self, context):
        frame, subframe = context.scene.frame_current, context.scene.frame_subframe
        active = context.view_layer.objects.active
        selected = list(context.selected_objects)
        mode = active.mode if active else "OBJECT"
        try:
            asset_id = ensure_persistent_ids(context)
            directory = Path(self.filepath).resolve().parent
            with tempfile.TemporaryDirectory(prefix="faset-export-") as temporary:
                payload = Path(temporary) / "scene.glb"
                result = bpy.ops.export_scene.gltf(filepath=str(payload), export_format="GLB",
                    export_extras=True, export_yup=True, export_animations=False,
                    export_materials="EXPORT", use_selection=False, use_active_scene=True)
                if "FINISHED" not in result:
                    raise RuntimeError("Blender glTF export did not finish")
                manifest = publish_bundle(payload, directory, asset_id,
                                          bpy.app.version_string, bpy.data.filepath)
            self.report({"INFO"}, f"Published {manifest['generation'][:12]}; save .blend to persist IDs")
            return {"FINISHED"}
        except Exception as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        finally:
            context.scene.frame_set(frame, subframe=subframe)
            for obj in context.selected_objects:
                obj.select_set(False)
            for obj in selected:
                if obj.name in context.view_layer.objects:
                    obj.select_set(True)
            if active and active.name in context.view_layer.objects:
                context.view_layer.objects.active = active
                if active.mode != mode:
                    try:
                        bpy.ops.object.mode_set(mode=mode)
                    except RuntimeError:
                        pass


def menu_export(self, context):
    self.layout.operator(FASET_OT_export.bl_idname, text="Faset GLB Bundle (.json)")


def register():
    bpy.utils.register_class(FASET_OT_new_selected_ids)
    bpy.utils.register_class(FASET_OT_export)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    bpy.utils.unregister_class(FASET_OT_export)
    bpy.utils.unregister_class(FASET_OT_new_selected_ids)


if __name__ == "__main__":
    register()
