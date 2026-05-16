"""
Create a Blender .blend scene from an extracted Myth II map folder.

Run through Blender:
  blender --background --python tools/create_blend.py -- <map_folder> [output.blend]
"""

import argparse
import importlib.util
import json
import math
import re
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

ROOT_COLLECTION = "myth2"

SOUND_HELPER_TEXT = r'''
import bpy
import aud
import json
import re
from pathlib import Path
from mathutils import Vector

_myth2_sound_device = None
_myth2_sound_handle = None


def _selected_speaker(context):
    obj = context.object
    if not obj or obj.type != "SPEAKER":
        raise RuntimeError("Select a Myth II speaker object first")
    return obj


def _sound_path(obj):
    sound = obj.data.sound
    if sound and sound.filepath:
        return bpy.path.abspath(sound.filepath)

    paths = obj.get("myth2_audio_permutations", [])
    if paths:
        map_folder = obj.get("myth2_map_folder", "")
        return str(__import__("pathlib").Path(map_folder) / paths[0])

    raise RuntimeError("Selected speaker has no sound path")


def _is_terrain_object(obj):
    return obj and obj.type == "MESH" and obj.name.lower().startswith("terrain")


def _terrain_object():
    for obj in bpy.context.scene.objects:
        if _is_terrain_object(obj):
            return obj
    raise RuntimeError("Could not find terrain mesh object")


def _raycast_terrain(terrain, x, y):
    origin_world = Vector((x, y, 10000.0))
    direction_world = Vector((0.0, 0.0, -1.0))
    inv = terrain.matrix_world.inverted()
    origin_local = inv @ origin_world
    direction_local = (inv.to_3x3() @ direction_world).normalized()
    hit, location, _normal, _face_index = terrain.ray_cast(origin_local, direction_local, distance=20000.0)
    if not hit:
        return None
    return terrain.matrix_world @ location


def _world_bounds_min_z(obj):
    if obj.type == "MESH" and obj.bound_box:
        return min((obj.matrix_world @ Vector(corner)).z for corner in obj.bound_box)
    return obj.location.z


def _iter_unit_objects():
    for obj in bpy.context.scene.objects:
        if obj.get("myth2_asset_kind") == "unit":
            yield obj


def _iter_asset_objects(kind):
    for obj in bpy.context.scene.objects:
        if obj.get("myth2_asset_kind") == kind:
            yield obj


def _unit_output_path(context):
    active = context.object
    if active and active.get("myth2_map_folder"):
        base = Path(active["myth2_map_folder"])
    else:
        base = Path(bpy.path.abspath("//"))
    return base / "assets" / "sprites" / "units_edited.json"


def _map_folder_for_context(context):
    active = context.object
    if active and active.get("myth2_map_folder"):
        return Path(active["myth2_map_folder"])
    return Path(bpy.path.abspath("//"))


def _marker_cell_position(obj):
    dx = float(obj.location.x) - float(obj.get("myth2_origin_x", 0.0))
    dy = float(obj.location.y) - float(obj.get("myth2_origin_y", 0.0))
    dz = float(obj.location.z) - float(obj.get("myth2_origin_z", 0.0))
    x = float(obj.get("myth2_cell_x", 0.0)) - dx
    y = float(obj.get("myth2_cell_y", 0.0)) - dy
    z = float(obj.get("myth2_cell_z", 0.0)) + dz
    return x, y, z


class MYTH2_OT_play_selected_sound(bpy.types.Operator):
    bl_idname = "myth2.play_selected_sound"
    bl_label = "Myth II: Play Selected Sound"
    bl_description = "Play only the selected Myth II speaker sound"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.type == "SPEAKER"

    def execute(self, context):
        global _myth2_sound_device, _myth2_sound_handle
        obj = _selected_speaker(context)
        path = _sound_path(obj)
        if _myth2_sound_handle:
            _myth2_sound_handle.stop()
        if _myth2_sound_device is None:
            _myth2_sound_device = aud.Device()
        _myth2_sound_handle = _myth2_sound_device.play(aud.Sound(path))
        return {"FINISHED"}


class MYTH2_OT_stop_selected_sound(bpy.types.Operator):
    bl_idname = "myth2.stop_sound_preview"
    bl_label = "Myth II: Stop Sound Preview"
    bl_description = "Stop the current Myth II sound preview"
    bl_options = {"REGISTER"}

    def execute(self, context):
        global _myth2_sound_handle
        if _myth2_sound_handle:
            _myth2_sound_handle.stop()
            _myth2_sound_handle = None
        return {"FINISHED"}


class MYTH2_OT_export_unit_placements(bpy.types.Operator):
    bl_idname = "myth2.export_unit_placements"
    bl_label = "Myth II: Export Unit Placements"
    bl_description = "Write moved unit sprite marker positions for build_plugin --edit"
    bl_options = {"REGISTER"}

    selected_only: bpy.props.BoolProperty(
        name="Selected Only",
        default=False,
    )

    def execute(self, context):
        objects = list(context.selected_objects) if self.selected_only else list(_iter_unit_objects())
        units = []
        for obj in objects:
            if obj.get("myth2_asset_kind") != "unit":
                continue
            marker_idx = obj.get("myth2_marker_idx")
            if marker_idx is None:
                continue
            x, y, z = _marker_cell_position(obj)
            entry = {
                "tag": obj.get("myth2_tag", ""),
                "x": round(x, 4),
                "y": round(y, 4),
                "z": round(z, 4),
                "facing_deg": round(float(obj.get("myth2_facing_deg", 0.0)), 4),
            }
            if obj.get("myth2_new_unit") or re.search(r"\.\d{3}$", obj.name):
                entry["add"] = True
                entry["source_marker_idx"] = int(marker_idx)
            else:
                entry["marker_idx"] = int(marker_idx)
            units.append(entry)

        if not units:
            self.report({"WARNING"}, "No unit sprite marker objects found")
            return {"CANCELLED"}

        out_path = _unit_output_path(context)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps({"units": units}, indent=2) + "\n", encoding="utf-8")
        self.report({"INFO"}, f"Wrote {len(units)} unit placement(s) to {out_path}")
        return {"FINISHED"}


class MYTH2_OT_export_marker_placements(bpy.types.Operator):
    bl_idname = "myth2.export_marker_placements"
    bl_label = "Myth II: Export Marker Placements"
    bl_description = "Write moved Myth II marker positions for build_plugin --edit"
    bl_options = {"REGISTER"}

    asset_kind: bpy.props.StringProperty(default="scenery")
    array_name: bpy.props.StringProperty(default="scenery")
    relative_path: bpy.props.StringProperty(default="assets/sprites/scenery.json")
    selected_only: bpy.props.BoolProperty(
        name="Selected Only",
        default=False,
    )

    def execute(self, context):
        objects = list(context.selected_objects) if self.selected_only else list(_iter_asset_objects(self.asset_kind))
        entries = []
        for obj in objects:
            if obj.get("myth2_asset_kind") != self.asset_kind:
                continue
            marker_idx = obj.get("myth2_marker_idx")
            if marker_idx is None:
                continue
            x, y, z = _marker_cell_position(obj)
            entries.append({
                "tag": obj.get("myth2_tag", ""),
                "x": round(x, 4),
                "y": round(y, 4),
                "z": round(z, 4),
                "facing_deg": round(float(obj.get("myth2_facing_deg", 0.0)), 4),
                "marker_idx": int(marker_idx),
            })

        if not entries:
            self.report({"WARNING"}, f"No {self.asset_kind} marker objects found")
            return {"CANCELLED"}

        out_path = _map_folder_for_context(context) / self.relative_path
        out_path.parent.mkdir(parents=True, exist_ok=True)
        data = {}
        if out_path.exists():
            try:
                data = json.loads(out_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                data = {}
        existing = data.get(self.array_name, [])
        if isinstance(existing, list):
            by_marker = {
                int(item["marker_idx"]): dict(item)
                for item in existing
                if isinstance(item, dict) and "marker_idx" in item
            }
            order = [
                int(item["marker_idx"])
                for item in existing
                if isinstance(item, dict) and "marker_idx" in item
            ]
            for entry in entries:
                marker_idx = int(entry["marker_idx"])
                if marker_idx not in by_marker:
                    order.append(marker_idx)
                merged = by_marker.get(marker_idx, {})
                merged.update(entry)
                by_marker[marker_idx] = merged
            data[self.array_name] = [by_marker[idx] for idx in order if idx in by_marker]
        else:
            data[self.array_name] = entries
        out_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.report({"INFO"}, f"Wrote {len(entries)} {self.asset_kind} marker placement(s) to {out_path}")
        return {"FINISHED"}


class MYTH2_OT_mark_selected_units_new(bpy.types.Operator):
    bl_idname = "myth2.mark_selected_units_new"
    bl_label = "Myth II: Mark Selected Units As New"
    bl_description = "Export selected duplicated unit sprites as added unit markers"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        changed = 0
        for obj in context.selected_objects:
            if obj.get("myth2_asset_kind") != "unit":
                continue
            obj["myth2_new_unit"] = True
            changed += 1
        if not changed:
            self.report({"WARNING"}, "No unit sprite marker objects selected")
            return {"CANCELLED"}
        self.report({"INFO"}, f"Marked {changed} unit(s) as new")
        return {"FINISHED"}


class MYTH2_OT_mark_selected_units_existing(bpy.types.Operator):
    bl_idname = "myth2.mark_selected_units_existing"
    bl_label = "Myth II: Mark Selected Units As Existing"
    bl_description = "Export selected unit sprites as edits to their existing markers"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        changed = 0
        for obj in context.selected_objects:
            if obj.get("myth2_asset_kind") != "unit":
                continue
            obj["myth2_new_unit"] = False
            changed += 1
        if not changed:
            self.report({"WARNING"}, "No unit sprite marker objects selected")
            return {"CANCELLED"}
        self.report({"INFO"}, f"Marked {changed} unit(s) as existing")
        return {"FINISHED"}


class MYTH2_OT_drop_selected_to_terrain(bpy.types.Operator):
    bl_idname = "myth2.drop_selected_to_terrain"
    bl_label = "Myth II: Drop Selected To Terrain"
    bl_description = "Move selected objects vertically until they rest on the terrain mesh"
    bl_options = {"REGISTER", "UNDO"}

    mode: bpy.props.EnumProperty(
        name="Drop",
        items=(
            ("ORIGIN", "Origin", "Place the object's origin on terrain"),
            ("BOTTOM", "Bottom", "Place the object's lowest bounding-box point on terrain"),
        ),
        default="ORIGIN",
    )
    offset: bpy.props.FloatProperty(
        name="Offset",
        description="Vertical offset above terrain after dropping",
        default=0.0,
        soft_min=-10.0,
        soft_max=10.0,
    )

    @classmethod
    def poll(cls, context):
        return any(obj.type in {"MESH", "EMPTY", "SPEAKER"} for obj in context.selected_objects)

    def execute(self, context):
        terrain = _terrain_object()
        dropped = 0
        missed = 0
        for obj in context.selected_objects:
            if obj == terrain or _is_terrain_object(obj):
                continue
            if obj.type not in {"MESH", "EMPTY", "SPEAKER"}:
                continue
            hit = _raycast_terrain(terrain, obj.location.x, obj.location.y)
            if hit is None:
                missed += 1
                continue
            if self.mode == "ORIGIN":
                obj.location.z += (hit.z + self.offset) - obj.location.z
            else:
                obj.location.z += (hit.z + self.offset) - _world_bounds_min_z(obj)
            dropped += 1
        if dropped == 0:
            self.report({"WARNING"}, "No selected objects intersected the terrain ray")
            return {"CANCELLED"}
        if missed:
            self.report({"INFO"}, f"Dropped {dropped} object(s); {missed} missed terrain")
        else:
            self.report({"INFO"}, f"Dropped {dropped} object(s)")
        return {"FINISHED"}


class MYTH2_PT_sound_preview(bpy.types.Panel):
    bl_label = "Myth II Sound"
    bl_idname = "MYTH2_PT_sound_preview"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Myth II"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.type == "SPEAKER"

    def draw(self, context):
        layout = self.layout
        obj = context.object
        sound = obj.data.sound
        layout.label(text=obj.name)
        if sound:
            layout.label(text=sound.name)
        layout.operator("myth2.play_selected_sound", icon="PLAY")
        layout.operator("myth2.stop_sound_preview", icon="PAUSE")


class MYTH2_PT_terrain_tools(bpy.types.Panel):
    bl_label = "Myth II Terrain"
    bl_idname = "MYTH2_PT_terrain_tools"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Myth II"

    @classmethod
    def poll(cls, context):
        return context.selected_objects

    def draw(self, context):
        layout = self.layout
        op = layout.operator("myth2.drop_selected_to_terrain", text="Drop Origin To Terrain", icon="EMPTY_AXIS")
        op.mode = "ORIGIN"
        op.offset = 0.0
        op = layout.operator("myth2.drop_selected_to_terrain", text="Drop Bounds To Terrain", icon="TRIA_DOWN")
        op.mode = "BOTTOM"
        op.offset = 0.0


class MYTH2_PT_unit_tools(bpy.types.Panel):
    bl_label = "Myth II Units"
    bl_idname = "MYTH2_PT_unit_tools"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Myth II"

    @classmethod
    def poll(cls, context):
        return any(True for _obj in _iter_unit_objects())

    def draw(self, context):
        layout = self.layout
        layout.operator("myth2.export_unit_placements", text="Export All Unit Placements", icon="EXPORT")
        op = layout.operator("myth2.export_unit_placements", text="Export Selected Unit Placements", icon="RESTRICT_SELECT_OFF")
        op.selected_only = True
        layout.separator()
        layout.operator("myth2.mark_selected_units_new", text="Mark Selected As New", icon="ADD")
        layout.operator("myth2.mark_selected_units_existing", text="Mark Selected As Existing", icon="CHECKMARK")


class MYTH2_PT_scenery_tools(bpy.types.Panel):
    bl_label = "Myth II Scenery"
    bl_idname = "MYTH2_PT_scenery_tools"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Myth II"

    @classmethod
    def poll(cls, context):
        return any(True for _obj in _iter_asset_objects("scenery"))

    def draw(self, context):
        layout = self.layout
        op = layout.operator("myth2.export_marker_placements", text="Export All Scenery Placements", icon="EXPORT")
        op.asset_kind = "scenery"
        op.array_name = "scenery"
        op.relative_path = "assets/sprites/scenery.json"
        op = layout.operator("myth2.export_marker_placements", text="Export Selected Scenery Placements", icon="RESTRICT_SELECT_OFF")
        op.asset_kind = "scenery"
        op.array_name = "scenery"
        op.relative_path = "assets/sprites/scenery.json"
        op.selected_only = True


class MYTH2_PT_projectile_tools(bpy.types.Panel):
    bl_label = "Myth II Projectiles"
    bl_idname = "MYTH2_PT_projectile_tools"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Myth II"

    @classmethod
    def poll(cls, context):
        return any(True for _obj in _iter_asset_objects("projectile"))

    def draw(self, context):
        layout = self.layout
        op = layout.operator("myth2.export_marker_placements", text="Export All Projectile Placements", icon="EXPORT")
        op.asset_kind = "projectile"
        op.array_name = "projectiles"
        op.relative_path = "assets/models/projectiles.json"
        op = layout.operator("myth2.export_marker_placements", text="Export Selected Projectile Placements", icon="RESTRICT_SELECT_OFF")
        op.asset_kind = "projectile"
        op.array_name = "projectiles"
        op.relative_path = "assets/models/projectiles.json"
        op.selected_only = True


class MYTH2_PT_model_tools(bpy.types.Panel):
    bl_label = "Myth II Models"
    bl_idname = "MYTH2_PT_model_tools"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Myth II"

    @classmethod
    def poll(cls, context):
        return any(True for _obj in _iter_asset_objects("model"))

    def draw(self, context):
        layout = self.layout
        op = layout.operator("myth2.export_marker_placements", text="Export All Model Placements", icon="EXPORT")
        op.asset_kind = "model"
        op.array_name = "instances"
        op.relative_path = "placement.json"
        op = layout.operator("myth2.export_marker_placements", text="Export Selected Model Placements", icon="RESTRICT_SELECT_OFF")
        op.asset_kind = "model"
        op.array_name = "instances"
        op.relative_path = "placement.json"
        op.selected_only = True


_CLASSES = (
    MYTH2_OT_play_selected_sound,
    MYTH2_OT_stop_selected_sound,
    MYTH2_OT_export_unit_placements,
    MYTH2_OT_export_marker_placements,
    MYTH2_OT_mark_selected_units_new,
    MYTH2_OT_mark_selected_units_existing,
    MYTH2_OT_drop_selected_to_terrain,
    MYTH2_PT_sound_preview,
    MYTH2_PT_terrain_tools,
    MYTH2_PT_unit_tools,
    MYTH2_PT_scenery_tools,
    MYTH2_PT_projectile_tools,
    MYTH2_PT_model_tools,
)


def register():
    for cls in _CLASSES:
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_CLASSES):
        try:
            bpy.utils.unregister_class(cls)
        except RuntimeError:
            pass


if __name__ == "__main__":
    register()
'''


