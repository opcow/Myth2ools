# Myth II `mesh_cell.flags` — Bit 15 (`_mesh_cell_will_be_marked_impassable_bit`)

## What it actually is: scenery-driven impassability staging

The Loathing manual at projectmagma.net describes a category called **Scenery Impassable**:

> "These purple cells are created automatically when scenery items are placed. This option is not available as an option in the menu window, and scenery impassable cells cannot be edited until the scenery is moved."

Bit 15 is almost certainly the storage for that "scenery-locked" state. The flag's name —`_mesh_cell_will_be_marked_impassable_bit`, *will be*, future tense — is the runtime hint: it's not "this cell is impassable" (that's already encoded in the terrain-type nibbles, bits 0–7) but rather "Loathing has computed that this cell sits inside a scenery footprint and needs to be impassable when shipped."

The lifecycle inside the editor:

1. Designer drops a tree, wall, rock, or other scenery item.
2. Loathing computes the cell footprint that scenery occupies (and sets bit 15 on those cells).
3. Those cells render purple in the editor and are locked from manual painting.
4. If the designer moves the scenery, the bit is cleared from the old footprint and stamped on the new one.
5. On save / map export, Loathing walks every cell with bit 15 set, rewrites the terrain‑type nibbles to `_terrain_walking_impassable` / `_terrain_flying_impassable` based on the scenery type, then clears the bit.

The shipped map contains only the *result* (impassable terrain types in the cell nibbles). Bit 15 itself never leaves the editor.

## Why it's editor-only (`#ifdef LOATHING`)

[`reference_source/vengeance_july27_2004/ToolCode/mesh.h:167-181`](reference_source/vengeance_july27_2004/ToolCode/mesh.h#L167-L181) gates the bit behind `#ifdef LOATHING`. In non‑editor builds the symbol doesn't compile, so the runtime engine literally cannot read or write that bit. The cell struct has the same 16-bit `flags` field in both builds, but the editor is the only consumer that has a name for bit 15.

This separation matters because bit 15 is bookkeeping that links cells back to scenery items — information the runtime doesn't need. Once the scenery is final and the map is baked, the link is collapsed into terrain types and the cells are just impassable, full stop.

**The bit doesn't physically exist as a meaningful value in shipped maps.** If an extractor reads a 1 there from a real Myth II `.gor`, it almost certainly means one of:
- The editor crashed mid‑save and left staging data in
- The map was exported by a non‑release build of Loathing
- The map was saved by a less careful fan-made tool

## Why defer it at all? — design rationale

A few reasons editors defer this kind of mutation rather than rewriting terrain types immediately when scenery is placed:

1. **Reversibility on scenery moves.** If moving a tree had to read back the terrain types of every cell it just left and *guess* what they were before placement, the editor would either need a history buffer per cell or would lose the original terrain. Bit 15 means the original terrain nibbles are untouched while the scenery is around — moving the tree just clears the bit on the old cells and stamps it on the new ones.

2. **Triangle-level resolution from cell-level footprints.** Impassability is *per triangle* (two nibbles per cell), but a scenery footprint is *per cell*. Deferring lets the bake step look at neighbors and decide which of a cell's two triangles needs to be marked.

3. **Computed vs. authored impassability.** Some cells become impassable automatically (scenery, steep slope, fence connectors); some are painted by the designer using the passability tool. The bit distinguishes "scenery put me here" from "designer painted me." Important for the editor's UI (purple vs. red cells, lockability) but useless to the runtime.

4. **Batch validation before commit.** Loathing can warn about pathfinding orphans, units placed in newly-impassable footprints, etc. at save time rather than mid-brushstroke.

## TL;DR

```text
bit 15 = _mesh_cell_will_be_marked_impassable_bit  (#ifdef LOATHING only)
       = "this cell sits inside a scenery footprint; bake to impassable on save"
       = the storage for Loathing's purple "Scenery Impassable" cells
       = converted to impassable terrain type at export, then cleared
       = should never appear set in a shipped Myth II map
```

Production maps don't carry "marked-for-impassable" metadata — they carry impassable terrain *types*. Bit 15 is the workflow artifact that links scenery placement to that final terrain rewrite, gated out of the shipping format precisely so authoring intent and runtime data stay cleanly separated.

It's a small but very Bungie-flavored design choice: keep the authoring-time "what the human meant" data adjacent to but distinct from the runtime "what the machine reads" data, and use the compiler to enforce the boundary.
