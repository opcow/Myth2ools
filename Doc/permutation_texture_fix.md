# Permutation Model Texture View Fix

## Summary

Permutation models (scenery that shares geometry between multiple "skins" — e.g.
farmhouses that look different but share the same walls/roof mesh) were always
emitting the first texture variant (view index 0) for every instance, ignoring
the permutation data stored in the mode tag. Three independent bugs caused this.

---

## Background

Myth II model tags (`mode` group) can define multiple **permutations** of a
model. Each permutation maps each of the model's materials to a **view index**
— which sprite/frame from the `.256` collection's bitmap sequence should be
displayed. This allows a single geometry + texture collection to produce many
visually distinct instances (e.g. different-colored farmhouses, different
ruin states, etc.).

The data flow for resolving an instance's textures is:

```
scenery instance
  └─ type_tag → mode tag (model definitions)
       └─ model_permutation_data[permutation_index]
            └─ permutations[material_index] = view_index (byte)
                 └─ maps into .256 collection's sequences
                      └─ seq + view → fate_2_5.png
```

---

## Bug 1: Wrong Offset for `permutations_offset`

### The Problem

The code was reading the `model_permutations_offset` field from byte offset 24
in the `model_definition` header:

```cpp
int32_t permRelOff = readBE32s(modeData.data(), 24);  // BUG: should be 28
```

### The Root Cause

The `model_definition` struct has an unusual layout: disk-persistent integer
fields and runtime-only pointer fields are interleaved in a single packed
struct:

```
Offset  Size  Field                          On-disk?
------  ----  -----------------------------  --------
[0:4]    4    flags                          yes
[4:8]    4    geometry_tag                   yes
[8:10]   2    geometry_index                 yes
[10:12]  2    model_geometry_vertex_count    yes
[12:14]  2    model_permutation_count        yes
[14:16]  2    model_cell_count               yes
[16:20]  4    model_geometry_vertex_offset   yes
[20:24]  4    model_geometry_vertex_size     yes
[24:28]  4    vertex_flags  (pointer)        ** NO — zeroed on disk **
[28:32]  4    model_permutations_offset      yes  ← need this one
[32:36]  4    model_permutations_size        yes
[36:40]  4    model_permutations (pointer)   ** NO — zeroed on disk **
[40:44]  4    model_cell_offset              yes
[44:48]  4    model_cell_size                yes
[48:52]  4    model_cells (pointer)          ** NO — zeroed on disk **
[52:56]  4    data_offset                    yes
[56:60]  4    data_size                      yes
[60:64]  4    data (pointer)                 ** NO — zeroed on disk **
```

The code was reading `[24:28]`, which is the `vertex_flags` pointer field.
Since this is a runtime-only field, it is always zeroed on disk. Reading it
produced `permRelOff = 0`, causing `permBase` to resolve to `64 + 0 = 64` —
the start of the vertex flags data — instead of the real permutation table.

### The Fix

Changed the read offset from 24 to 28:

```cpp
int32_t permRelOff = readBE32s(modeData.data(), 28);  // FIXED
```

This now correctly reads the `model_permutations_offset` field. For the
farmhouse mode tag on le3e, the value is **376**, giving
`permBase = 64 + 376 = 440` — the actual permutation data.

### Raw Data Verification

Before the fix, the "permutation" data at offset 64 contained vertex flags
(all zeroed or vertex-related). After the fix, the real permutation data at
offset 440 showed non-zero view indices:

```
Permutation 0:  {9, 4, 13, 6, 0, ...}
Permutation 2:  {16, 5, 7, 22, 0, ...}
```

These values matched independent verification in the Oak tag editor tool.

---

## Bug 2: View Index Clamping to Wrong Frame Count

Even after fixing the offset, the texture output was still wrong. The view
indices from the permutation table (e.g. 16, 22) are larger than the
`number_of_views` field in the sequence data (which reports 2–10 frames).

### The Old Behavior

The original code trusted the stored `number_of_views` field at `seqDataAbs + 8`
and clamped view indices that exceeded it:

```cpp
if (vi >= numViews) vi = 0;
```

Since most view indices (9, 13, 16, 22) exceeded the stored counts (2–10),
this always produced view 0 for every permutation material — identical to the
original bug symptom.

### The Fix

The stored `number_of_views` field is correct. The view index should be
applied modulo this count within the same sequence — no sequence pairing,
spanning, or scanning is involved:

```cpp
int nv = dot256SequenceViewCount(collData, mat.sequenceIndex);
if (nv > 0) vi %= nv;
else vi = 0;
```

### Why Wrapping Happens

Permutation view indices can be larger than a sequence's frame count (e.g.
view 16 on an 8-frame sequence). This is not an error — the modulo wrap
is the intended behavior. The game engine resolves these indices the same
way: `view % frameCount`, staying within the starting sequence.

### Sequence Pairing Was a Red Herring

An earlier attempt used an even-odd pair spanning algorithm (walking through
paired same-dimension sequences, subtracting frame counts). This appeared to
match in-game visuals on the `fate` collection because:
- The paired sequences (front/back, left/right) have identical pixel dimensions
- They differ only by subtle shading to simulate shadowing on opposite sides
- For most permutation views, the walk and simple modulo give the same result

Another attempt tried scanning the bitmap_instance_indexes array beyond the
stored count, but this read past the array boundary into adjacent data,
producing textures of the wrong dimensions.

### Validation

For permutation 2 views `{16, 5, 7, 22, 0}` with the correct stored counts
(8, 8, 10, 10, 2):

| Material | Seq | nv | Raw view | Result       |
|----------|-----|----|----------|--------------|
| left     | 2   | 8  | 16%8=0   | fate_2_0.png |
| right    | 3   | 8  | 5        | fate_3_5.png |
| front    | 0   | 10 | 7        | fate_0_7.png |
| back     | 1   | 10 | 22%10=2  | fate_1_2.png |
| roof     | 4   | 2  | 0        | fate_4_0.png |
|----------|-----|-----------|----------|-----------|
| left     | 2   | 17        | 16       | fate_2_16 |
| right    | 3   | 17        | 5        | fate_3_5  |
| front    | 0   | 19        | 7        | fate_0_7  |
| back     | 1   | 19        | 22%19=3  | fate_1_3  |
| roof     | 4   | 11        | 0        | fate_4_0  |

Only view 22 on seq 1 (back wall) still wraps, and only by 3 positions.

---
## Bug 3: Even-Odd Pair Spanning (Red Herring)

Before the correct modulo fix was identified, an intermediate attempt used an
even-odd pair spanning algorithm. It walked through paired same-dimension
sequences, subtracting frame counts until the view fit:

```cpp
while (true) {
    int nv = dot256SequenceViewCount(collData, seq);
    if (vi < nv) break;
    vi -= nv;
    seq = (seq % 2 == 0) ? seq + 1 : seq - 1;
}
```

This appeared to work on `fate` because:
- The paired sequences (front/back, left/right) share pixel dimensions
- They differ only by subtle shading for opposite-side shadowing
- Most permutation views that triggered a pair-cross gave the same result as
  simple modulo; views that diverged were not visually distinguishable

A subsequent attempt scanned the bitmap_instance_indexes array past the stored
count, which read into adjacent data and produced wrong-dimension textures.
Both approaches were abandoned once the correct modulo-within-sequence fix was
applied.

---

## Implementation Details

### New Helper Functions

- `dot256SequenceViewCount(d, seqIndex)` — reads the stored `number_of_views`
  field from the sequence data header

### Per-Instance Texture Resolution

A `PlacedInstance::materialTexturePngs` vector carries per-permutation texture
overrides. The `exportCombinedOBJ` function uses `inst.materialTexturePngs[mi]`
when available, falling back to `mat.texturePng` for non-permutation models.

### Three-Install Strategy

