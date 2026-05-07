# Myth II 3D Model Extraction & Placement

Findings from reverse-engineering the le3e (Willow Creek) mesh tag and tag files.
All integers are big-endian (PowerPC origin).

---

## Marker / Instance Record (64 bytes)

The object instance table in the mesh tag (`mesh_header[0x38]` offset, `mesh_header[0x34]` count)
holds one `marker_data` record per placed object, of every type.

```
[0:4]   flags (uint32)
[4:6]   type (int16) — marker type enum:
            0  = team start
            1  = scenery collision marker
            3  = monster
            5  = effect
            6  = _marker_model  ← placed 3D scenery with geometry
            9  = netgame objective
           11  = local controller
[6:8]   palette_index (int16) — type-relative index into unit type palette
[8:10]  identifier (int16)
[10:12] minimum_difficulty_level (int16)
[12:16] position.x (int32, world_distance)
[16:20] position.y (int32, world_distance)
[20:24] position.z (int32, world_distance = height above datum)
[24:30] velocity (3 × int16)
[30:32] height_above_ground (int16)
[32:34] yaw (uint16, 0..65535 = full 360°)
[34:36] pitch (uint16)
[36:52] user_data[16] (uint8 × 16)
[52:54] roll (uint16)
[54:56] unused
[56:60] render_chain pointer (zeroed on disk)
[60:62] data_index (int16)
[62:64] data_identifier (int16)
```

Only markers with `type == 6` (_marker_model) are placed 3D scenery instances.

### Coordinate System

`WORLD_ONE = 512` (WORLD_FRACTIONAL_BITS = 9).

```
cell_x = position.x / 512.0
cell_y = position.y / 512.0
cell_z = position.z / 512.0   (height, matches terrain physical_height scale)
facing_deg = (yaw / 65536.0) * 360.0
```

### palette_index Resolution

`palette_index` is **type-relative**: it is the Nth entry among all unit type palette records
where `palette_entry.w0 == marker.type`. To resolve it:

1. Build a per-type ordered list from the unit type table.
2. For a marker with `(type=T, palette_index=N)`, take the Nth entry in the list for type T.

This matches the Myth II engine's save/load format (confirmed in TMeshForm.cpp).

### user_data — Permutation Index

For `_marker_model` (type 6) markers, `user_data[1]` is the **0-based permutation index**
selecting which visual variant of the model to display.

Confirmed on le3e: 5 farm house instances show `user_data[1]` values of 0 or 2,
corresponding to two distinct house appearance variants.

---

## Mode Tag (`mode`, group `0x6D6F6465`)

The `mode` tag (subgroup = scenery type_tag) wraps a `model_definition`:

```
model_definition header (64 bytes, all offsets relative to byte 64):
  [4:8]   geometry_tag        — subgroup ID for the geom tag
  [12:14] permutation_count   (int16)
  [28:32] permutations_offset (int32, relative to byte 64)

model_permutation_data (64 bytes each):
  [0:2]   collection_reference_permutation  (color table index)
  [2:34]  permutations[32] (uint8 × 32)
              permutations[matIndex] = view index for this material under this permutation
              0xFF = material not rendered for this permutation
  [34:60] unused
  [60:62] collection_index (runtime)
  [62:64] color_table_index (runtime)
```

`MAXIMUM_MATERIALS_PER_GEOMETRY = 32`

---

## Geom Tag (`geom`, group `0x67656F6D`)

See `mesh_object_format.md` for the full `geometry_definition` layout.

Key points for extraction:

- Header byte [4:8]: `collection_reference_tag` — the `.256` texture collection subgroup ID.
- Header byte [16:22]: `center` (3 × int16) — local origin; subtract before scaling vertices.
- `geometry_material.sequence_index` at material record offset [32:34] — which sequence in the
  `.256` collection provides this material's texture frames.
- All offset fields in the 128-byte header are relative to the start of the tag data (byte 0).
  The extractor reads them as `relativeOffset` and adds 128 to get the absolute position within
  the tag buffer. *(Note: `mesh_object_format.md` says offsets are relative to byte 128; both
  descriptions are equivalent since the header itself is 128 bytes.)*

---

## `.256` Collection Tag (group `0x2E323536`)

Myth II `.256` (COLORMAP type) collection tags hold all sprite/texture bitmaps for a model.

### Header Fields Used (all big-endian, from tag start)

```
[68:72]   color_tables_offset   (int32, from bulk_offset)
[96:100]  bitmap_count          (int32)
[100:104] bitmap_refs_offset    (int32, from bulk_offset)
[112:116] bitmap_instance_count (int32)
[116:120] bitmap_instances_offset (int32, from bulk_offset)
[128:132] sequence_count        (int32)
[132:136] sequence_refs_offset  (int32, from bulk_offset)
[248:252] bulk_offset           (int32, from tag start)
```

### Extraction Chain