def add_sound_helper_text():
    text = bpy.data.texts.get("myth2_sound_tools.py")
    if not text:
        text = bpy.data.texts.new("myth2_sound_tools.py")
    text.clear()
    text.write(SOUND_HELPER_TEXT.lstrip())
    text.use_module = True


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


def ensure_collection(name, parent=None):
    collection = bpy.data.collections.get(name)
    if not collection:
        collection = bpy.data.collections.new(name)

    target_parent = parent or bpy.context.scene.collection
    if collection.name not in target_parent.children.keys():
        for existing_parent in list(bpy.data.collections):
            if collection.name in existing_parent.children.keys():
                existing_parent.children.unlink(collection)
        if collection.name in bpy.context.scene.collection.children.keys() and target_parent != bpy.context.scene.collection:
            bpy.context.scene.collection.children.unlink(collection)
        target_parent.children.link(collection)
    return collection


def root_collection():
    return ensure_collection(ROOT_COLLECTION)


def child_collection(name, parent=None):
    parent = parent or root_collection()
    return ensure_collection(name, parent)


def safe_collection_name(name):
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", name.strip())
    return cleaned or "unknown"


def unit_tag_from_object_name(name):
    base = name.split(".", 1)[0]
    if "_" in base:
        return safe_collection_name(base.rsplit("_", 1)[0])
    return safe_collection_name(base)


