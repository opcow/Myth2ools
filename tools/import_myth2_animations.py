"""
Import Myth II model-animation frames into Blender.

Usage inside Blender:
  1. Import out/<map>/models/map_combined.obj normally.
  2. Open this script in Blender's Text Editor.
  3. Set MANIFEST_PATH below to out/<map>/models/animations.json.
  4. Run Script, then press Play.

The script imports each frame OBJ listed in animations.json, places the frames
over the map, and keyframes viewport/render visibility so only one frame is
visible at a time.
"""

import json
import math
from pathlib import Path

import bpy


MANIFEST_PATH = r"out/le3e/models/animations.json"

# Blender frames per Myth tick. Myth II commonly runs simulation ticks at 30 Hz,
# so 1:1 gives a useful preview at 30 fps.
BLENDER_FRAMES_PER_MYTH_TICK = 1

# Set true if you want all imported frame objects collected under an Empty.
ADD_PARENT_EMPTY = True

# map_combined.obj includes the first animation frame as a static snapshot. Hide
# matching snapshot objects so only the keyframed frame stack is visible.
HIDE_STATIC_SNAPSHOT = True


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

    obj.location = (half_w - cell_x, cell_z, cell_y - half_h)
    obj.rotation_euler = (0.0, -math.radians(facing_deg - 90.0), 0.0)


def hide_static_snapshot(anim):
    frames = anim.get("frames", [])
    if not frames:
        return
    prefix = f"{anim['tag']}_{frames[0]['model']}"
    for obj in bpy.context.scene.objects:
        if obj.name.startswith(prefix):
            obj.hide_viewport = True
            obj.hide_render = True


def main():
    manifest_path = Path(MANIFEST_PATH)
    if not manifest_path.is_absolute():
        manifest_path = Path.cwd() / manifest_path
    manifest_path = manifest_path.resolve()
    models_dir = manifest_path.parent

    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    scene = bpy.context.scene
    scene.frame_start = 1

    parent = None
    if ADD_PARENT_EMPTY:
        parent = bpy.data.objects.new("myth2_animations", None)
        scene.collection.objects.link(parent)

    last_frame = 1
    for anim in manifest.get("animations", []):
        if HIDE_STATIC_SNAPSHOT:
            hide_static_snapshot(anim)

        frame_duration = max(
            1,
            int(anim.get("frame_duration_ticks", 1)) * BLENDER_FRAMES_PER_MYTH_TICK,
        )
        anim_objects = []

        for frame_info in anim.get("frames", []):
            obj_path = models_dir / frame_info["obj"]
            imported = import_obj(obj_path)
            for obj in imported:
                obj.name = f"{anim['tag']}_frame{int(frame_info['frame']):02d}_{obj.name}"
                place_object(obj, anim, manifest)
                if parent:
                    obj.parent = parent
                anim_objects.append((int(frame_info["frame"]), obj))

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
            set_visible(obj, False, end)

    scene.frame_end = max(scene.frame_end, last_frame)
    scene.frame_set(1)


if __name__ == "__main__":
    main()
