# Myth II `mesh_cell.flags` — Bit 10 (`_mesh_vertex_is_animated_media_bit`)

> **Status: confirmed by disassembly.** Bit 10 is the per-vertex gate for sinusoidal wave displacement of media (water/lava/swamp/blood) surfaces. The runtime engine reads it in the rendering path and applies X/Y/Z sine offsets to vertices where it's set. **However, the engine recomputes bit 10 from cell topology at mesh load time, overwriting any value present on disk** — which is why earlier empirical tests of clearing the bit saw no visual change.

## What the engine actually does

Three pieces, all confirmed by Ghidra disassembly of the Project Magma 1.8.5.471 binary.

### 1. The renderer reads bit 10 to gate wave displacement

`FUN_004c0b70` is the per-vertex render-prep loop. The decompiled gate (lines 780–798 of `out/trace_bit10.txt`):

```c
if ((((*(byte *)(unaff_EDI + 4) & 8) == 0) &&
    (((*psVar24 == -1 || uVar8 != 0) && ((psVar24[-3] & 0x100U) != 0)))) &&    // bit 8
   (iVar18 = (int)psVar24[-1], (psVar24[-3] & 0x400U) != 0)) {                 // bit 10
  iVar25 = *(int *)(unaff_EDI + 0x3c) + local_144 + iVar21;                    // wave phase
  if (*(short *)(iVar7 + 0x68) != 0) {
    iVar13 = iVar13 + (sine_table[freq_x*phase>>6 & 0x3ff] * amp_x >> 14);
  }
  if (*(short *)(iVar7 + 0x6a) != 0) {
    iVar16 = iVar15 + (sine_table[freq_y*phase>>6 & 0x3ff] * amp_y >> 14);
  }
  if (*(short *)(iVar7 + 0x6c) != 0) {
    iVar18 = iVar18 + (sine_table[freq_z*phase>>6 & 0x3ff] * amp_z >> 14);
  }
}
```

Decoded:

- `psVar24` walks a stride-12 array (= `sizeof(mesh_cell)`); `psVar24[-3]` is the short at offset −6 from `psVar24`'s logical position, which corresponds to the `flags` field of the cell.
- The conditions chain to: render this vertex's wave displacement only if bit 8 (`vertex_is_media`) **and** bit 10 (`vertex_is_animated_media`) are both set, plus a couple of ancillary checks.
- `&DAT_006a9100` indexed with `& 0x3ff` is a **1024-entry sine table**.
- The struct at `iVar7` (pointed to by global `DAT_005314c0`) holds wave parameters per axis: amplitudes at offsets +0x68/+0x6a/+0x6c, frequencies at +0x6e/+0x70/+0x72.
- For each axis with non-zero amplitude, the vertex coordinate (iVar13/iVar16/iVar18 = X/Y/Z) gets a sinusoidal offset added.

**With bit 10 set, the vertex wobbles. With bit 10 clear, the vertex stays at its `media_height`.** The original "wobbles when media" interpretation from this doc's earlier version was correct.

### 2. There's an additional bit-10 gate in the particle/effect subsystem

Two more readers exist:

- **`FUN_004a42bb`** (called from particle iteration loop `FUN_004a4074`): bit-10 gates a spatial-overlap test for a specific particle subtype identified by `(typeword >> 0xb & 3) == 2 && (typeword & 0xe000) == 0x6000`. Effect not yet pinned down — likely an effect that should only spawn over interior media (splash/foam/bubble candidates).
- **Orphan code at `004a2975`** (in particle_systems.c address range, function not auto-detected by Ghidra): explicit `TEST EDI, 0x400 / JZ skip` after `TEST EDI, 0x200 / JNZ skip`. The combined logic skips processing for cells that are on fire OR don't have bit 10 set. Operates on a parallel 80-byte struct (probably a per-cell particle/animation record) at base `DAT_00532e6c`.

So bit 10 also enables some effect/particle behavior beyond the renderer, but the wave displacement is the headline use.

### 3. The engine recomputes bit 10 at mesh load — this is what masked the test

`FUN_00472ce6` (in `mesh.c`, called only by mesh-load function `FUN_00471534` at `00471753`) walks every interior cell and recomputes bit 10 from the topology of bits 11 and 12 in the four cells meeting at each vertex:

```c
for (each interior cell):
    flags_self = load_cell_flags(self)        // (col, row)
    flags_W    = load_cell_flags(west)        // (col-1, row)
    flags_N    = load_cell_flags(north)       // (col, row-1)
    flags_NW   = load_cell_flags(northwest)   // (col-1, row-1)
    all_AND = flags_self & flags_W & flags_N & flags_NW
    all_OR  = flags_self | flags_W | flags_N | flags_NW

    new_flags = flags_self & 0xfdff   // clear bit 9 (was/is on fire)

    if (all_OR & 0x1800 != 0) && (all_OR & 0x4000 == 0) && (render_height == -1):
        new_flags |= 0x100   // set bit 8 (vertex_is_media)

    if (all_AND & 0x800 != 0) && (all_AND & 0x1000 != 0):
        new_flags |= 0x400   // set bit 10 (vertex_is_animated_media)

    write back, clearing bit 14
```

