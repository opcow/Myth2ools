# Myth II Mesh Object Placement Format

Reverse-engineered from binary analysis of le3e (Willow Creek) and 85gy (Siege of Madrigal) mesh tags.

---

## Mesh Tag Layout

All offsets in the header at bytes [0x28], [0x38] etc. are **relative to the data start**, which is always at file offset **1024** (0x400). So absolute file offset = `header_field + 1024`.

```
[0x00:04]  mesh subgroup tag  (e.g. 'le3e', '85gi')
[0x04:08]  water/media tag
[0x08:0A]  submesh_width  (short)  — e.g. 6 for le3e
[0x0A:0C]  submesh_height (short)
[0x10:14]  cell_data_size = submesh_w*32 * submesh_h*32 * 12
[0x18:1C]  cell_data_offset = 1024 (always)
[0x1C:20]  total data size
[0x24:28]  unit_type_count
[0x28:2C]  unit_type_table_offset  (from data start)
[0x2C:30]  unit_type_table_size    = count * 32
[0x34:38]  object_instance_count
[0x38:3C]  object_instance_table_offset (from data start)
[0x3C:40]  object_instance_table_size   = count * 64
[0x44:48]  landscape/terrain .256 tag
[0x7C:80]  action section marker ('amdu' or 'amds')
[0x80:84]  action count
[0x84:88]  action table offset (from data start)
[0x88:8C]  action table size
```

The cell grid starts immediately after the 1024-byte header (at file offset 1024). Each cell is 12 bytes. The unit type table follows the cell grid.

---

## Unit Type Record (32 bytes)

```
[0:2]   w0 = object class:
            1  = unit/marker (mupt, nelf, musr, sarb, etc.)
            3  = monster (soul, dwar, arch, etc.)
            5  = effect/projectile (amri, ratr, etc.)
            6  = scenery/building (barn, farm, corn, oute, etc.)
            9  = netgame objective (wipp, etc.)
           11  = local controller (l0cg)
[4:8]   type_tag = 4-char Myth II tag ID
[28:30] instance count for this type
[30:32] unique sequential type index
```

---

## Object Instance Record (64 bytes)

```
[0:4]   flags (usually 0)
[4:6]   type_idx — index into unit type table
[6:8]   team (units) or variant (scenery)
[8:12]  x_world — 32-bit; integer part (>>16) gives X in grid-cell units minus map_base_x
[12:16] y_world — 32-bit; divide by 512 to get Y in grid-cell units
[16:20] z_world — 32-bit; divide by 512 to get Z (height) in grid-cell units
[20:24] facing  — fixed-point angle
[32:36] scale/render flags (0xC000C000 observed for scenery)
[60:62] next_object_index (linked list per cell, -1 = end)
[62:64] extra (0 for most scenery; extra bytes for some types)
```

### Coordinate System

- **X**: `cell_x = (x_world >> 16) - map_base_x`
  - `map_base_x` = minimum value of `(x_world >> 16)` across all instances in the map
  - Typical values: le3e base = 10000, so cell_x ∈ [0, 191] for a 192-cell map
- **Y**: `cell_y = y_world / 512.0`
  - Gives approximately cell index within [0, grid_height)
- **Z**: `z_world / 512.0` — height above some datum (not directly comparable to physicalHeight grid values, different scale)

### Cell Linked List

Each mesh cell stores `firstObjectIndex` (at byte offset 6 within the 12-byte cell record). Objects in the same cell form a singly-linked list via `next_object_index` at record bytes [60:62]. A value of -1 (0xFFFF) terminates the list.

---

## Scenery Objects (w0 = 6)

Scenery instance `type_tag` directly corresponds to `geom`, `mode`, and optionally `scen` tag subgroup IDs in the Myth II tag files. Example for Willow Creek (le3e):

| type_tag | geom tag | mode tag | Description |
|----------|----------|----------|-------------|
| `barn`   | yes      | yes      | Barn building |
| `farm`   | no       | yes      | Farmhouse |
| `corn`   | yes      | yes      | Corn stalk |
| `oute`   | yes      | yes      | Outpost building |
| `owen`   | yes      | yes      | Owen building variant |
| `ow02`   | yes      | yes      | Owen building variant 2 |
| `l1_t`   | no       | yes      | Tree (level 1) |
| `03si`   | no       | yes      | Sign/marker |

The `geom` tag contains the actual 3D geometry. The `mode` tag wraps it and links to the `.256` collection for textures.

---

## Geometry Tag Format (`geom`, group tag `0x67656F6D`)

From Malice source `geometry_definitions.h` and verified via model dump in `09 Needle Model Dump.txt`:

```
SIZEOF_STRUCT_GEOMETRY_DEFINITION = 128 bytes header
  [0:4]   flags
  [4:8]   collection_reference_tag  (4-char tag ID for .256 texture collection)
  [8:10]  material_count
  [10:12] vertex_count
  [12:14] surface_count
  [14:16] dependency_count
  [16:22] center (short_world_point3d: 3 × short)
  [22:24] height
  [24:28] obsolete
  [28:32] materials_offset (from start of tag data)
  [32:36] materials_size
  [36:40] materials* (runtime pointer, zero on disk)
  [40:44] vertices_offset
  [44:48] vertices_size
  [48:52] vertices* (runtime pointer)
  [52:56] surfaces_offset
  [56:60] surfaces_size
  [60:64] surfaces* (runtime pointer)
  [64:68] dependency_list_offset
  [68:72] dependency_size
  [72:76] dependency_list* (runtime pointer)
  [76:80] data_offset
  [80:84] data_size
  [84:88] data* (runtime pointer)
  [88:100] bounds (short_world_rectangle3d)
  [100:126] unused
  [126:128] collection_reference_index (runtime only)

SIZEOF_STRUCT_GEOMETRY_MATERIAL = 64 bytes
  [0:32]  name (null-terminated string, 32 chars max)
  [32:34] sequence_index
  [34:60] unused
  [60:62] collection_index (runtime)
  [62:64] color_table_index (runtime)
  (bitmap_index follows, runtime only)

SIZEOF_STRUCT_GEOMETRY_VERTEX = 6 bytes
  [0:6] short_world_point3d: x(short), y(short), z(short)

SIZEOF_STRUCT_GEOMETRY_SURFACE = 64 bytes
  [0:2]   flags
  [2:8]   normal (world_vector3d: 3 × int16 = 6 bytes)
  [8:12]  d (plane equation distance, int32)
  [12:24] bounds (short_world_rectangle3d: 6 × int16)
  [24:26] transparency (fixed_fraction)
  [26:28] material_index (int16)
  [28:52] corners[3] (3 × geometry_corner, each 8 bytes):
            [+0:+2] u (fixed_fraction = uint16, divide by 65536)
            [+2:+4] v (fixed_fraction = uint16, divide by 65536)
            [+4:+6] vertex_index (int16)
            [+6:+8] encoded_normal (uint16)
  [52:54] next_negative_surface_index (int16)
  [54:56] next_parallel_surface_index (int16)
  [56:58] next_positive_surface_index (int16)
```

All multi-byte integers are **big-endian** (PowerPC origin). UV coordinates are `fixed_fraction` in range [0.0, 1.0] stored as 0..65536 (divide by 65536 to get float UV).

---

## Model Tag Format (`mode`, group tag `0x6d6f6465`)

From Malice source `model_definitions.h`:

```
SIZEOF_STRUCT_MODEL_DEFINITION = 64 bytes header
  [0:4]   flags
  [4:8]   geometry_tag (4-char subgroup ID for the geom tag)
  [8:10]  geometry_index
  [10:12] model_geometry_vertex_count
  [12:14] model_permutation_count
  [14:16] model_cell_count
  [16:20] model_geometry_vertex_offset
  [20:24] model_geometry_vertex_size
  [24:28] model_permutations_offset
  [28:32] model_permutations_size
  [32:36] model_cell_offset
  [36:40] model_cell_size
  [40:44] data_offset
  [44:48] data_size

SIZEOF_STRUCT_MODEL_PERMUTATION_DATA = 64 bytes
  [0:2]   collection_reference_permutation
  [2:34]  permutations[32] (one per material slot)
  [34:60] unused
  [60:62] collection_index (runtime)
  [62:64] color_table_index (runtime)

SIZEOF_STRUCT_GEOMETRY_INDEX_DATA = 4 bytes (per vertex flag entry)
  [0:4]   flags (bit 0=deforms_mesh, bit 1=is_media, bit 2=wobbles_when_media)

SIZEOF_STRUCT_MODEL_MESH_DATA = 8 bytes (per mesh cell entry)
  [0:2]   x
  [2:4]   y
  [4:6]   flags (bit 0=replaces_mesh, bit 1=impassable_walking, bit 2=impassable_flying)
  [6:8]   unused
```

---

## Extraction Pipeline for 3D Scenery

1. **Parse mesh header** to get submesh dimensions and table offsets.
2. **Read unit type table** — filter for `w0 == 6` (scenery).
3. **Read object instance table** — filter for scenery type_idx entries, extract XY position and facing.
4. **For each unique scenery type_tag**:
   a. Find the `geom` tag (group `0x67656F6D`, subgroup = type_tag) in the Myth II tag files.
   b. Parse `geometry_definition` header to get material, vertex, and surface arrays.
   c. Extract vertices as `float3 = (x/WORLD_ONE, y/WORLD_ONE, z/WORLD_ONE)`.
   d. Extract surfaces — each has 3 corners with vertex_index and UV (divide fixed_fraction by 65536).
   e. Find the `collection_reference_tag` (the `.256` tag for textures).
   f. Extract the relevant bitmap frames from the `.256` tag for each material.
5. **Write per-type OBJ** with geometry, per-material UV groups, and MTL referencing extracted textures.
6. **Write a placement JSON or OBJ** listing all scenery instances with position and facing for import into Blender.

### WORLD_ONE

From the Malice source constants and the geometry dump in `09 Needle Model Dump.txt`, vertices for the Monument geometry range up to (512, 512, 2817). The model is described as fitting within the mesh cell system. This suggests `WORLD_ONE` ≈ 512–1024. The exact value needs empirical validation by comparing extracted vertex positions against expected real-world scale.
