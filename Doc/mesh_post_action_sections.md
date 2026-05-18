# Mesh Tag Sections After Actions

Investigation of the mesh tag regions that follow the action buffer — required to build a `mesh_tag.bin` from scratch instead of patching an extracted one.

## TL;DR — Empirical results contradict the Vengeance/decomp claim

The Vengeance source and Myth II runtime decomp **suggest** that media_coverage, mesh_LOD, and the 0x120 connector section are engine-regenerated when their size fields are 0. **In practice, loading such a mesh in Myth II 1.8.5 fails:**

- Dropping **mesh_LOD** alone: map loads but **terrain renders black/transparent and unclickable**. The engine does NOT regenerate LOD.
- Dropping **media_coverage + mesh_LOD + 0x120 connectors** together: **crash** in `drop_model_for_current_frame` (model_animations.c:575) during model_animation marker init.
- Regenerating 0x120 connectors from semantic data (`assets/terrain/fences.json`) but still dropping MC and LOD: **same crash**. The crash is caused by dropping MC and/or LOD, not the connector format.
- Trailing editor data (past `data_size` at 0x1C) is safely droppable — confirmed both by decomp and empirical test.

**Current verified-working blob-free strategy:**

| Region | Header descriptor | Status in 1.8.5 | Strategy |
| --- | --- | --- | --- |
| media_coverage_region | 0xC0 / 0xC4 | **Required** (despite decomp claim) | Preserve from source for now; decode later. ~12 KB for le3e. |
| mesh_LOD_data | 0xCC / 0xD0 | **Required** (despite Vengeance claim) | Preserve from source for now; decode later. 18432 B = 9216 × 2 B = likely 96×96 downsampled height grid. |
| section_0x120 (fence connectors) | 0x11C count, 0x120 offset, 0x124 size | **Required** when map has fences | Regenerate from `assets/terrain/fences.json`. Format: 64-byte records, up to 24 × uint16 BE post identifiers, byte 63 = post count, bytes 48-62 unknown (appear uninitialized in source). |
| trailing editor data | (file past `data_size`) | **Safely omitted** | Drop entirely. |
| trailing_a | 0x114 / 0x118 | Always 0 in tested maps | Drop. |

## Evidence

### Vengeance source — fresh-mesh creation

`reference_source/vengeance_july27_2004/ToolCode/mesh.cpp` lines 421-441 show how the Bungie editor builds a brand-new mesh:

```c
h.media_coverage_region_offset = h.map_actions_offset + h.map_action_buffer_size;
h.media_coverage_region_size = 0;

h.mesh_LOD_data_offset = h.media_coverage_region_offset + h.media_coverage_region_size;
h.mesh_LOD_data_size = 0;

// ...

h.connector_count = 0;
h.connectors_offset = h.mesh_LOD_data_offset + h.mesh_LOD_data_size;
h.connectors_size = 0;

// ...

h.data_offset = sizeof(mesh_definition);  // = 1024
h.data_size = h.connectors_offset + h.connectors_size;  // size up to end of connectors

byte *b = (byte *)malloc( sizeof(h) + h.data_size );
// ... write header, write all-default cells, done.
```

A freshly-created mesh in Bungie's own editor has zero bytes for media_coverage, LOD, and connectors. The bytes we see in our extracted maps are **cached runtime values** the editor wrote back after they were computed once.

### Myth II runtime — fills defaults when size=0

Ghidra decomp of `Myth II.exe` `FUN_004b2580` (the function that processes media_coverage_region):

```c
if (param_2 == 0) {                                    // size == 0
    dwBytes = iVar8 << 4;                              // alloc cellH * 16 bytes
    param_1 = HeapAlloc(...);
    // fill the buffer with CONCAT22(uVar1, uVar1)
    // where uVar1 = (cellW * 8)
}
```

When the on-disk size is zero, the engine **allocates and fills the region itself** with `cellW * 8` BE16 values. We confirmed this: extracted maps' media_coverage_region bytes start with `06 00 06 00 06 00 ...` — that's `0x0600 = 1536 = 192 * 8` for a 192-cell-wide map. **The on-disk data IS the runtime default**.

### `data_size` (0x1C) bounds what the runtime loads

The `mesh_definition.data_size` field (at header offset 0x1C) tells the runtime how many bytes to load after the 1024-byte header. Anything past `1024 + data_size` in the file is editor-only metadata that the runtime ignores.

This explains the wildly varying tail sizes in our corpus:

| Map | data_size (0x1C) | File size | Trailing editor data |
| --- | --- | --- | --- |
| 85gy | 408084 | 409444 | 304 B |
| 08li | 407708 | 409444 | 712 B |
| thkm | 739484 | 743692 | 3184 B |
| le3e | 589892 | 619048 | 28132 B |
| 03hp | 567820 | 619048 | 50204 B |

