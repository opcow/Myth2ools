# Loathing Editor Workflow — what authors what, and how it lands in the mesh

A reference for understanding which fields in `mesh_cell` and adjacent structures come from which Loathing tool, and which are external bitmap imports vs. in-app brush operations. Cross-referenced against the Project Magma Loathing manual (`projectmagma.net/downloads/myth2docs/loathing.html`) and the Vengeance reference source.

## The Fear / Loathing split

The two editors carve up Myth II's data along clean lines:

| Editor | Owns | Examples |
| --- | --- | --- |
| **Fear** | Tag-level data — individual asset definitions and their attributes | `mons` (monster tags), `unit`, `proj`, `soun`, `medi`, `part`, `ligh`, the `mesh` *definition* (header fields like `media_type`, ambient sound, win/loss screens) |
| **Loathing** | Mesh-level data — the per-cell terrain grid and item placement | `mesh_cell` array, item placement, markers, map actions/scripting, lighting placement |

Practical implication: the global "this map's water is lava" decision lives in the mesh definition (set in Fear). The "this triangle is wet to giant depth" decision lives in `mesh_cell.flags` (painted in Loathing). They reference each other but you edit them in different programs.

## What gets painted in-app vs. imported as bitmaps

This is the distinction that matters for round-tripping. Loathing supports two authoring modalities:

### In-app brush operations

The designer paints directly onto the 3D mesh view using a tool palette. These tools mutate `mesh_cell` fields:

| Loathing tool | What it sets | Cell field(s) |
| --- | --- | --- |
| Passability brush | Terrain type per triangle (4-bit nibble × 2) | `flags` bits 0–7 |
| Water Mask Adjustment | Per-cell media flags | `flags` bits 8 (vertex), 11/12 (triangle 0/1) |
| Water height tool | Media surface elevation | `media_height`, plus bit 8 on corner verts |
| Elevation tool | Terrain floor | `physical_height` |
| Scenery placement | (indirectly) scenery-impassable footprints | `flags` bit 15 (editor-only, baked at save) |
| Marker placement | Item entries | separate marker arrays, not `mesh_cell` |

The passability tool, per the Loathing manual: "a red grid appears on the map, and a menu window opens listing the various terrain types," with three modes — Default, Option Key (single cell), Shift Key (fill-like).

### External bitmap imports

For data that's better authored in Photoshop than with brush strokes, Loathing supports importing bitmaps that map 1:1 to the mesh grid:

| Bitmap import | What it controls | Cell field(s) | Loathing menu |
| --- | --- | --- | --- |
| Reflection map | Per-cell reflection rendering | `flags` bit 13 | `Maps > Reflection > Import` |
| Water displacement / height map | `media_height` topography across the level | `media_height` | (per the manual: "matching the normal displacement map's scale") |
| Terrain displacement | `physical_height` topography | `physical_height` | Standard displacement workflow |

A "Set All Media Heights" command in the mesh menu also exists for bulk flattening.

This is why round-tripping the project's `terrain/reflection.bmp` and `terrain/animation.bmp` is plausible: the bitmap-import workflow has Loathing precedent. The Loathing manual doesn't describe a direct bitmap input for the **animated** media flag (bit 10) — that's the gap the project's experimental `--animation` option is filling.

## How this maps to the project's tool outputs

Each `extract_map` output corresponds to one Loathing authoring channel:

| Project output | Loathing analogue | Cell representation |
| --- | --- | --- |
| `terrain/passability.bmp` | Passability brush state | `flags` bits 0–7 (terrain types) |
| `terrain/water.bmp` | Water Mask Adjustment + tier | `flags` bits 11/12 + media tier in nibbles |
| `terrain/reflection.bmp` | Imported reflection map | `flags` bit 13 |
| `terrain/animation.bmp` | (no direct Loathing import; bit set implicitly) | `flags` bit 10 |
| `terrain/elevation.bmp` (heightmap) | Imported displacement map | `physical_height` |
| `terrain/water_generated.bmp` | (no Loathing analogue — synthesizes tier from depth) | tier nibbles for `flags` bits 0–7 |

The OBJ exports (`mesh`, `water_mesh`) are a third channel that doesn't have a Loathing equivalent — they let the designer sculpt geometry in Blender that no in-editor brush could produce, especially for sloped media surfaces.

## Designer-facing depth tier semantics

The Loathing manual restates the four media tiers in pure gameplay language:

| Internal name | Loathing description |
| --- | --- |
| `_terrain_media_dwarf_depth` | "Most units can pass through this, with the notable exceptions of shades" |
| `_terrain_media_human_depth` | "Hardy, human sized units and larger can handle this" |
| `_terrain_media_giant_depth` | "Only Trow, large Myrkridia, and dead units may pass through" |
| `_terrain_media_deep` | "Only dead or amphibious units may pass through" |

That confirms: the tiers are gameplay categories about *which units survive*, not depth measurements. Geometry (`media_height`) and gameplay (terrain type) really are independent and both authored.

## Other terrain types worth noting

Not all terrain types are media. The non-media types map to the surface-substance and movement-cost system:

- `_terrain_grass`, `_terrain_desert`, `_terrain_rocky`, `_terrain_marsh`, `_terrain_snow`, `_terrain_forest` — surface substance, controls texture/footstep sounds and visibility (forest blocks long-range sighting, per `mesh.h` comment)
- `_terrain_sloped`, `_terrain_steep` — locomotion penalty / impassable to most, but ghols and spiders can climb
- `_terrain_walking_impassable`, `_terrain_flying_impassable` — hard barriers, used for map edges and scenery footprints
- `_terrain_loathing_special` — the magenta debug terrain type the editor uses for cells in some special editor state; not present in shipped maps

## Cross-references to the bit notes

For per-bit detail see:

- [`bit_10.md`](bit_10.md) — `_mesh_vertex_is_animated_media_bit`, the per-vertex wave displacement gate (confirmed by Ghidra disassembly; recomputed by the engine at mesh load, so disk edits without topology changes have no effect)
- [`bit_15.md`](bit_15.md) — `_mesh_cell_will_be_marked_impassable_bit`, the scenery-footprint staging flag
- [`water_depth.md`](water_depth.md) — full treatment of `media_height` ↔ depth tier interaction