def group_imported_objects_by_tag(obj_path, root_name):
    root = child_collection(root_name)
    imported = import_obj(obj_path)
    for obj in imported:
        move_to_collection(obj, child_collection(unit_tag_from_object_name(obj.name), root))
    return imported


def marker_idx_from_object_name(name):
    base = name.split(".", 1)[0]
    if "_" not in base:
        return None
    tail = base.rsplit("_", 1)[1]
    return int(tail) if tail.isdigit() else None


def ordinal_idx_from_object_name(name):
    return marker_idx_from_object_name(name)


def apply_unit_metadata(objects, units_json, map_folder):
    if not units_json.exists():
        return
    with units_json.open("r", encoding="utf-8") as f:
        data = json.load(f)
    by_marker = {int(item["marker_idx"]): item for item in data.get("units", []) if "marker_idx" in item}
    for obj in objects:
        marker_idx = marker_idx_from_object_name(obj.name)
        if marker_idx is None or marker_idx not in by_marker:
            continue
        item = by_marker[marker_idx]
        obj["myth2_asset_kind"] = "unit"
        obj["myth2_marker_idx"] = marker_idx
        obj["myth2_tag"] = item.get("tag", "")
        obj["myth2_cell_x"] = float(item.get("x", 0.0))
        obj["myth2_cell_y"] = float(item.get("y", 0.0))
        obj["myth2_cell_z"] = float(item.get("z", 0.0))
        obj["myth2_facing_deg"] = float(item.get("facing_deg", 0.0))
        obj["myth2_map_folder"] = str(map_folder)
        origin = object_bottom_center_world(obj)
        move_origin_to_world(obj, origin)
        obj["myth2_origin_x"] = float(obj.location.x)
        obj["myth2_origin_y"] = float(obj.location.y)
        obj["myth2_origin_z"] = float(obj.location.z)


