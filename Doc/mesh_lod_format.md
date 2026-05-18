# mesh_LOD_data Format

Reverse-engineered from Loathing 1.8.4 (`G:\Myth_2\Loathing 1.8.4.exe`) via Ghidra decompilation of `FUN_00419220` (entry), `FUN_004415d0` (rectangle-scoped recompute), and `FUN_00440730` (per-LOD-cell compute).

Generator implementation: [`myth2/build_mesh.cpp::generateMeshLodFromCells`](../myth2/build_mesh.cpp).

## Section layout

- Header field `mesh_LOD_data_offset` at 0xCC; `mesh_LOD_data_size` at 0xD0.
- Total size = `(cellW / 2) × (cellH / 2) × 2` bytes — 18,432 for a 192×192 mesh.
- Stored as a row-major array of uint16 BE entries; `(LW × LH)` entries where `LW = cellW/2`, `LH = cellH/2`.
- Each LOD entry covers a 2×2 cell block: LOD entry `(lx, ly)` covers cells `[lx*2, lx*2+1] × [ly*2, ly*2+1]`.

## Per-entry encoding

Each 16-bit entry packs slope, edge, and flag information for the underlying 2×2 cell block. Bit layout:

| Bit | Hex | Meaning |
| --- | --- | --- |
| 0 | 0x0001 | N edge: top-row slope detected, or render_height/flags propagation |
| 1 | 0x0002 | E edge |
| 2 | 0x0004 | S edge |
| 3 | 0x0008 | W edge |
| 4 | 0x0010 | (cellX, cellY) flags bit `0x4000` is NOT set |
| 5 | 0x0020 | (cellX+1, cellY) flags bit `0x4000` is NOT set |
| 6 | 0x0040 | (cellX+1, cellY+1) flags bit `0x4000` is NOT set |
| 7 | 0x0080 | (cellX, cellY+1) flags bit `0x4000` is NOT set |
| 8 | 0x0100 | (cellX, cellY) flags bit `0x1000` |
| 9 | 0x0200 | (cellX+1, cellY) flags bit `0x800` |
| 10 | 0x0400 | (cellX+1, cellY) flags bit `0x1000` |
| 11 | 0x0800 | (cellX+1, cellY+1) flags bit `0x1000` |
| 12 | 0x1000 | (cellX+1, cellY+1) flags bit `0x800` |
| 13 | 0x2000 | (cellX, cellY+1) flags bit `0x1000` |
| 14 | 0x4000 | (cellX, cellY+1) flags bit `0x800` |
| 15 | 0x8000 | (cellX, cellY) flags bit `0x800` |

Sentinel: if `(L & 0xFFF0) == 0xFFF0` after all bits are accumulated, the entry is forced to `0xFFF0`.

## Generation algorithm (per LOD cell)

**Cell record fields (12 bytes BE per cell):**

- 0–1: `physical_height` (int16 BE)
- 4–5: `flags16` (uint16 BE) — the cell's flags field
- 10–11: `render_height` (int16 BE; `-1` = unset)

**Step 1: slope detection.** Take 9 cells in a 3×3 region anchored at `(cellX, cellY)` with `cellX+2` / `cellY+2` clamped to mesh bounds. Let `H[p]` = physical_height of cell `p`. For each of these six second-derivatives, set the indicated bits when `|H[p₀] − 2·H[p₁] + H[p₂]| > 128`:

- `(cellX, cellY) → (cellX+1, cellY) → (cellX+2, cellY)`: top edge → bit 0
- `(cellX+2, cellY) → (cellX+2, cellY+1) → (cellX+2, cellY+2)`: east edge → bit 1
- `(cellX, cellY+2) → (cellX+1, cellY+2) → (cellX+2, cellY+2)`: bottom edge → bit 2
- `(cellX, cellY) → (cellX, cellY+1) → (cellX, cellY+2)`: west edge → bit 3
- `(cellX+1, cellY) → (cellX+1, cellY+1) → (cellX+1, cellY+2)`: middle column → bits 0–3 (`0x0F`)
- `(cellX, cellY+1) → (cellX+1, cellY+1) → (cellX+2, cellY+1)`: middle row → bits 0–3 (`0x0F`)

**Step 2: render_height presence.** For each 2×2 corner cell, if `render_height ≠ −1`, OR in a specific edge-bit pair:

- top-left (cellX, cellY) → `0x09` (bits 0+3)
- top-right (cellX+1, cellY) → `0x03` (bits 0+1)
- bottom-right (cellX+1, cellY+1) → `0x06` (bits 1+2)
- bottom-left (cellX, cellY+1) → `0x0C` (bits 2+3)

**Step 3: flags bit `0x4000`.** Per 2×2 corner cell, if the cell's `flags16 & 0x4000` is set, OR in the same edge-bit pair as Step 2; otherwise OR in the corresponding "absent" marker bit:

- top-left: set → `0x09`, else → `0x10`
- top-right: set → `0x03`, else → `0x20`
- bottom-right: set → `0x06`, else → `0x40`
- bottom-left: set → `0x0C`, else → `0x80`

**Step 4: flags bits `0x0800` / `0x1000` propagation** (per the table above).

**Step 5: XOR post-pass.** After all prior bits accumulate, check pairs for mismatch and light the matching edge bit:

- `(L & 0x0100) != (L & 0x0200)` → set bit 0
- `(L & 0x0400) != (L & 0x0800)` → set bit 1
- `(L & 0x1000) != (L & 0x2000)` → set bit 2
- `(L & 0x4000) != (L & 0x8000)` → set bit 3

**Step 6: sentinel.** If `(L & 0xFFF0) == 0xFFF0`, force `L = 0xFFF0`.

## Neighbor edge propagation (second pass over all LOD entries)

After every entry is computed, walk the buffer linearly and propagate edge bits between neighbors. The neighbor clamp is at the **buffer level**, not the 2D grid level (Loathing's pointer arithmetic clamps to buffer base and `base + size - 1`):

- For entry at index `i`: `N = lod[max(0, i − LW)]`, `S = lod[min(total−1, i + LW)]`, `E = lod[min(total−1, i + 1)]`, `W = lod[max(0, i − 1)]`.
- If entry's bit 0 is clear and `N`'s bit 2 is set → set bit 0.
- If entry's bit 1 is clear and `E`'s bit 3 is set → set bit 1.
- If entry's bit 2 is clear and `S`'s bit 0 is set → set bit 2.
- If entry's bit 3 is clear and `W`'s bit 1 is set → set bit 3.

The pass is in-place; later entries see earlier mutations.

## Empirical validation

Generator output compared byte-for-byte against the Bungie-shipped `mesh_tag.bin` LOD section for le3e: **18,401 / 18,432 bytes match (99.83%)**. The remaining 31 single-bit differences are all in row 0 / column 0 of the LOD grid and appear to be drift between Bungie's original generator and Loathing's recompute logic. Plugin built from the with-blob mesh with the LOD region replaced by generator output loads cleanly in Myth II 1.8.5 with correct terrain rendering.

## Remaining unknowns

- The semantic meaning of `flags16` bits `0x4000`, `0x0800`, `0x1000`. They're treated as opaque "propagate to LOD" bits in the algorithm; we don't yet know what mesh feature they correspond to (texture transition? path edge? something else?).
- Why Bungie's shipped LOD has those 31 edge-bit differences from Loathing's recompute. Likely an algorithm tweak between Bungie's internal editor and the version Loathing was derived from.
