# Myth II Water Discussion

## Purpose

This note captures the current understanding of Myth II water handling and the workflow ideas discussed for authoring and round-tripping water in the extractor tools.

It is intentionally a working document rather than final documentation.

## Current Findings

### Myth II water is not just a 2D mask

Myth II water appears to be composed of several layers of state stored in the mesh:

- wet/dry state per triangle
- media terrain type per triangle
- `media_height` per cell
- `vertex_media` state
- animated/wavy-water state
- reflection-related state

The important practical split is:

- `physical_height` controls terrain elevation
- `media_height` controls the rendered water surface elevation
- passability and wetness come from flags / terrain-type state rather than `media_height` alone

### Animated-media bit is topology-derived

Recent testing and binary analysis strongly suggest that the animated-media bit
(bit `10`) is not a freely authored gameplay/render control in normal engine
behavior.

Instead:

- bit `10` is set for a vertex if and only if the four cells meeting at that
  vertex are all fully wet
- this is derived from the wet/dry topology, not painted independently

That explains earlier observations:

- newly painted water produced an inset animated region automatically
- clearing `animation.bmp` did not stop visible water motion
- `animation.bmp` can round-trip on disk while still being overridden in memory

Practical conclusion:

- `water.bmp` remains the real authored water/media layer
- `animation.bmp` is best treated as a diagnostic/exported view of the derived
  bit-10 pattern
- changing wet topology is the reliable way to change the in-memory bit-10
  pattern without patching the game

### Safe/default water editing path

The current safe Myth II workflow uses `water.bmp` in flags/types-only mode.

That means:

- water placement/type can be round-tripped safely
- `media_height` is left unchanged
- this avoids the embossed/raised-water problem seen in earlier experiments

### What caused the embossed water problem

Testing showed that the embossed or raised-surface water issue came from rewriting `media_height`, not from:

- OBJ terrain insertion
- `render_height`
- the basic wet/type flags alone

Changing `media_height` alone can make water render below or above the terrain while units still walk on the original land surface.

Observed behavior:

- lowering `media_height` makes water appear in a canyon below the land
- raising `media_height` makes water appear to cut through or float above the terrain
- units still walk as before, implying pathing is not driven by `media_height`

## Geometry and Export Observations

### Terrain

For `85gy`:

- mesh cells: `160 x 160`
- implied grid points: `161 x 161`
- texture size: `1280 x 1280`
- pixels per cell: `8 x 8`

The extra row/column of grid points closes the final ring of cells; it does not create extra quads.

### Water surface OBJ

A dedicated water surface OBJ exporter was added so the current water surface can be inspected in Blender.

That exporter:

- aligns to the terrain OBJ coordinate system
- uses `media_height` as vertex `Y`
- writes only wet triangles
- uses an MTL that points to `terrain/water.bmp`

This made it possible to inspect actual water levels on `85gy`.

## Important Visual Discovery from `85gy`

Inspection of the water surface on `85gy` suggests:

- the water around the outer playfield is all at one shared level
- a lake nearer the middle sits at a different, higher level
- water bodies appear mostly flat rather than varying continuously like terrain

This suggests Myth II water may usually be authored as a small number of flat water bodies, each with a constant `media_height`.

That matches real-world intuition as well:

- still water tends to have a level surface
- shoreline and extent vary horizontally
- height usually changes per lake/body rather than per cell

## Candidate Authoring Models

### Model A: multiple 2D water maps named by height

Example:

- `water_-1146.bmp`
- `water_394.bmp`

Each map would define all water at a particular level.

Advantages:

- easy to paint in Photoshop
- simple import logic
- matches the observation that many water bodies are flat

Problem:

- a modder creating new water may not know what height to assign
- the filename-based height workflow is not intuitive for authoring from scratch

### Model B: paint in 2D, position in 3D

This is the more promising workflow discussed.

Proposed pipeline:

1. Paint the water shape in Photoshop as a BMP mask.
2. Convert that mask into one or more flat OBJ water surfaces at a default elevation such as `0`.
3. Load the water OBJ into Blender alongside the terrain OBJ.
4. Move each water body up or down to the desired height.
5. Re-export the water OBJ.
6. Use that OBJ to drive `media_height` insertion back into the map.

Benefits:

- Photoshop is used for water shape
- Blender is used for water level
- the modder does not need to know numeric water heights ahead of time
- the geometry can remain very simple because water bodies are mostly flat

## Likely Best Direction

The best authoring model currently appears to be a hybrid:

- `water.bmp` defines horizontal water extent / wet triangles
- water-body OBJ geometry defines vertical water level

