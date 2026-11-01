"""
Il carro della fixture blender_tank.glb (Source/GeoTiles/TestData), fatto con
Blender: un glTF scritto da un esportatore VERO, non dai nostri test.

Uno scafo 6 x 3 x 1,5 m; sopra una torretta (perno a 1,9 m) con una canna
lunga 4 m verso il davanti di Blender (-Y); un'animazione che gira la torretta
di +90 gradi attorno all'alto in 2 s (fotogrammi 1..49 a 24 al secondo).
Esportato in .glb come fa 'geoworld convert-models' (animazioni, niente Draco).

    blender --background --factory-startup --python make_blender_tank.py -- <cartella>
    python make_blender_tank.py <cartella>       (con il modulo bpy: pip install bpy, Python 3.11)

La fixture e' stata fatta con Blender 5.0.1. geogltf_tests controlla che la
canna, a riposo verso +X dell'attore, finisca a SINISTRA (-Y): gli angoli di
Blender e di glTF sono matematici, come quelli di OpenFlight.
"""

import math
import os
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
out = argv[0] if argv else "."

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 24
scene.frame_start, scene.frame_end = 1, 49

bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, 0.75))
hull = bpy.context.object
hull.name = "scafo"
hull.scale = (3, 6, 1.5)
material = bpy.data.materials.new("verde")
material.use_nodes = True
material.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.1, 0.3, 0.1, 1)
hull.data.materials.append(material)
bpy.ops.object.transform_apply(scale=True)

bpy.ops.mesh.primitive_cylinder_add(radius=1.0, depth=0.8, location=(0, 0, 1.9))
turret = bpy.context.object
turret.name = "torretta"
bpy.ops.mesh.primitive_cube_add(size=1, location=(0, -3.0, 1.9))
gun = bpy.context.object
gun.name = "canna"
gun.scale = (0.2, 4.0, 0.2)
bpy.ops.object.transform_apply(scale=True)
gun.parent = turret
gun.matrix_parent_inverse = turret.matrix_world.inverted()
turret.parent = hull
turret.matrix_parent_inverse = hull.matrix_world.inverted()

turret.rotation_euler = (0, 0, 0)
turret.keyframe_insert("rotation_euler", frame=1)
turret.rotation_euler = (0, 0, math.radians(90))
turret.keyframe_insert("rotation_euler", frame=49)

bpy.ops.export_scene.gltf(filepath=os.path.join(out, "blender_tank.glb"), export_format="GLB", export_animations=True,
                          export_draco_mesh_compression_enable=False)
print("scritto", os.path.join(out, "blender_tank.glb"))