Same map, different "tails" because the editor freely appends notes/undo state after `data_size`. **A from-scratch build skips this entirely**.

## Mesh header field map (Myth II 1.x, big-endian)

Confirmed against Vengeance's `mesh_definition_byte_swapping_data[]` table and verified empirically:

| Offset | Size | Field | Value (le3e) |
| --- | --- | --- | --- |
| 0x00 | 4 | landscape_collection_tag (.256 tag id) | terrain palette ref |
| 0x04 | 4 | media_collection_tag | water/lava .256 ref |
| 0x08 | 2 | submesh_width | 6 |
| 0x0A | 2 | submesh_height | 6 |
| 0x0C | 4 | mesh_offset | 0 (cells start right after header) |
| 0x10 | 4 | mesh_size | `cellW * cellH * 12` |
| 0x14 | 4 | mesh ptr (runtime) | 0 on disk |
| 0x18 | 4 | data_offset | 1024 (= sizeof header) |
| **0x1C** | 4 | **data_size** — runtime stops loading here | 589892 |
| 0x20 | 4 | data ptr (runtime) | 0 on disk |
| 0x24 | 4 | marker_palette_entries (unit type count) | 65 |
| 0x28 | 4 | marker_palette_offset | 442368 |
| 0x2C | 4 | marker_palette_size | (entries × 32) |
| 0x30 | 4 | marker_palette ptr (runtime) | 0 |
| 0x34 | 4 | marker_count (instance count) | 1283 |
| 0x38 | 4 | markers_offset | 443680 |
| 0x3C | 4 | markers_size | (count × 64) |
| 0x40 | 4 | markers ptr (runtime) | 0 |
| 0x44 | 4 | mesh_lighting_tag | -1 (none) |
| 0x48 | 4 | connector_tag | -1 |
| 0x4C | 4 | flags | mesh-level flags |
| 0x50 | 4 | particle_system_tag | -1 |
| 0x54 | 4 | team_count | usually 2 |
| 0x58 | 2 | dark_fraction | |
| 0x5A | 2 | light_fraction | |
| 0x5C-0x63 | 8 | dark_color (rgb_color) | |
| 0x64-0x6B | 8 | light_color (rgb_color) | |
| 0x6C | 4 | transition_point (fixed) | |
| 0x70 | 2 | ceiling_height | |
| 0x72 | 2 | unused | |
| 0x74-0x7B | 8 | edge_of_mesh_buffer_zones (rectangle2d) | |
| 0x7C | 4 | action_section_signature | usually `amds`; stock docs also mention `amdu` |
| **0x80** | 4 | **map_action_count** | 239 |
| **0x84** | 4 | **map_actions_offset** | 525792 |
| **0x88** | 4 | **map_action_buffer_size** | 33132 |
| 0x8C-0x9B | 16 | description / postgame / pregame / overhead .256 tags | -1 / tag |
| 0x9C-0xA3 | 8 | next_mesh_tags[2] | |
| 0xA4-0xAF | 12 | cutscene_movie_tags[3] | |
| 0xB0-0xBF | 16 | storyline_string_tags[4] | |
| **0xC0** | 4 | **media_coverage_region_offset** | 558924 |
| **0xC4** | 4 | **media_coverage_region_size** | **set to 0 for fresh build** |
| 0xC8 | 4 | media_coverage_region ptr (runtime) | 0 |
| **0xCC** | 4 | **mesh_LOD_data_offset** | 570948 |
| **0xD0** | 4 | **mesh_LOD_data_size** | **set to 0 for fresh build** |
| 0xD4 | 4 | mesh_LOD_data ptr (runtime) | 0 |
| 0xD8-0xDF | 8 | global_tint_color + global_tint_fraction + pad | |
| 0xE0 | 4 | wind_tag | |
| 0xE4-0xEF | 12 | screen_collection_tags[3] | |
| 0xF0-0xF7 | 8 | blood_color | |
| 0xF8-0x107 | 16 | picture_caption / narration / win_ambient / loss_ambient sound tags | |
| 0x108-0x117 | 16 | reverb_environment + reverb_volume + reverb_decay + reverb_damping (Creative EAX) | |
| **0x118** | 4 | **connector_count** | **0 for fresh build** |
| **0x11C** | 4 | **connectors_offset** | |
| **0x120** | 4 | **connectors_size** (or in Myth II: section_0x120 offset) | **0 for fresh build** |
| 0x124 | 4 | connectors ptr (runtime) / section_0x120 size | |

### Note on the "section_0x120" anomaly

Three of nine sample maps (`le3e`, `le3e-*`, `03hp`, `sprite-row-test`) have a non-zero descriptor at offset 0x120 with 512 bytes of monster-ID-looking data; six others have 0. The Vengeance struct has `connectors_size` at this position and Myth II places `cutscene_names[3][64] = 192 bytes` immediately after — so the layout is slightly shifted. The 512-byte payloads we observed are likely either:

1. Cached connector data (despite count=0 being inconsistent), or
2. A 1.5+ field added after Vengeance was forked.

Empirical test: build a plugin with all of media_coverage_size, mesh_LOD_size, connector_count, AND the 0x120 field set to 0 — then load it in Myth II 1.8.x. If it loads, all four are confirmed regeneratable; if it crashes, drill into what's special about that 512 bytes.

## Build-from-scratch recipe

Pseudocode for assembling a brand-new mesh tag (no `mesh_tag.bin` input required):

```c
// 1. Allocate and zero the 1024-byte header.
mesh_header h = {0};

// 2. Compute sizes for the data sections we DO need.
size_t mesh_size           = cellW * cellH * 12;          // cell array
size_t marker_palette_size = unit_type_count * 32;        // unit type defs
size_t markers_size        = marker_count * 64;           // placed unit instances
size_t actions_size        = serialize_actions(...);      // see action_scripts.md

// 3. Lay out section offsets (each starts where the previous one ends).
h.mesh_offset           = 0;
h.mesh_size             = mesh_size;
h.marker_palette_offset = mesh_size;
h.marker_palette_entries = unit_type_count;
h.marker_palette_size   = marker_palette_size;
h.markers_offset        = h.marker_palette_offset + marker_palette_size;
h.marker_count          = marker_count;
h.markers_size          = markers_size;
h.map_actions_offset    = h.markers_offset + markers_size;
h.map_action_count      = action_count;
h.map_action_buffer_size = actions_size;

// 4. Mark all the regenerable sections as zero-sized.
size_t end_of_actions = h.map_actions_offset + actions_size;
h.media_coverage_region_offset = end_of_actions;
h.media_coverage_region_size   = 0;       // engine fills from defaults
h.mesh_LOD_data_offset         = end_of_actions;
h.mesh_LOD_data_size           = 0;       // engine derives from cell heights
h.connector_count              = 0;
h.connectors_offset            = end_of_actions;
h.connectors_size              = 0;
// (set the 0x120 field to 0 too)

// 5. data_size bounds what the runtime loads.
h.data_offset = 1024;
h.data_size   = end_of_actions;

// 6. Set fixed defaults from the Vengeance "new mesh" code.
h.team_count                    = 2;
h.ceiling_height                = FLOAT_TO_WORLD(32.0);   // 32 world units
h.edge_of_mesh_buffer_zones     = {32, 32, 32, 32};
h.flags                         = 0;
// ... initialize all the file_tag fields to -1 (no reference)
h.new_field_block_valid_code    = 0xFEDC;

// 7. Write the file: 1024-byte header (all BE-swapped), then concatenate
//    cells, marker_palette, markers, actions.
write(&h, 1024);
write(cells, mesh_size);
write(marker_palette, marker_palette_size);
write(markers, markers_size);
write(action_buffer, actions_size);
// NO trailing editor data.
```

## Bugs found in `build_plugin.cpp`

While verifying the layout, two bugs surfaced in the existing patch-based builder that should be fixed regardless of whether we go full-rebuild:

1. **`bumpSectionOffset(0x3E4)` is a no-op on a constant** — the value at 0x3E4 is the magic `0x00010008` / `0x00010009`. It's not an offset, just a version field. The bump never triggers (constant < action offset) but the call is misleading. **Remove it.**
2. **0x120 is not bumped** when actions resize. If section_0x120 contains real data (as it does in `le3e`, `03hp`), changing the action buffer size by enough bytes to push the threshold would corrupt the offset. **Add `bumpSectionOffset(0x120)` to both call sites.** (Or, ideally, switch to full rebuild and stop patching.)

## Next steps

1. **Decode `mesh_LOD_data` format.** Hypothesis: 9216 × 2-byte values = 96×96 downsampled height grid (every other cell in a 192-cell-wide map). If confirmed, generate from `cell_grid.json` heights.
2. **Decode `media_coverage_region` format.** Source on-disk MC starts with `06 00 06 00 06 00 ...` (uint16 BE = `cellW * 8`) which matches the runtime allocation/fill formula from the Ghidra decomp. Need to check whether the runtime regen path is reachable in gameplay vs editor only.
3. **Decode the unknown 14 bytes of each connector record** (offsets 48-62). Vary wildly across records; possibly per-fence rendering hints, possibly uninitialized editor heap. Test with extreme values to see if engine reads them.
4. **Investigate `drop_model_for_current_frame` crash.** Use Ghidra to read the offset 0x7E instruction in model_animations.c:575 to see what specific field is being dereferenced. That will pin down whether MC or LOD (or something else) is the missing input.