The bit-10 rule: **set bit 10 iff all four cells touching this vertex have BOTH triangles flagged media (bits 11 AND 12).** That's exactly the "interior of wet area" pattern observed in extracted maps — confirming Loathing must have used the same rule (or the engine quietly fixes it up).

Critically, **this runs in the engine, not just in the editor.** Whatever bit 10 value sits in the disk format gets overwritten by the topology recompute before the renderer sees it. The disk value is effectively advisory.

## Why the earlier empirical test couldn't see bit 10's effect

The previous version of this doc said the wave-wobble hypothesis was empirically falsified, because clearing bit 10 across all 2099 cells of a map produced no visible change. That conclusion was wrong — but the test itself was correct and the result is consistent with the new picture:

```text
[disk] bit 10 cleared in animation.bmp
   ↓
[load] mesh data read into memory with bit 10 cleared
   ↓
[load] FUN_00472ce6 runs: recomputes bit 10 from neighborhood topology
   ↓                       — restores bit 10 to its original value, since
   ↓                         bits 11/12 weren't changed
[render] FUN_004c0b70 reads bit 10: still set on the same cells as before
   ↓
[screen] waves still happen
```

The test result was real, the renderer behavior was unchanged, but the chain went through a step we hadn't yet observed. With the recompute in the picture, the original hypothesis is the simplest explanation of all the evidence.

## How to actually test bit 10's effect on rendering

Three approaches, in increasing order of invasiveness.

### Approach A — Edit the medi tag (no patching, recommended for visual confirmation)

The wave parameters live in a struct pointed to by global `DAT_005314c0` — almost certainly per `medi` tag. Set the X/Y/Z amplitudes (offsets +0x68/+0x6a/+0x6c) to zero in the `medi` tag for the relevant media type and waves stop everywhere on every map using that media. This doesn't *isolate* bit 10's effect, but it confirms the renderer pipeline behaves as decoded and gives you a knob for authoring "still water" maps.

### Approach B — Modify topology so the recompute leaves bit 10 clear

Clear bit 11 or bit 12 on at least one cell touching the target vertex. The topology rule fails → bit 10 stays clear after recompute → renderer skips wave displacement at that vertex. Side effect: those cells lose media status (gameplay-wise too), which probably isn't what you want for a pure visual test. Useful when designing "shoreline puddles that don't ripple."

### Approach C — Patch the binary to skip the recompute

Two small patches achieve clean isolation:

1. **Skip the writer**: NOP the `CALL FUN_00472ce6` instruction at `00471753`. Bit 10 stays as loaded from disk. Now your `animation.bmp` edits actually reach the renderer.
2. **Skip the reader instead**: NOP the `JZ` after `TEST [psVar24-3], 0x400` in `FUN_004c0b70`. This forces the renderer to treat every wet vertex as bit-10-set (waves everywhere) — useful for the inverse experiment.

Ghidra's patch-instruction feature can do both. Save and re-export the .exe, run with your existing modified maps, and you should now see a definitive on/off effect.

## Updated TL;DR

```text
bit 10 = _mesh_vertex_is_animated_media_bit
       = per-vertex gate for sinusoidal wave displacement of media surfaces
       = renderer applies X/Y/Z sine offsets to vertex coords when set
       = also gates an unspecified particle-system effect (FUN_004a42bb)
       = computed at mesh load from topology: set iff all 4 surrounding
         cells have BOTH triangles flagged media
       = engine recomputes on every load — disk value is effectively
         advisory; tooling that wants to override bit 10 must also edit
         topology (bits 11/12) or patch the engine
```

## Method note for future bit investigations

The earlier "falsification" reminded us that disassembly evidence and behavioral evidence sometimes disagree because of an intermediate step (here, a load-time recompute). The pattern to watch for:

- If a binary has a clear *consumer* of a bit, and changing the bit on disk produces no behavior change, suspect that some intermediate function is normalizing/recomputing it.
- The shipping mesh-load sequence in this binary runs at least three normalization passes (`FUN_00472cb4`, `FUN_00472ce6`, `FUN_00472df2`) — bits 8 and 10 are both produced from topology in `FUN_00472ce6`, and probably other bits get fixed up in the others.
- A good early step in any future bit investigation is to xref the mesh-load function (`FUN_00471534` here) and decompile every cell-flag-touching helper it calls *before* attempting to test bit changes empirically.