To extract view `viewIndex` of sequence `seqIndex`:

```
sequence_reference[seqIndex]   128 bytes at bulk+seqRefsOff + seqIndex*128
  [+64:+68] seqDataOff (int32)

sequence_data                  64 bytes at bulk+seqDataOff
  [+8:+10]  number_of_views (int16)  — frame count for this sequence

sequence_frame_data[0]         46 bytes immediately after sequence_data (at seqDataAbs+64)
  followed by bitmap_instance_indexes[number_of_views] (int16 each)
  → bii = bitmap_instance_indexes[viewIndex]

bitmap_instance_data[bii]      64 bytes at bulk+bitmapInstsOff + bii*64
  [+28:+30] bitmap_index (int16) → bi

bitmap_reference[bi]           128 bytes at bulk+bitmapRefsOff + bi*128
  [+64:+68] imgDataOff (int32)   — pixel data offset from bulk_offset
  [+76:+78] width  (int16)
  [+78:+80] height (int16)

pixel data                     at bulk+imgDataOff+52  (52-byte bitmap_data header skipped)
                               width*height bytes, palette-indexed
```

### Palette

Located at `bulk + color_tables_offset`. Skip a 32-byte `color_table` header to reach `color[0]`.

Each palette entry is 8 bytes matching `rgb_color { word red, word green, word blue, word flags }`:

```
c[0] = red   (high byte of word)
c[2] = green
c[4] = blue
c[6] = flags (high byte; non-zero = special, but index 0 is always transparent)
```

**Transparency**: palette index 0 is always the transparent void color. Set alpha=0 and RGB=0
for index-0 pixels to avoid color bleed at texture edges when bilinear filtering is applied.

### Texture Naming Convention

```
<collTag>_<seqIndex>_<viewIndex>.png
```

- `collTag` = 4-char subgroup tag of the `.256` collection
- `seqIndex` = material's `sequence_index` (0=front, 1=back, 2=left, 3=right, 4=roof for farm)
- `viewIndex` = variant view index (driven by permutation table)

All views for all sequences are extracted up front. Per-instance MTL files reference the specific
view chosen by the permutation table for that instance.

---

## UV Transform

Myth II UV coordinates are stored as `fixed_fraction` (uint16, divide by 65536) with the origin
at the top-left of the texture, `u` going right and `v` going down.

OBJ/Blender expects origin at bottom-left. The correct transform to produce properly oriented
textures (confirmed empirically against in-game appearance):

```
OBJ_s = myth_v
OBJ_t = 1.0 - myth_u
```

Winding is also flipped: Myth surfaces use clockwise vertex order; OBJ uses counter-clockwise.
Face emission reverses the corner order: `vi[2], vi[1], vi[0]`.

---

## Vertex Transform (model → world OBJ)

```
// 1. Subtract local origin (center), scale to cell units, mirror X
mx = -((vtx.x - cx) / WORLD_ONE)
my =  (vtx.y - cy) / WORLD_ONE
mz =  (vtx.z - cz) / WORLD_ONE

// 2. Rotate around up axis by instance facing
//    facing_rad = (facing_deg - 90) * PI/180
rx =  cos(f)*mx - sin(f)*my
ry =  sin(f)*mx + cos(f)*my
rz =  mz

// 3. Translate to world position
//    Terrain OBJ convention: vx = halfW - cellX, vz = cellY - halfH
wx = rx + (halfW - cellX)
wy = ry + (cellY - halfH)
wz = rz + cellZ

// 4. Emit as OBJ vertex (Y=up convention)
"v wx wz wy"
```

Where `halfW = submeshW*32 / 2.0`, `halfH = submeshH*32 / 2.0`.

---

## Per-Instance OBJ Export

Each placed instance gets its own `<tag>_<markerIdx>.obj` + `<tag>_<markerIdx>.mtl`.

The MTL selects textures based on the permutation table:

```
perm = user_data[1]
matViews = permCache[typeTag][perm]        // [matIndex] → viewIndex
vi = matViews[matIndex]                    // 0xFF = not rendered → vi=0 fallback
png = "<collTag>_<seqIndex>_<vi>.png"
```

MTL `map_Kd` paths use `textures/<filename>` relative to the `models/` output folder.

---

## Output Layout

```
<out_folder>/
  models/
    <tag>.obj              per-type canonical geometry (view 0 textures)
    <tag>.mtl
    <tag>_<N>.obj          per-instance geometry with permutation-correct textures
    <tag>_<N>.mtl
    map_combined.obj       all instances transformed into world space + terrain
    map_combined.mtl
    displacement.obj       terrain mesh (written by myth2_mesh)
    displacement.mtl       terrain material (map_Kd ../layers/terrain.png)
    textures/
      <collTag>_<seq>_<view>.png   all extracted texture variants
  placement.json           all _marker_model instances with position, facing, permutation
```