This keeps the right job in the right tool:

- 2D painting for shape
- 3D placement for height

## Strong Candidate Workflow

### Export side

The tools should eventually be able to:

- export connected water bodies as separate flat OBJ objects or groups
- keep them aligned with the terrain OBJ
- optionally preserve current `media_height` in the exported geometry

### Authoring side

The modder would:

- edit or repaint water shape in BMP form
- generate or update flat water-body geometry
- move bodies vertically in Blender

### Import side

The tools should eventually:

- read the water-body OBJ
- determine which cells/triangles are covered
- set `media_height` from the object elevation
- continue using `water.bmp` for wet/type layout

## Open Questions

- How should overlapping water-body objects be resolved on import?
- Should export create one object per connected body, or one object with multiple groups?
- Is `watermask.bmp` actually useful in this workflow, or only as a diagnostic artifact?
- Are there Myth II maps with truly non-flat water bodies, or is flat-per-body nearly universal?
- How should shoreline triangle assignment be derived when a painted mask and water-body geometry differ slightly?

## Shoreline Working Rule

Current thinking is that shoreline should not be treated as a separate hand-authored structure.

Instead:

- `water.bmp` should define a generous horizontal footprint for the water
- that footprint can overlap the shoreline slightly
- that overlap is desirable because it helps avoid visible cracks or gaps where water meets land

This matches the observed relationship in:

- Myth TFL, where `water.bmp` appears broader than the tighter water mask
- `85gy`, where the water footprint also appears broader than the final visible edge behavior

### Practical interpretation

If a cell is touched by the painted water footprint, it should probably become at least partially wet.

That implies the shoreline can be derived from triangle coverage:

- neither triangle covered = dry cell
- one triangle covered = shoreline / partial wet cell
- both triangles covered = fully wet cell

Under this model:

- `water.bmp` provides the horizontal water extent
- water-body OBJ geometry provides the vertical water level
- wet/dry triangle bits are derived from coverage against the cell's actual diagonal split

So the shoreline is not a third authored layer. It is the result of:

- a slightly generous 2D water footprint
- plus per-triangle wet assignment

This seems like the simplest and most robust working model so far.

## Current Recommendation

Do not treat Myth II water as fully freeform sculpted geometry by default.

Treat it as:

- mostly flat water bodies
- with 2D masks for extent
- and simple 3D elevation placement for each body

That appears to match both the observed data and an intuitive modding workflow.

## Water Depth Generation Progress

A standalone generator now derives Myth II water media types from:

- terrain displacement OBJ
- water OBJ height
- user-supplied type thresholds

### Important result

The key improvement was switching from:

- one average depth per wet triangle

to:

- per-pixel depth classification inside each wet triangle using interpolated terrain and water heights

This removed most of the obvious faceted triangle-band look and produced much more natural water-depth regions.

### Threshold model

The best control model so far is explicit thresholds where deeper media types begin:

- type `0` is the default
- type `1` begins at threshold 1
- type `2` begins at threshold 2
- type `3` begins at threshold 3

This worked better than forcing equal spacing across a single near/far range.

### Current best `85gy` result

For `85gy`, the best-looking threshold set found so far is:

- `160 500 840`

Command:

```powershell
.\Release\water_depth.exe .\85gy .\85gy\displacement.obj .\85gy\85gy_water.obj 160 500 840
```

### Smoothing conclusion

An optional `--smooth` pass still exists, but on `85gy` it does not appear to make a noticeable visual difference once per-pixel interpolation is in place.

So the current conclusion is:

- the main quality improvement comes from per-pixel interpolated depth classification
- smoothing is optional and may be a no-op on some maps
- the threshold values matter much more than the smoothing toggle

## OBJ Orientation Note

The Myth II OBJ exporters now use a settled Blender-facing convention:

- terrain OBJ geometry matches the in-game / `color.bmp` orientation
- terrain OBJ uses a horizontally flipped UV convention so the textured playfield reads correctly in Blender
- water OBJ geometry matches the settled terrain OBJ world orientation
- water OBJ keeps the water UV convention that makes `water.bmp` align correctly on the water surface
- terrain OBJ import and assembler-side terrain OBJ import both match the current terrain geometry convention
- assembler-side water OBJ import matches the current water OBJ geometry convention for `media_height` round-tripping

This means:

- terrain and water should line up in Blender without manual rotation or mirroring
- the terrain texture should read the way the map appears in game
- `water.bmp` should align on the water surface with the current water OBJ UV convention
- this orientation should be treated as the baseline for future OBJ-based water tooling
