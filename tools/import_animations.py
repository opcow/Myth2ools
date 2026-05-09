"""
Blender importer for Myth II model-animation frame OBJs.

Open this script in Blender's Text Editor and run it. A file picker opens for
`assets/models/animations.json`; the importer can optionally bring in `map_combined.obj`,
then it imports the animation frame OBJs and keyframes visibility.
"""

import json
import math
from pathlib import Path

import bpy
from mathutils import Matrix
from bpy.props import BoolProperty, IntProperty, StringProperty
from bpy_extras.io_utils import ImportHelper


COLLECTION_NAME = "myth2_animations"


def import_obj(path):
    before = set(bpy.context.scene.objects)
    if hasattr(bpy.ops.wm, "obj_import"):
        bpy.ops.wm.obj_import(filepath=str(path))
    else:
        bpy.ops.import_scene.obj(filepath=str(path))
    after = set(bpy.context.scene.objects)
    imported = list(after - before)
    if not imported and bpy.context.object:
        imported = [bpy.context.object]
    return imported


def remove_collection(name):
    collection = bpy.data.collections.get(name)
    if not collection:
        return
    for child in list(collection.children):
        remove_collection(child.name)
    for obj in list(collection.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.collections.remove(collection)


def ensure_collection(name):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def move_to_collection(obj, collection):
    for existing in list(obj.users_collection):
        existing.objects.unlink(obj)
    collection.objects.link(obj)


def set_visible(obj, visible, frame):
    obj.hide_viewport = not visible
    obj.hide_render = not visible
    obj.keyframe_insert(data_path="hide_viewport", frame=frame)
    obj.keyframe_insert(data_path="hide_render", frame=frame)
    action = obj.animation_data.action if obj.animation_data else None
    for curve in getattr(action, "fcurves", []) or []:
        for key in curve.keyframe_points:
            key.interpolation = "CONSTANT"


def place_object(obj, anim, manifest):
    if manifest.get("coordinate_space") == "world":
        return

    half_w = float(manifest["map_width_cells"]) * 0.5
    half_h = float(manifest["map_height_cells"]) * 0.5
    cell_x = float(anim["x"])
    cell_y = float(anim["y"])
    cell_z = float(anim["z"])
    facing_deg = float(anim["facing_deg"])
    facing_rad = math.radians(facing_deg - 90.0)

    # Match export_models.cpp's map_combined.obj placement transform. Bake the
    # placement into the mesh and reset the object transform, so animated frame
    # OBJs have the same world-space vertex layout as map_combined.obj.
    placement = (
        Matrix.Translation((half_w - cell_x, cell_z, cell_y - half_h))
        @ Matrix.Rotation(-facing_rad, 4, "Y")
    )
    obj.data.transform(placement)
    obj.matrix_world = Matrix.Identity(4)


def hide_static_snapshot(anim):
    frames = anim.get("frames", [])
    if not frames:
        return
    prefix = f"{anim['tag']}_{frames[0]['model']}"
    for obj in bpy.context.scene.objects:
        if obj.name.startswith(prefix):
            obj.hide_viewport = True
            obj.hide_render = True


def import_map_if_requested(models_dir, collection):
    map_obj = models_dir / "map_combined.obj"
    if not map_obj.exists():
        return 0
    imported = import_obj(map_obj)
    for obj in imported:
        move_to_collection(obj, collection)
    return len(imported)


def import_animations(manifest_path, frame_scale, replace_existing, import_map, hide_snapshots):
    manifest_path = Path(manifest_path).resolve()
    models_dir = manifest_path.parent
    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    if replace_existing:
        remove_collection(COLLECTION_NAME)
    collection = bpy.data.collections.get(COLLECTION_NAME) or ensure_collection(COLLECTION_NAME)

    imported_count = 0
    if import_map:
        imported_count += import_map_if_requested(models_dir, collection)

    scene = bpy.context.scene
    scene.frame_start = 1
    scene.render.fps = 30

    last_frame = 1
    for anim in manifest.get("animations", []):
        if hide_snapshots:
            hide_static_snapshot(anim)

        frame_duration = max(1, int(anim.get("frame_duration_ticks", 1)) * frame_scale)
        anim_objects = []
        anim_collection = ensure_collection(f"{COLLECTION_NAME}_{anim['tag']}_{anim['marker_idx']}")
        collection.children.link(anim_collection)
        bpy.context.scene.collection.children.unlink(anim_collection)

        for frame_info in anim.get("frames", []):
            obj_path = models_dir / frame_info["obj"]
            imported = import_obj(obj_path)
            for obj in imported:
                obj.name = f"{anim['tag']}_frame{int(frame_info['frame']):02d}_{obj.name}"
                place_object(obj, anim, manifest)
                move_to_collection(obj, anim_collection)
                anim_objects.append((int(frame_info["frame"]), obj))
                imported_count += 1

        if not anim_objects:
            continue

        frame_count = max(index for index, _ in anim_objects) + 1
        anim_end = 1 + frame_count * frame_duration
        last_frame = max(last_frame, anim_end)

        for index, obj in anim_objects:
            start = 1 + index * frame_duration
            end = start + frame_duration
            set_visible(obj, False, 1)
            set_visible(obj, True, start)
            if index + 1 < frame_count:
                set_visible(obj, False, end)

    scene.frame_end = max(scene.frame_end, last_frame)
    scene.frame_set(1)
    return imported_count


class IMPORT_OT_myth2_animations(bpy.types.Operator, ImportHelper):
    bl_idname = "import_scene.myth2_animations"
    bl_label = "Import Myth II Animations"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".json"
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    import_map: BoolProperty(
        name="Import map_combined.obj",
        description="Import map_combined.obj from the same models folder before importing animations",
        default=False,
    )
    replace_existing: BoolProperty(
        name="Replace existing Myth II animation collection",
        description="Delete the previous myth2_animations collection before importing",
        default=True,
    )
    hide_snapshots: BoolProperty(
        name="Hide static animation snapshots",
        description="Hide first-frame objects that were imported from map_combined.obj",
        default=True,
    )
    frame_scale: IntProperty(
        name="Blender frames per Myth tick",
        description="Multiplier applied to frame_duration_ticks",
        default=1,
        min=1,
        max=10,
    )

    def execute(self, context):
        try:
            count = import_animations(
                self.filepath,
                self.frame_scale,
                self.replace_existing,
                self.import_map,
                self.hide_snapshots,
            )
        except Exception as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}

        self.report({"INFO"}, f"Imported {count} Myth II animation objects")
        return {"FINISHED"}


def menu_func_import(self, context):
    self.layout.operator(IMPORT_OT_myth2_animations.bl_idname, text="Myth II Animations (.json)")


def register():
    bpy.utils.register_class(IMPORT_OT_myth2_animations)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.utils.unregister_class(IMPORT_OT_myth2_animations)


if __name__ == "__main__":
    try:
        unregister()
    except Exception:
        pass
    register()
    bpy.ops.import_scene.myth2_animations("INVOKE_DEFAULT")