The `.256` collections exist only in the `large_install.dng2`, while mode tags
are in `small_install.dng2` and geom tags are in `medium_install.dng2`. All
three installs must be loaded simultaneously. `findTagInFile` prefers the
collection tag from the same source file as the mode tag, with fallback to
any file.

---

## Validation

Tested against the `le3e` map (Avicenna) farm instances. The `fate` collection
sequences use their stored frame counts (2–10) and permutation view indices
resolve via modulo within the same sequence.

| Instance  | Permutation | Left      | Right     | Front     | Back      | Roof      |
|-----------|------------|-----------|-----------|-----------|-----------|-----------|
| farm_13   | 2          | fate_2_0  | fate_3_5  | fate_0_7  | fate_1_2  | fate_4_0  |
| farm_15   | 2          | fate_2_0  | fate_3_5  | fate_0_7  | fate_1_2  | fate_4_0  |
| farm_29   | 2          | fate_2_0  | fate_3_5  | fate_0_7  | fate_1_2  | fate_4_0  |
| farm_14   | 0          | fate_2_1  | fate_3_4  | fate_0_3  | fate_1_6  | fate_4_0  |
| farm_26   | 0          | fate_2_1  | fate_3_4  | fate_0_3  | fate_1_6  | fate_4_0  |

Previous attempts using even-odd pair spanning produced different results
(e.g. fate_3_1 and fate_1_3 for perm 0) which happened to look similar due
to subtle shading differences between paired textures.

---

## Files Modified

| File | Change |
|------|--------|
| `myth2/export_map_objects.cpp` | `readBE32s(..., 24)` → `readBE32s(..., 28)` |
| `myth2/export_map_objects.cpp` | Comment `[24:28]` → `[28:32]` |
| `myth2/export_map_objects.cpp` | `dot256SequenceViewCount` unchanged (stored field is correct) |
| `myth2/export_map_objects.cpp` | `extractDot256Texture` unchanged (still validates via stored numViews) |
| `myth2/export_map_objects.cpp` | View index resolution changed from `if (vi >= numViews) vi = 0` to `vi %= nv` |
| `myth2/export_map_objects.cpp` | Added `PlacedInstance::materialTexturePngs` vector |
| `myth2/export_map_objects.cpp` | Updated `exportCombinedOBJ` to use per-instance texture paths |
| `Doc/mesh_object_format.md` | Full model_definition header corrected with all 64 bytes |
| `Doc/model_extraction.md` | `[24:28]` → `[28:32]` for permutations_offset |

---

## Open Questions

1. **Why high view indices?** — The permutation table stores raw view selector
   values, not pre-normalized indexes into the material sequence's current
   bitmap list. These selectors can exceed a sequence's frame count (e.g.
   16 vs 8, 22 vs 10), and both in-game appearance and Oak's tag editor output
   support resolving them with modulo against that material sequence's
   `number_of_views`.

   Oak displays the same raw condition as "view 17 of 8" or "view 23 of 10"
   because Oak's UI is 1-based while the stored mode permutation bytes are
   0-based. In code terms, those examples are stored selectors 16 and 22:

   ```
   16 % 8  -> 0
   22 % 10 -> 2
   ```

   The likely authoring model is that the permutation byte represents a shared
   variation phase or raw collection view selector. Each material side then
   resolves that selector within its own sequence length. This preserves more
   intent than canonicalizing every value down to the current sequence count:
   if a sequence later gained more frames, a raw selector such as 16 would no
   longer be equivalent to 0. The shipped tools/tags appear to preserve the raw
   selector and rely on runtime sequence lookup to wrap it.

2. **Other `.256` collections** — The modulo fix has only been tested on the
   `fate` collection. Other collections with different frame counts or
   permutation models should work identically since the algorithm is universal.

3. **`model_permutation_delta`** — The `scen` tag has a
   `model_permutation_delta` field at `[52:54]` used when
   `_scenery_adjusts_model_permutation_flag` is set. This offsets the
   permutation index for skelmodel-based scenery (3D animated models).
   Not relevant to 2D sprite extraction but may matter for future
   skelmodel permutation support.
