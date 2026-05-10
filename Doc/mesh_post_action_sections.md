# Mesh Tag Sections After Actions

Investigation of the mesh tag regions that follow the action buffer — required to eliminate the `mesh_tag.bin` dependency in the plugin builder.

## Summary

The mesh tag has **five distinct regions after the action buffer**, plus one "header field" at 0x3E4 that turns out to be a magic constant rather than an offset. None are currently extracted to editable form; the builder just patches the action buffer in place and bumps three of the section offsets.

| Region | Header descriptor | Size in `le3e` | Size scales with | Required? |
|---|---|---|---|---|
| media_coverage_region | 0xC0 (offset), 0xC4 (size) | 12024 B | water/lava extent | **yes** (engine reads it) |
| mesh_LOD_data | 0xCC (offset), 0xD0 (size) | 18432 B | map dimensions | **yes** (downsampled heightmap) |
| connectors | 0x114 (offset), 0x118 (size) | 0 B | fence/door count | **no** (empty in all 9 sample maps) |
| section_0x120 | 0x120 (offset), 0x124 (size) | 512 B | unknown | **unknown** (sometimes 0, sometimes 512) |
| editor tail | 0x1C points to its start | 28132 B (le3e) → 304 B (85gy) | map complexity | **no** (editor only) |

## Header descriptor format

Each section descriptor is 12 bytes:

| Offset | Field |
|---|---|
| +0 | raw offset (mesh-relative; file offset = 1024 + raw) |
| +4 | size in bytes |
| +8 | resolved pointer (runtime only — 0 on disk) |

The descriptors live at fixed offsets in the 1024-byte mesh header. The 12-byte stride is consistent: media at 0xC0, LOD at 0xCC (+12), then a gap, then connectors at 0x114, then section_0x120 at 0x120 (+12).

Confirmed via Ghidra decomp from `Myth II.exe`:

```
*(int *)(iVar1 + 0xC8) = *(int *)(iVar1 + 0xC0) + base;  // media: raw → resolved
*(int *)(iVar1 + 0xD4) = *(int *)(iVar1 + 0xCC) + base;  // LOD
FUN_004b2580(*(int *)(iVar1 + 0xC8), *(int *)(iVar1 + 0xC4));  // (data, size)
FUN_00405cbd("connectors", 0x40, &DAT_0052ecac);  // 64-byte connector records
```

## Region breakdown

### media_coverage_region (0xC0)

- **Always present** when the engine loads the mesh.
- Size varies per map: 2560 B (08li, mostly dry), 11388 B (85gy), 12024 B (le3e, 03hp).
- Bytes look like 16-bit BE values; first 128 bytes of `le3e` are uniformly `0600` (= 1536 BE16).
- Likely a spatial index (BSP/quadtree) of media (water/lava) regions for fast cell→region lookup.
- **Engine reads it** via `FUN_004b2580(resolved_ptr, byte_size)`.
- **Regeneration**: would require porting the engine's coverage-region builder. Format not yet decoded.

### mesh_LOD_data (0xCC)

- **Always present**.
- Size = `(submeshW * 32 / 2) * (submeshH * 32 / 2) * 2 bytes`. For 192×192 maps that's 96 × 96 × 2 = 18432 B (matches le3e/03hp).
- One 16-bit BE entry per 2×2 cell block — half-resolution of the cell grid.
- Values like `00FF`, `00FB`, `00F8` — look like compressed height/normal data for distant rendering.
- **Regeneration**: deterministic from the cell heights (downsample 2×2 → 1). Format needs a few more samples to confirm encoding, but this is the most regeneratable section.

### connectors (0x114)

- **Empty in all 9 sample maps** (offset=0, size=0).
- Engine registers a 64-byte record format for them ("connectors", 0x40, ...) — fences/gates/movable terrain features.
- **Safe to omit** in synthesized plugins: write `0x00000000` for offset+size at 0x114/0x118.
- If a map ever uses them, they'd be runs of 64-byte records appended in the same area.

### section_0x120

- **Not bumped by `build_plugin.cpp`** — this is a latent bug; if action buffer size changed, the offset wouldn't update. (Hasn't bitten us because the action edit path doesn't change buffer size enough to matter, but it should be added.)
- 512 B in `le3e`/`03hp`, 0 B in `08li`/`85gy`/`thkm`.
- Bytes look like 16-bit BE monster IDs (`2F5D`, `2F5E`, `2F60`...) interleaved with small numeric metadata. May be **monster groups / squads** for the AI scheduler.
- **Treat as opaque** — extract and reimport verbatim until the format is decoded.

