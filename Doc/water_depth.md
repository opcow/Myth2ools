# Myth II Water Depth

There are two parallel representations of "how deep is this water," and that duality is the key to the whole system. They interact in non-obvious ways.

## Track 1 — Continuous geometry: `media_height`

Each `mesh_cell` carries a `media_height` field (signed 16-bit world distance, last short in the struct, see [mesh.h:198](reference_source/vengeance_july27_2004/ToolCode/mesh.h#L198)). This is the **actual surface elevation** of the media in absolute world units — same coordinate system as `physical_height` (the terrain floor).

The "depth" at a cell isn't stored anywhere directly. It's computed:

```text
depth = media_height - physical_height
```

`media_height` is per-vertex (NW corner of the cell), so the water surface across a cell is a bilinear patch of four neighboring `media_height` values. Inside Loathing's water tool, raised verts come from [TMeshView.cpp:2536-2542](reference_source/vengeance_july27_2004/ToolCode/TMeshView.cpp#L2536-L2542):

```c
cell->flags |= _mesh_cell_triangle0_is_media_flag
            |  _mesh_cell_triangle1_is_media_flag
            |  _mesh_vertex_is_media_flag;
cell->media_height = targetMediaHeight;
```

So when the designer drops a water region, all four shared corners get assigned the same `targetMediaHeight` and the cell's two triangles get tagged wet. That's why bodies of water tend to be flat, flat, flat — Loathing's `flood_fill_water` only fills cells whose four corner `media_height` values *already* equal the target ([TMeshView.cpp:1932-1935](reference_source/vengeance_july27_2004/ToolCode/TMeshView.cpp#L1932-L1935)). Sloped media (a flowing stream surface) would have to be hand-raised vert by vert.

The 1/512 conversion you see in the editor visualization (`cell->media_height * (1.0f / 512.0f)` at [TMeshView.cpp:1672](reference_source/vengeance_july27_2004/ToolCode/TMeshView.cpp#L1672)) is the engine's `WORLD_FRACTIONAL_BITS=9` — fixed-point with 9 bits of fraction, so one world unit = 512 storage units. Range of `media_height` is therefore roughly ±64 world units (±32k storage / 512).

## Track 2 — Discrete gameplay: terrain type per triangle

Bits 0–7 of `flags` hold two 4-bit terrain types, one per triangle. Of the 16 terrain types ([mesh.h:120-152](reference_source/vengeance_july27_2004/ToolCode/mesh.h#L120-L152)), the **first four** are the media-depth gameplay tiers:

```text
0  _terrain_media_dwarf_depth   "dwarf-sized waist height"
1  _terrain_media_human_depth   "man-sized waist height"
2  _terrain_media_giant_depth   "giant-sized waist height"
3  _terrain_media_deep          "deeper than giant height"
```

This is what gameplay actually reads. It controls things like: which units can wade vs. drown, whether a unit takes movement penalty, how much of the sprite is occluded below the waterline, whether a dwarf bottle floats or sinks, fire/ignite checks against lava. The runtime never asks "how many world units deep is this water?" — it asks "what depth tier is this triangle?"

Note the macro [`TERRAIN_TYPE_IS_MEDIA(t)`](reference_source/vengeance_july27_2004/ToolCode/mesh.h#L156): `(t)<FIRST_MEDIA_TERRAIN_TYPE+NUMBER_OF_MEDIA_TERRAIN_TYPES`, i.e. type 0–3 are media, 4+ are dry land. So media-ness *is* depth tier — a triangle can't be "wet but undefined depth."

## How the two tracks relate

This is the bit that takes a minute to internalize:

- **Geometry** (`media_height`, per vertex) defines where the water surface visually sits and what gets reflected/animated/wave-displaced.
- **Gameplay** (terrain type, per triangle) defines what the water *does* to units that walk into it.

These are mostly redundant — a designer who paints a deep pool will set `media_height` high *and* tag the triangles `_terrain_media_giant_depth` or `_terrain_media_deep`, because that matches what the player will see. But the engine doesn't enforce the consistency. You can absolutely have a triangle whose geometry says "0.5 world units of water" but whose terrain type says `_terrain_media_deep`, and that triangle will *visually* look like an ankle splash but *gameplay-wise* will drown a giant. Loathing leaves this to the designer's judgment — there's no auto-classifier from depth to tier in the editor (`myth2_water_depth.cpp` is filling that gap).

This split also explains why Bungie didn't store depth as a number: there are only really four interesting outcomes (does the dwarf drown? does the human drown? does the giant drown? does anything drown?), so quantizing into 4 tiers is both more cache-friendly and lets the designer override geometry-implied depth for gameplay reasons (e.g., a shallow-looking puddle that's narratively a bottomless well).

## `GET_TRUE_CELL_HEIGHT` — the unified accessor

The [mesh.h:94](reference_source/vengeance_july27_2004/ToolCode/mesh.h#L94) macro shows the runtime's pragmatic view:

```c
#define GET_TRUE_CELL_HEIGHT(cell) (AT_LEAST_PARTIALLY_MEDIA(cell) \
    ? (cell)->media_height \
    : (cell)->height)
```

For sprite Z-sorting and projectile collision, "the height of this cell" means **media surface if any media is present, otherwise terrain.** A unit standing in water is considered to be at the water's surface for sorting purposes, not at the bottom. (`render_height` in the struct is a separate cached value the renderer uses for LOD/clipping; it's the max of physical_height and media_height plus any object protrusions.)

## How the project's tools handle it

Two separate tools mirror this duality:

- [`myth2_water_depth.cpp`](../myth2/myth2_water_depth.cpp) — **populates the gameplay tier** from the geometry tracks. Takes a terrain OBJ + water OBJ, computes `depth = water_z - terrain_z` per cell, and classifies into 4 tiers using user-supplied `level1/level2/level3` thresholds ([line 170-178](../myth2/myth2_water_depth.cpp#L170-L178)). This is the auto-classifier Loathing didn't have. The output is a `water_generated.bmp` color-coded by tier, ready to be painted into the terrain-type nibbles.
- [`myth2_media_height.cpp`](../myth2/myth2_media_height.cpp) — works the geometry side: read/write `media_height` directly.
- [`myth2_assemble.cpp:496`](../myth2/myth2_assemble.cpp#L496) `estimateMediaHeight()` — when you flood-fill a wet region without an explicit height, it picks the **maximum** `physical_height` of the cell and its three diagonal neighbors. That's a sensible "fill to the brim" default — the water surface sits at the highest terrain corner of the patch, guaranteeing the cell is at least slightly submerged.

## Tying it back to the flag bits

| Bit | Field | Role for water depth |
| --- | --- | --- |
| 0–3 | terrain[1] | Gameplay depth tier for triangle 1 (one of the 4 media types if wet) |
| 4–7 | terrain[0] | Gameplay depth tier for triangle 0 |
| 8 | `vertex_is_media` | "media_height for this vertex is real, not garbage." Set whenever the vertex anchors a wet cell. Without this, the engine treats the vertex as dry and ignores `media_height`. |
| 10 | `vertex_is_animated_media` | Per-vertex gate for sinusoidal wave displacement of the media surface (X/Y/Z sine offsets at render time). Set iff all 4 cells touching the vertex have both triangles flagged media. **Recomputed by the engine at mesh load**, so disk-format changes are overwritten unless topology (bits 11/12) is also edited. See [bit_10.md](bit_10.md). |
| 11/12 | `triangle{0,1}_is_media` | This triangle is wet — read its terrain-type nibble as a depth tier rather than as a dry terrain. |

So when you assemble a wet region, a single cell typically gets bits 8, 11, 12 set (often 10 too if it's open water rather than a stagnant pool), one of media types 0–3 painted into both terrain nibbles, and `media_height` populated.

## Practical edge cases worth knowing

1. **Variable-depth pools.** Because `media_height` is per-vertex and depth tier is per-triangle, you can have a pool where the surface tilts gently across the patch. As long as each triangle's tier matches its average depth, gameplay reads correctly. The `myth2_water_depth` classifier handles this since it operates per cell.

2. **Negative `media_height`.** Perfectly legal — water below the world reference plane. Caves and indoor maps use this.

3. **Wet on a dry vertex.** If a triangle is flagged wet but the vertex has bit 8 cleared, `media_height` reads as junk. Loathing's water tool always sets bit 8 on all four corners ([TMeshView.cpp:2542](reference_source/vengeance_july27_2004/ToolCode/TMeshView.cpp#L2542)) precisely to avoid this. The current assembler clears bits 10/13 on dry cells but doesn't clear bit 8 — worth checking that bit 8 is also gated on at least one neighboring wet triangle to stay consistent.

4. **The `media_type` field in the mesh header.** This is global per map ([mesh.h:430](reference_source/vengeance_july27_2004/ToolCode/mesh.h#L430)) — picks which `medi` tag (water vs. lava vs. swamp vs. blood) the depth tiers reference. So `_terrain_media_giant_depth` on Crow's Bridge means "neck-deep water" but on a fire level means "giant-engulfing lava." Same bit pattern, totally different gameplay effect, controlled by one map-level enum.
