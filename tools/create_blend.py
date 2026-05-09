"""
Create a Blender .blend scene from an extracted Myth II map folder.

Run through Blender:
  blender --background --python tools/create_blend.py -- <map_folder> [output.blend]
"""

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

import bpy

ROOT_COLLECTION = "myth2"

SOUND_HELPER_TEXT = r'''
import bpy
import aud

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


_CLASSES = (
    MYTH2_OT_play_selected_sound,
    MYTH2_OT_stop_selected_sound,
    MYTH2_PT_sound_preview,
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
        location = (
            float(entry.get("obj_x", entry.get("x", 0.0))),
            float(entry.get("obj_y", entry.get("z", 0.0))),
            float(entry.get("obj_z", entry.get("y", 0.0))),
        )
        bpy.ops.object.speaker_add(location=location)
        speaker = bpy.context.object
        speaker.name = f"{tag}_{entry.get('marker_idx', index)}_speaker"
        speaker.data.name = speaker.name
        speaker.data.sound = bpy.data.sounds.load(str(wav_path), check_existing=True)
        speaker.data.muted = True
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
            mat.use_nodes = True
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
    parser.add_argument("--no-water", action="store_true", help="Do not import assets/models/water.obj")
    parser.add_argument("--no-camera", action="store_true", help="Do not add a default camera/light")
    return parser.parse_args(argv)


def main():
    args = parse_args(sys.argv)
    map_folder = Path(args.map_folder).resolve()
    assets_dir = map_folder / "assets"
    models_dir = assets_dir / "models"
    sprites_dir = assets_dir / "sprites"
    sounds_dir = assets_dir / "sounds"
    output_blend = Path(args.output_blend).resolve() if args.output_blend else map_folder / f"{map_folder.name}.blend"

    if not models_dir.exists():
        raise SystemExit(f"Missing assets/models folder: {models_dir}")

    combined_obj = models_dir / "map_combined.obj"
    displacement_obj = models_dir / "displacement.obj"
    water_obj = models_dir / "water.obj"
    units_obj = sprites_dir / "units.obj"
    sounds_obj = sounds_dir / "sounds.obj"
    sounds_json = sounds_dir / "sounds.json"
    scenery_obj = sprites_dir / "scenery.obj"
    projectiles_obj = models_dir / "projectiles.obj"
    animations_json = models_dir / "animations.json"

    setup_scene()
    add_sound_helper_text()

    imported_combined = False
    combined_has_terrain = False
    if combined_obj.exists():
        import_combined_map(combined_obj)
        imported_combined = True
        combined_has_terrain = obj_has_terrain(combined_obj)

    if not imported_combined and displacement_obj.exists():
        import_into_collection(displacement_obj, "terrain")
    elif imported_combined and not combined_has_terrain and displacement_obj.exists():
        import_into_collection(displacement_obj, "terrain")

    if not args.no_water and water_obj.exists():
        import_into_collection(water_obj, "water")

    if units_obj.exists():
        group_imported_objects_by_tag(units_obj, "units")

    if sounds_obj.exists():
        group_imported_objects_by_tag(sounds_obj, "sounds")
    add_sound_speakers(sounds_json, map_folder)

    if scenery_obj.exists():
        enable_alpha_for_sprite_materials(group_imported_objects_by_tag(scenery_obj, "scenery"))

    if projectiles_obj.exists():
        group_imported_objects_by_tag(projectiles_obj, "projectiles")

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