def apply_marker_metadata(objects, metadata_json, array_name, asset_kind, map_folder):
    if not metadata_json.exists():
        return
    with metadata_json.open("r", encoding="utf-8") as f:
        data = json.load(f)
    by_marker = {int(item["marker_idx"]): item for item in data.get(array_name, []) if "marker_idx" in item}
    for obj in objects:
        marker_idx = marker_idx_from_object_name(obj.name)
        if marker_idx is None or marker_idx not in by_marker:
            continue
        item = by_marker[marker_idx]
        obj["myth2_asset_kind"] = asset_kind
        obj["myth2_marker_idx"] = marker_idx
        obj["myth2_tag"] = item.get("tag", "")
        obj["myth2_cell_x"] = float(item.get("x", 0.0))
        obj["myth2_cell_y"] = float(item.get("y", 0.0))
        obj["myth2_cell_z"] = float(item.get("z", 0.0))
        obj["myth2_facing_deg"] = float(item.get("facing_deg", 0.0))
        obj["myth2_map_folder"] = str(map_folder)
        origin = object_bottom_center_world(obj)
        move_origin_to_world(obj, origin)
        obj["myth2_origin_x"] = float(obj.location.x)
        obj["myth2_origin_y"] = float(obj.location.y)
        obj["myth2_origin_z"] = float(obj.location.z)


