"""Recreate the demo's original exit arch with an unmodified Blender 4.5+.
Run: blender --background --factory-startup --python create.py -- ENGINE_ROOT
"""
import pathlib
import sys
import uuid
import bpy

root = pathlib.Path(sys.argv[sys.argv.index('--') + 1]).resolve()
output = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(root / 'tools'))
import blender_addon
from blender_addon.bundle import publish_bundle

blender_addon.register()
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
material = bpy.data.materials.new('Weathered stone')
material.diffuse_color = (0.22, 0.29, 0.34, 1)
material.use_nodes = True
shader = material.node_tree.nodes.get('Principled BSDF')
shader.inputs['Base Color'].default_value = material.diffuse_color
shader.inputs['Metallic'].default_value = 0.08
shader.inputs['Roughness'].default_value = 0.8
material['faset_id'] = '08046276-106a-4aeb-80f1-d4f02a2ffdc1'
namespace = uuid.UUID('73023ab9-8fe6-4d56-b136-c25c51510b72')
for name, position, size in [
    ('Left post', (0, -1.3, 1.45), (0.45, 0.35, 2.9)),
    ('Right post', (0, 1.3, 1.45), (0.45, 0.35, 2.9)),
    ('Lintel', (0, 0, 3.0), (0.52, 3.0, 0.35)),
]:
    bpy.ops.mesh.primitive_cube_add(location=position)
    obj = bpy.context.object
    obj.name = name
    obj.dimensions = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.data.materials.append(material)
    obj['faset_id'] = str(uuid.uuid5(namespace, name))
    obj.data['faset_id'] = str(uuid.uuid5(namespace, name + ' mesh'))
    bevel = obj.modifiers.new('Small stone bevel', 'BEVEL')
    bevel.width = 0.06
    bevel.segments = 1
bpy.context.scene['faset_asset_id'] = '5832763b-3ed0-44d6-9088-0b524f196a91'
bpy.ops.wm.save_as_mainfile(filepath=str(output / 'source.blend'))
raw = output / 'arch.glb'
bpy.ops.export_scene.gltf(filepath=str(raw), export_format='GLB', export_extras=True,
    export_yup=True, export_animations=False, export_materials='EXPORT', use_active_scene=True)
publish_bundle(raw, output, bpy.context.scene['faset_asset_id'], bpy.app.version_string, 'source.blend')
raw.unlink()
print('Faset exit arch recreated')
