import bpy, sys, pathlib, shutil, json
root=pathlib.Path(sys.argv[sys.argv.index('--')+1])
output=pathlib.Path(sys.argv[sys.argv.index('--')+2])
sys.path.insert(0,str(root/'tools'))
import blender_addon
blender_addon.register()
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.mesh.primitive_cube_add(location=(0,0,1))
obj=bpy.context.object;obj.name='Door panel'
obj['faset_id']='1c4b69fb-1219-4430-b046-8185eb265a0f'
obj.data['faset_id']='2072eb21-2558-41ee-a843-82e676e6a44e'
mat=bpy.data.materials.new('Brass');mat.diffuse_color=(0.8,0.5,0.12,1);mat['faset_id']='31dc2a5b-3d1c-41b8-b4ef-ddc626087458';obj.data.materials.append(mat)
bpy.context.scene['faset_asset_id']='54c039e4-c19f-4365-a9f8-a38416ec6e3f'
bpy.context.scene.frame_set(17,subframe=.25)
for stage in ('initial','renamed','removed'):
 directory=output/stage;directory.mkdir(parents=True,exist_ok=True)
 if stage=='renamed':
  obj.name='Renamed Door';obj.data.vertices[0].co.x-=.2
 if stage=='removed':
  bpy.data.objects.remove(obj,do_unlink=True)
 result=bpy.ops.export_scene.faset_bundle(filepath=str(directory/'manifest.json'))
 assert 'FINISHED' in result,result
 assert bpy.context.scene.frame_current==17 and bpy.context.scene.frame_subframe==.25
 if stage!='removed':
  assert bpy.context.view_layer.objects.active==obj and obj.select_get()
  bpy.ops.wm.save_as_mainfile(filepath=str(directory/'source.blend'))
print('FASET_BLENDER_ROUNDTRIP_EXPORT_OK')