def apply_ordered_marker_metadata(objects, metadata_json, array_name, asset_kind, map_folder):
    if not metadata_json.exists():
        return
    with metadata_json.open("r", encoding="utf-8") as f:
        data = json.load(f)
    entries = data.get(array_name, [])
    for obj in objects:
        if obj.name.lower() == "terrain" or obj.name.lower().startswith("terrain."):
            continue
        ordinal_idx = ordinal_idx_from_object_name(obj.name)
        if ordinal_idx is None or ordinal_idx < 0 or ordinal_idx >= len(entries):
            continue
        item = entries[ordinal_idx]
        obj["myth2_asset_kind"] = asset_kind
        obj["myth2_marker_idx"] = int(item.get("marker_idx", -1))
        obj["myth2_tag"] = item.get("tag", "")
        obj["myth2_cell_x"] = float(item.get("x", 0.0))
        obj["myth2_cell_y"] = float(item.get("y", 0.0))
        obj["myth2_cell_z"] = float(item.get("z", 0.0))
        obj["myth2_facing_deg"] = float(item.get("facing_deg", 0.0))
        obj["myth2_map_folder"] = str(map_folder)
        origin = object_bottom_center_world(obj)
        move_origin_to_world(obj, origin)
        obj["myth2_origin_x"] = float(obj.location.x)
        obj["myth2_origin_y"] = float(obj.location.y)
        obj["myth2_origin_z"] = float(obj.location.z)


def object_bottom_center_world(obj):
    if obj.type == "MESH" and obj.bound_box:
        corners = [obj.matrix_world @ Vector(corner) for corner in obj.bound_box]
        min_x = min(c.x for c in corners)
        max_x = max(c.x for c in corners)
        min_y = min(c.y for c in corners)
        max_y = max(c.y for c in corners)
        min_z = min(c.z for c in corners)
        return Vector(((min_x + max_x) * 0.5, (min_y + max_y) * 0.5, min_z))
    return obj.location.copy()


def move_origin_to_world(obj, origin):
    old_matrix = obj.matrix_world.copy()
    new_matrix = old_matrix.copy()
    new_matrix.translation = origin
    if obj.type == "MESH" and obj.data:
        obj.data.transform(new_matrix.inverted() @ old_matrix)
    obj.matrix_world = new_matrix


def add_sound_speakers(sounds_json, map_folder):
    if not sounds_json.exists():
        return []
    with sounds_json.open("r", encoding="utf-8") as f:
        data = json.load(f)

    root = child_collection("sounds")
    speakers = []
    for index, entry in enumerate(data.get("sounds", [])):
        audio = entry.get("audio") or []
        if not audio:
            continue
        wav_path = map_folder / audio[0].get("path", "")
        if not wav_path.exists():
            continue

        tag = safe_collection_name(entry.get("tag", "sound"))
        collection = child_collection(tag, root)
        obj_x = float(entry.get("obj_x", entry.get("x", 0.0)))
        obj_y = float(entry.get("obj_y", entry.get("z", 0.0)))
        obj_z = float(entry.get("obj_z", entry.get("y", 0.0)))
        location = (obj_x, -obj_z, obj_y)
        bpy.ops.object.speaker_add(location=location)
        speaker = bpy.context.object
        speaker.rotation_euler = (math.pi, 0.0, 0.0)
        speaker.name = f"{tag}_{entry.get('marker_idx', index)}_speaker"
        speaker.data.name = speaker.name
        speaker.data.sound = bpy.data.sounds.load(str(wav_path), check_existing=True)
        speaker.data.muted = True
        speaker.data.cone_angle_inner = math.tau
        speaker.data.cone_angle_outer = math.tau
        speaker["myth2_audio_permutations"] = [item.get("path", "") for item in audio]
        speaker["myth2_map_folder"] = str(map_folder)
        move_to_collection(speaker, collection)
        speakers.append(speaker)
    return speakers


