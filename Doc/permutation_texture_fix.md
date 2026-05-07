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

## Bug 2: Wrong Sequence View Count

Even after fixing the offset, the texture output was still wrong. The view
indices from the permutation table (e.g. 16, 22) are much larger than the
`number_of_views` field in the sequence data (which reports 2–10 frames).

### The Root Cause

The `.256` collection stores a `number_of_views` field at `seqDataAbs + 8`.
This field reports a SMALLER count than the actual number of valid
bitmap_instance_index entries in the sequence's frame array. The true frame
count is determined by scanning the bitmap_instance_indexes array for valid
entries (non-negative `bii < bitmapInstCount`).

For the `fate` collection on le3e, the real frame counts are much larger:

| Seq | Stored nv | Actual nv | Extra frames |
|-----|-----------|-----------|--------------|
| 0   | 10        | 19        | 9            |
| 1   | 10        | 19        | 9            |
| 2   | 8         | 17        | 9            |
| 3   | 8         | 17        | 9            |
| 4   | 2         | 11        | 9            |

The stored `number_of_views` field may represent the count of unique/animation-
key frames, while the full bitmap_instance_indexes array includes additional
entries for animation cycle frames or palette-swapped variants. Regardless of
the game's intent, the correct frame count for indexing purposes is the count
of valid entries in the bitmap_instance_indexes array.

### The Old Behavior

The old code trusted the stored field at `+8`, extracting only 2–10 frames per
sequence. The permutation view indices (up to 22) were then clamped modulo this
wrong count, or wrapped via an incorrect even-odd pairing algorithm, producing
the wrong frames.

### The Fix

`dot256SequenceViewCount` now scans the bitmap_instance_indexes array instead
of reading the stored field:

```cpp
static int dot256SequenceViewCount(const std::vector<uint8_t>& d, int seqIndex) {
    // ...resolve seqDataAbs and biiBase...
    int count = 0;
    for (int vi = 0; ; vi++) {
        size_t off = biiBase + (size_t)vi * 2;
        if (off + 2 > d.size()) break;
        int16_t bii = readBE16s(d.data(), off);
        if (bii < 0 || bii >= bitmapInstCount) break;
        count++;
    }
    return count > 0 ? count : 1;
}
```

The view index is then applied modulo this correct count:

```cpp
int nv = dot256SequenceViewCount(collData, mat.sequenceIndex);
if (nv > 0) vi %= nv;
```

### Result for Permutation 2

With the real frame counts (17/19/11 frames per sequence), most permutation
view indices fall directly within range — no wrapping needed:

| Material | Seq | Actual nv | Raw view | Result    |
|----------|-----|-----------|----------|-----------|
| left     | 2   | 17        | 16       | fate_2_16 |
| right    | 3   | 17        | 5        | fate_3_5  |
| front    | 0   | 19        | 7        | fate_0_7  |
| back     | 1   | 19        | 22%19=3  | fate_1_3  |
| roof     | 4   | 11        | 0        | fate_4_0  |

Only view 22 on seq 1 (back wall) still wraps, and only by 3 positions.

---

## Bug 3: View Index Clamping (Previous Code)

### The Problem

Before the scan-based fix was discovered, an intermediate version clamped the
view index to the stored (wrong) frame count:

```cpp
int numViews = dot256SequenceViewCount(collData, mat.sequenceIndex);
if (vi >= numViews) vi = 0;
```

Since most view indices (9, 13, 16, 22) exceeded the stored counts (2–10),
this always produced view 0 for every material — identical to the original
bug symptom.

### Sequence Pairing Red Herring

An earlier attempted fix used an even-odd pair spanning algorithm (walking
through paired same-dimension sequences, subtracting frame counts). This
happened to match in-game visuals on the `fate` collection because:
- The paired sequences (front/back, left/right) have identical pixel dimensions
- They differ only by subtle shading to simulate shadowing on opposite sides
- The textures were too similar to notice small mismatches

This approach was abandoned when it was realized that the true frame count
is larger than the stored field, making the spanning unnecessary.

---

## Implementation Details

### New Helper Functions

- `dot256SequenceViewCount(d, seqIndex)` — scans the bitmap_instance_indexes
  array to find the true number of frames in a sequence

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
sequences now report their true frame counts (17–19) via bii array scanning,
and all permutation view indices resolve correctly.

---

## Files Modified

| File | Change |
|------|--------|
| `myth2/myth2_model.cpp:1171` | `readBE32s(..., 24)` → `readBE32s(..., 28)` |
| `myth2/myth2_model.cpp:1164` | Comment `[24:28]` → `[28:32]` |
| `myth2/myth2_model.cpp` | `dot256SequenceViewCount` now scans bii array for true frame count |
| `myth2/myth2_model.cpp` | `extractDot256Texture` no longer rejects views via stored numViews |
| `myth2/myth2_model.cpp` | View index resolved as `vi % nv` with real frame count |
| `myth2/myth2_model.cpp` | Added `PlacedInstance::materialTexturePngs` vector |
| `myth2/myth2_model.cpp` | Updated `exportCombinedOBJ` to use per-instance texture paths |
| `Doc/mesh_object_format.md` | Full model_definition header corrected with all 64 bytes |
| `Doc/model_extraction.md` | `[24:28]` → `[28:32]` for permutations_offset |

---

## Open Questions

1. **Meaning of the `number_of_views` field** — The stored field at
   `seqDataAbs + 8` consistently reports a smaller count (~9 fewer) than the
   actual valid entries in the bitmap_instance_indexes array. It may represent
   "unique key frames" vs. "total animation frames," or some other game-engine
   concept. Its exact semantics are unknown.

2. **Other `.256` collections** — The scan-based approach has only been tested
   on the `fate` collection. Other collections may have different patterns or
   invalid data at higher view indices. The scan stops at the first invalid
   bii entry, which should be safe.

3. **`model_permutation_delta`** — The `scen` tag has a
   `model_permutation_delta` field at `[52:54]` used when
   `_scenery_adjusts_model_permutation_flag` is set. This offsets the
   permutation index for skelmodel-based scenery (3D animated models).
   Not relevant to 2D sprite extraction but may matter for future
   skelmodel permutation support.