### editor tail (pointed to by 0x1C)

- Size varies wildly: **304 B (85gy), 712 B (08li), 3184 B (thkm), 28132 B (le3e), 50204 B (03hp)**.
- The 0x1C field stores `start_of_tail` as a mesh-relative offset; the tail extends to EOF.
- No section descriptor points into this region — only 0x1C does. Bytes look like leftover editor state (designer notes, undo metadata, label positions).
- **Hypothesis: runtime engine ignores this**. Worth empirical testing — strip it and see if the map still loads.
- `build_plugin.cpp` already bumps 0x1C correctly when the action buffer resizes, so the tail moves with the rest.

### 0x3E4 — NOT an offset

- `build_plugin.cpp` calls `bumpSectionOffset(0x3E4)` and labels it "editor_data_offset", but the value at 0x3E4 is **a magic constant**: `0x00010008` (le3e, 03hp, 08li) or `0x00010009` (thkm). Looks like a version number.
- The bump is a no-op in practice (`0x10008 = 65544` is never larger than the action offset, which is always > 100K), but the call is misleading and should be removed or relabeled as a version field.

## Recommended approach

A **two-phase plan** that lets us strip the bin-file dependency without first decoding every format:

### Phase 1 — Extract & reimport as named blobs (do now)

Add to `extract_map`:

```
<map_folder>/raw/mesh_extras/
  media_coverage.bin       # opaque
  mesh_lod.bin             # opaque (regeneratable later)
  connectors.bin           # usually 0 bytes
  section_120.bin          # opaque
  editor_tail.bin          # opaque, may be omittable
  layout.json              # original sizes + positions for round-tripping
```

In the builder, when assembling a fresh mesh:

1. Lay out cell data, unit types, instances, actions in order.
2. Append each `mesh_extras/*.bin` blob.
3. Recompute and write the four section offsets at 0xC0/0xCC/0x114/0x120 + their sizes.
4. Write `0x1C` = offset to start of `editor_tail.bin` (or to EOF if the tail is omitted).
5. Drop the `bumpSectionOffset(0x3E4)` call (it's a no-op on a constant).

This **proves round-trip correctness** before any format is decoded.

### Phase 2 — Regenerate or omit (incremental)

Once round-trip works, attack regenerable sections one at a time:

| Section | Plan |
|---|---|
| connectors | Always emit empty (0/0). |
| editor_tail | Build a plugin without it, test in the engine. If it loads, omit. |
| mesh_LOD_data | Regenerate from cell heights — half-resolution downsample. Test by diffing against original. |
| section_0x120 | Decode the monster-ID layout; likely produced from instance/squad data already in the mesh. |
| media_coverage_region | Hardest. Requires reverse-engineering the coverage builder (likely a flood-fill over wet cells). Until then, regenerate by **copying from a base mesh** when authoring brand-new maps without water, or keep the extracted blob for water-bearing maps. |

### Bug to fix in `build_plugin.cpp`

The `applyActions` and `applyMarker*` paths bump offsets at `0xC0`, `0xCC`, `0x114`, `0x3E4` — but **not `0x120`**. Either the section_0x120 offset doesn't shift in our test corpus (we got lucky), or it's silently corrupted. Add `bumpSectionOffset(0x120)` to both call sites and remove the `0x3E4` bump.

## Field reference (as observed)

| Mesh offset | Meaning | Notes |
|---|---|---|
| 0x08, 0x0A | submesh W, H (BE16) | cell grid = W*32 × H*32 |
| 0x1C | end-of-structured-data offset (BE32, mesh-relative) | tail begins here |
| 0x20 | runtime base ptr | always 0 on disk |
| 0x24, 0x28 | unit type count, offset | |
| 0x34, 0x38, 0x3C | instance count, offset, total bytes | |
| 0x80, 0x84, 0x88 | action count, offset, size | |
| 0xC0, 0xC4 | media_coverage_region offset, size | |
| 0xCC, 0xD0 | mesh_LOD_data offset, size | |
| 0x114, 0x118 | connectors offset, size | |
| 0x120, 0x124 | section_0x120 offset, size | unknown contents |
| 0x3E4 | magic version constant (`0x00010008` / `0x00010009`) | NOT an offset |