def enable_alpha_for_sprite_materials(objects):
    for obj in objects:
        for slot in getattr(obj, "material_slots", []):
            mat = slot.material
            if not mat or not mat.name.endswith("_sprite"):
                continue
            mat.blend_method = "CLIP"
            ensure_material_nodes(mat)
            if hasattr(mat, "show_transparent_back"):
                mat.show_transparent_back = True


def enable_texture_alpha(objects, blend_method="CLIP"):
    for obj in objects:
        for slot in getattr(obj, "material_slots", []):
            mat = slot.material
            if not mat:
                continue
            node_tree = ensure_material_nodes(mat)
            if not node_tree:
                continue
            nodes = node_tree.nodes
            links = node_tree.links
            bsdf = nodes.get("Principled BSDF")
            if not bsdf:
                continue
            image_node = None
            for node in nodes:
                if node.type == "TEX_IMAGE":
                    image_node = node
                    break
            if not image_node or "Alpha" not in image_node.outputs:
                continue
            mat.blend_method = blend_method
            alpha_input = bsdf.inputs.get("Alpha")
            if alpha_input:
                for link in list(alpha_input.links):
                    links.remove(link)
                links.new(image_node.outputs["Alpha"], alpha_input)
            if hasattr(mat, "show_transparent_back"):
                mat.show_transparent_back = True


def enable_alpha_for_objects(objects, blend_method="CLIP"):
    for obj in objects:
        for slot in getattr(obj, "material_slots", []):
            mat = slot.material
            if not mat:
                continue
            mat.blend_method = blend_method
            ensure_material_nodes(mat)
            if hasattr(mat, "show_transparent_back"):
                mat.show_transparent_back = True


def prepare_fence_materials(objects):
    for obj in objects:
        for slot in getattr(obj, "material_slots", []):
            mat = slot.material
            if not mat:
                continue
            mat.blend_method = "BLEND"
            node_tree = ensure_material_nodes(mat)
            if not node_tree:
                continue
            nodes = node_tree.nodes
            links = node_tree.links
            bsdf = nodes.get("Principled BSDF")
            if not bsdf:
                continue
            image_node = None
            for node in nodes:
                if node.type == "TEX_IMAGE":
                    image_node = node
                    break
            if not image_node:
                continue
            image_node.interpolation = "Closest"
            invert = nodes.get("Myth2FenceAlphaInvert")
            if not invert:
                invert = nodes.new("ShaderNodeInvert")
                invert.name = "Myth2FenceAlphaInvert"
                invert.label = "Myth2FenceAlphaInvert"
                invert.location = (image_node.location.x + 220.0, image_node.location.y - 180.0)
            invert.inputs["Fac"].default_value = 1.0

            separate = nodes.get("Myth2FenceSeparateRGB")
            if not separate:
                separate = nodes.new("ShaderNodeSeparateColor")
                separate.name = "Myth2FenceSeparateRGB"
                separate.label = "Myth2FenceSeparateRGB"
                separate.mode = "RGB"
                separate.location = (image_node.location.x + 220.0, image_node.location.y + 80.0)

            blue_gt = nodes.get("Myth2FenceBlueGT")
            if not blue_gt:
                blue_gt = nodes.new("ShaderNodeMath")
                blue_gt.name = "Myth2FenceBlueGT"
                blue_gt.label = "Myth2FenceBlueGT"
                blue_gt.operation = "GREATER_THAN"
                blue_gt.location = (separate.location.x + 220.0, separate.location.y + 80.0)
            blue_gt.inputs[1].default_value = 0.95

            red_lt = nodes.get("Myth2FenceRedLT")
            if not red_lt:
                red_lt = nodes.new("ShaderNodeMath")
                red_lt.name = "Myth2FenceRedLT"
                red_lt.label = "Myth2FenceRedLT"
                red_lt.operation = "LESS_THAN"
                red_lt.location = (separate.location.x + 220.0, separate.location.y)
            red_lt.inputs[1].default_value = 0.05

            green_lt = nodes.get("Myth2FenceGreenLT")
            if not green_lt:
                green_lt = nodes.new("ShaderNodeMath")
                green_lt.name = "Myth2FenceGreenLT"
                green_lt.label = "Myth2FenceGreenLT"
                green_lt.operation = "LESS_THAN"
                green_lt.location = (separate.location.x + 220.0, separate.location.y - 80.0)
            green_lt.inputs[1].default_value = 0.05

            blue_and_red = nodes.get("Myth2FenceBlueAndRed")
            if not blue_and_red:
                blue_and_red = nodes.new("ShaderNodeMath")
                blue_and_red.name = "Myth2FenceBlueAndRed"
                blue_and_red.label = "Myth2FenceBlueAndRed"
                blue_and_red.operation = "MULTIPLY"
                blue_and_red.location = (blue_gt.location.x + 220.0, blue_gt.location.y)

            blue_mask = nodes.get("Myth2FenceBlueMask")
            if not blue_mask:
                blue_mask = nodes.new("ShaderNodeMath")
                blue_mask.name = "Myth2FenceBlueMask"
                blue_mask.label = "Myth2FenceBlueMask"
                blue_mask.operation = "MULTIPLY"
                blue_mask.location = (blue_and_red.location.x + 220.0, blue_and_red.location.y - 40.0)

            color_mix = nodes.get("Myth2FenceColorMix")
            if not color_mix:
                color_mix = nodes.new("ShaderNodeMix")
                color_mix.name = "Myth2FenceColorMix"
                color_mix.label = "Myth2FenceColorMix"
                color_mix.data_type = "RGBA"
                color_mix.location = (blue_mask.location.x + 220.0, image_node.location.y)
            color_mix.inputs["B"].default_value = (0.0, 0.0, 0.0, 1.0)

            alpha_input = bsdf.inputs.get("Alpha")
            base_color_input = bsdf.inputs.get("Base Color")
            if base_color_input:
                for link in list(base_color_input.links):
                    links.remove(link)
            if alpha_input:
                for link in list(alpha_input.links):
                    links.remove(link)
            links.new(image_node.outputs["Color"], separate.inputs["Color"])
            links.new(separate.outputs["Blue"], blue_gt.inputs[0])
            links.new(separate.outputs["Red"], red_lt.inputs[0])
            links.new(separate.outputs["Green"], green_lt.inputs[0])
            links.new(blue_gt.outputs["Value"], blue_and_red.inputs[0])
            links.new(red_lt.outputs["Value"], blue_and_red.inputs[1])
            links.new(blue_and_red.outputs["Value"], blue_mask.inputs[0])
            links.new(green_lt.outputs["Value"], blue_mask.inputs[1])
            links.new(blue_mask.outputs["Value"], color_mix.inputs["Factor"])
            links.new(image_node.outputs["Color"], color_mix.inputs["A"])
            links.new(image_node.outputs["Alpha"], invert.inputs["Color"])
            if base_color_input:
                links.new(color_mix.outputs["Result"], base_color_input)
            if alpha_input:
                links.new(invert.outputs["Color"], alpha_input)
            if hasattr(mat, "show_transparent_back"):
                mat.show_transparent_back = True


def ensure_material_nodes(mat):
    if mat.node_tree:
        return mat.node_tree
    if hasattr(mat, "use_nodes"):
        mat.use_nodes = True
    return mat.node_tree


def set_material_alpha(objects, alpha, blend_method="BLEND"):
    for obj in objects:
        for slot in getattr(obj, "material_slots", []):
            mat = slot.material
            if not mat:
                continue
            mat.diffuse_color = (mat.diffuse_color[0], mat.diffuse_color[1], mat.diffuse_color[2], alpha)
            mat.blend_method = blend_method
            node_tree = ensure_material_nodes(mat)
            bsdf = node_tree.nodes.get("Principled BSDF") if node_tree else None
            if bsdf:
                if "Alpha" in bsdf.inputs:
                    bsdf.inputs["Alpha"].default_value = alpha
                if "Base Color" in bsdf.inputs:
                    base = bsdf.inputs["Base Color"].default_value
                    bsdf.inputs["Base Color"].default_value = (base[0], base[1], base[2], alpha)
            if hasattr(mat, "show_transparent_back"):
                mat.show_transparent_back = True


def clear_empty_top_level_collections():
    root = root_collection()
    for collection in list(bpy.context.scene.collection.children):
        if collection == root:
            continue
        if not collection.objects and not collection.children:
            bpy.context.scene.collection.children.unlink(collection)


def ensure_animation_root():
    collection = bpy.data.collections.get("myth2_animations")
    root = root_collection()
    if collection:
        if collection.name in bpy.context.scene.collection.children.keys():
            bpy.context.scene.collection.children.unlink(collection)
        if collection.name not in root.children.keys():
            root.children.link(collection)
        return collection
    return child_collection("myth2_animations", root)


def move_to_collection(obj, collection):
    for existing in list(obj.users_collection):
        existing.objects.unlink(obj)
    collection.objects.link(obj)


def import_into_collection(path, collection_name):
    collection = child_collection(collection_name)
    imported = import_obj(path)
    for obj in imported:
        move_to_collection(obj, collection)
    return imported


def import_combined_map(path):
    imported = import_obj(path)
    terrain_collection = child_collection("terrain")
    models_collection = child_collection("models")
    for obj in imported:
        name = obj.name.lower()
        if name == "terrain" or name.startswith("terrain."):
            move_to_collection(obj, terrain_collection)
        else:
            move_to_collection(obj, models_collection)
    return imported


def imported_model_objects(objects):
    return [
        obj for obj in objects
        if obj.name.lower() != "terrain" and not obj.name.lower().startswith("terrain.")
    ]


def obj_has_terrain(path):
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                stripped = line.strip()
                if stripped in ("o terrain", "g terrain"):
                    return True
    except OSError:
        return False
    return False


def load_animation_importer(script_dir):
    importer_path = script_dir / "import_animations.py"
    if not importer_path.exists():
        return None
    spec = importlib.util.spec_from_file_location("myth2_import_animations", importer_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def setup_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_set(1)
    root_collection()


def set_view_defaults():
    bpy.ops.object.light_add(type="SUN", location=(0.0, 0.0, 10.0))
    bpy.context.object.name = "sun"
    bpy.context.object.data.energy = 3.0

    bpy.ops.object.camera_add(location=(0.0, -80.0, 60.0), rotation=(1.1, 0.0, 0.0))
    bpy.context.scene.camera = bpy.context.object


def parse_args(argv):
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []

    parser = argparse.ArgumentParser(description="Create a .blend from an extracted Myth II map folder.")
    parser.add_argument("map_folder", help="Extracted map folder")
    parser.add_argument("output_blend", nargs="?", help="Output .blend path")
    parser.add_argument("--no-animations", action="store_true", help="Do not import animations.json frame OBJs")
    parser.add_argument("--no-water", action="store_true", help="Do not import assets/terrain/water.obj")
    parser.add_argument("--no-camera", action="store_true", help="Do not add a default camera/light")
    return parser.parse_args(argv)


def main():
    args = parse_args(sys.argv)
    map_folder = Path(args.map_folder).resolve()
    assets_dir = map_folder / "assets"
    models_dir = assets_dir / "models"
    terrain_dir = assets_dir / "terrain"
    sprites_dir = assets_dir / "sprites"
    sounds_dir = assets_dir / "sounds"
    output_blend = Path(args.output_blend).resolve() if args.output_blend else map_folder / f"{map_folder.name}.blend"

    if not models_dir.exists():
        raise SystemExit(f"Missing assets/models folder: {models_dir}")

    combined_obj = terrain_dir / "map_combined.obj"
    displacement_obj = terrain_dir / "displacement.obj"
    water_obj = terrain_dir / "water.obj"
    fences_obj = terrain_dir / "fences.obj"
    units_obj = sprites_dir / "units.obj"
    sounds_obj = sounds_dir / "sounds.obj"
    sounds_json = sounds_dir / "sounds.json"
    units_json = sprites_dir / "units.json"
    scenery_json = sprites_dir / "scenery.json"
    scenery_obj = sprites_dir / "scenery.obj"
    projectiles_obj = models_dir / "projectiles.obj"
    projectiles_json = models_dir / "projectiles.json"
    placement_json = map_folder / "placement.json"
    animations_json = models_dir / "animations.json"

    setup_scene()
    add_sound_helper_text()

    imported_combined = False
    combined_has_terrain = False
    if combined_obj.exists():
        combined_objects = import_combined_map(combined_obj)
        apply_ordered_marker_metadata(imported_model_objects(combined_objects), placement_json, "instances", "model", map_folder)
        enable_texture_alpha(imported_model_objects(combined_objects))
        imported_combined = True
        combined_has_terrain = obj_has_terrain(combined_obj)

    if not imported_combined and displacement_obj.exists():
        import_into_collection(displacement_obj, "terrain")
    elif imported_combined and not combined_has_terrain and displacement_obj.exists():
        import_into_collection(displacement_obj, "terrain")

    if not args.no_water and water_obj.exists():
        set_material_alpha(import_into_collection(water_obj, "water"), 0.25)

    if fences_obj.exists():
        fence_objects = import_into_collection(fences_obj, "fences")
        enable_alpha_for_objects(fence_objects, blend_method="BLEND")
        prepare_fence_materials(fence_objects)

    if units_obj.exists():
        unit_objects = group_imported_objects_by_tag(units_obj, "units")
        enable_texture_alpha(unit_objects)
        apply_unit_metadata(unit_objects, units_json, map_folder)

    sound_speakers = add_sound_speakers(sounds_json, map_folder)
    if not sound_speakers and sounds_obj.exists():
        group_imported_objects_by_tag(sounds_obj, "sounds")

    if scenery_obj.exists():
        scenery_objects = group_imported_objects_by_tag(scenery_obj, "scenery")
        enable_texture_alpha(scenery_objects)
        apply_marker_metadata(scenery_objects, scenery_json, "scenery", "scenery", map_folder)
        enable_alpha_for_sprite_materials(scenery_objects)

    if projectiles_obj.exists():
        projectile_objects = group_imported_objects_by_tag(projectiles_obj, "projectiles")
        enable_texture_alpha(projectile_objects)
        apply_marker_metadata(projectile_objects, projectiles_json, "projectiles", "projectile", map_folder)

    if not args.no_animations and animations_json.exists():
        ensure_animation_root()
        importer = load_animation_importer(Path(__file__).resolve().parent)
        if importer:
            importer.import_animations(
                str(animations_json),
                frame_scale=1,
                replace_existing=True,
                import_map=False,
                hide_snapshots=True,
            )
            ensure_animation_root()

    if not args.no_camera:
        set_view_defaults()

    clear_empty_top_level_collections()

    output_blend.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(output_blend))
    print(f"Saved {output_blend}")


if __name__ == "__main__":
    main()
