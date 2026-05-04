# Myth 2 Map Tools

Small console tools for extracting, editing, and rebuilding **Myth II: Soulblighter** map assets.

## Upfront

This project was originally based on my code and notes from the 1990s, which was made possible in large part by information discovered by many other smart and generous map hackers in the Myth:TFL modding community at that time. I compiled the information, discovered some on my own, and I wrote the original code.

The point: I am 65 years old with 3 dogs, a family, and other commitments. I have neither the time nor the inclination to spend days or weeks updating this code for modernity, but I have much nostalgia for Myth OG and Myth 2, and I thought it would be fun to see what could come of these old tools.

And so I did, with the help of both Claude Code and Codex AI coding tools. Without them, this would have never happened. Regardless of all the hate for "vibe coding," these tools are incredibly useful. No apologies.

This is largely an academic exercise. I know there are other tools out there, and some of this is redundant, but I do hope others may find this project interesting or useful.

## Background

In the late 90s I created software called MythTech for Mac OS. MythTech was, to the best of my knowledge, the first publicly available tool to allow modders to graphically edit Myth: The Fallen Lords map meshes.

The process was a bit of a hack in that MythTech didn't know anything about 3D file formats. All of the exportable map elements were created as image files, which could be brought into Photoshop or other image editing software as layers. It was surprisingly effective for what it was. Using just a grayscale image, 3D terrain could be created and imported back into Myth's mesh files.

MythTech had many other utility functions, but that's the gist of it.

The tools here were based on that old Metroworks Codewarrior project. First working as MythTech did with Myth: TFL maps, but then moving to focus on Myth II: Soulblighter maps. 

## Building

### With CMake
```
mkdir build && cd build
cmake ..
cmake --build .
```

### Source Layout

- `tfl/` contains Myth: The Fallen Lords-era tools and exploratory helpers.
- `myth2/` contains Myth II: Soulblighter tools and shared Myth II headers.

### Quick compile (MSVC)
```
cl /EHsc /O2 tfl\myth_extract.cpp /Fe:myth_extract.exe
cl /EHsc /O2 tfl\myth_assemble.cpp /Fe:myth_assemble.exe
cl /EHsc /O2 tfl\myth_mesh.cpp /Fe:myth_mesh.exe
cl /EHsc /O2 tfl\myth_mesh_import.cpp /Fe:myth_mesh_import.exe
cl /EHsc /O2 myth2\myth2_dump.cpp /Fe:myth2_dump.exe
cl /EHsc /O2 myth2\myth2_extract.cpp /Fe:myth2_extract.exe
cl /EHsc /O2 myth2\myth2_mesh.cpp /Fe:myth2_mesh.exe
cl /EHsc /O2 myth2\myth2_water_mesh.cpp /Fe:myth2_water_mesh.exe
cl /EHsc /O2 myth2\myth2_water_depth.cpp /Fe:myth2_water_depth.exe
cl /EHsc /O2 myth2\myth2_mesh_import.cpp /Fe:myth2_mesh_import.exe
cl /EHsc /O2 myth2\myth2_mesh_diff.cpp /Fe:myth2_mesh_diff.exe
cl /EHsc /O2 myth2\myth2_mesh_dump.cpp /Fe:myth2_mesh_dump.exe
cl /EHsc /O2 myth2\myth2_mesh_summary.cpp /Fe:myth2_mesh_summary.exe
cl /EHsc /O2 myth2\myth2_normal_analyze.cpp /Fe:myth2_normal_analyze.exe
cl /EHsc /O2 myth2\myth2_normal_compare.cpp /Fe:myth2_normal_compare.exe
cl /EHsc /O2 myth2\myth2_normal_table.cpp /Fe:myth2_normal_table.exe
cl /EHsc /O2 myth2\myth2_media_height.cpp /Fe:myth2_media_height.exe
cl /EHsc /O2 myth2\myth2_media_dump.cpp /Fe:myth2_media_dump.exe
cl /EHsc /O2 myth2\myth2_core_dump.cpp /Fe:myth2_core_dump.exe
cl /EHsc /O2 myth2\myth2_proj_dump.cpp /Fe:myth2_proj_dump.exe
cl /EHsc /O2 myth2\myth2_assemble.cpp /Fe:myth2_assemble.exe
```

### Quick compile (GCC / Clang)
```
g++ -std=c++17 -O2 tfl/myth_extract.cpp -o myth_extract
g++ -std=c++17 -O2 tfl/myth_assemble.cpp -o myth_assemble
g++ -std=c++17 -O2 tfl/myth_mesh.cpp -o myth_mesh
g++ -std=c++17 -O2 tfl/myth_mesh_import.cpp -o myth_mesh_import
g++ -std=c++17 -O2 myth2/myth2_dump.cpp -o myth2_dump
g++ -std=c++17 -O2 myth2/myth2_extract.cpp -o myth2_extract
g++ -std=c++17 -O2 myth2/myth2_mesh.cpp -o myth2_mesh
g++ -std=c++17 -O2 myth2/myth2_water_mesh.cpp -o myth2_water_mesh
g++ -std=c++17 -O2 myth2/myth2_water_depth.cpp -o myth2_water_depth
g++ -std=c++17 -O2 myth2/myth2_mesh_import.cpp -o myth2_mesh_import
g++ -std=c++17 -O2 myth2/myth2_mesh_diff.cpp -o myth2_mesh_diff
g++ -std=c++17 -O2 myth2/myth2_mesh_dump.cpp -o myth2_mesh_dump
g++ -std=c++17 -O2 myth2/myth2_mesh_summary.cpp -o myth2_mesh_summary
g++ -std=c++17 -O2 myth2/myth2_normal_analyze.cpp -o myth2_normal_analyze
g++ -std=c++17 -O2 myth2/myth2_normal_compare.cpp -o myth2_normal_compare
g++ -std=c++17 -O2 myth2/myth2_normal_table.cpp -o myth2_normal_table
g++ -std=c++17 -O2 myth2/myth2_media_height.cpp -o myth2_media_height
g++ -std=c++17 -O2 myth2/myth2_media_dump.cpp -o myth2_media_dump
g++ -std=c++17 -O2 myth2/myth2_core_dump.cpp -o myth2_core_dump
g++ -std=c++17 -O2 myth2/myth2_proj_dump.cpp -o myth2_proj_dump
g++ -std=c++17 -O2 myth2/myth2_assemble.cpp -o myth2_assemble
```

---

## Tools

### `myth_extract`
```
myth_extract [-o] <tags.gor> <meshtag>
```

| Argument | Description |
|----------|-------------|
| `-o` | Overwrite existing extracted files instead of skipping them |
| `tags.gor` | Path to `tags.gor` for the map set you want to extract |
| `meshtag` | 4-character mesh tag name (for example `sega`, `balo`, `00tm`) |

### `myth_assemble`
```
myth_assemble <folder> [output.gor] [--edit] [--obj <input.obj>] [--heightscale <n>]
```

- `--edit` reapplies editable assets from the extracted folder before rebuilding.
- `--obj` imports terrain heights from a Wavefront OBJ and recomputes slope bytes.
- If `--edit` is used and `--obj` is omitted, the assembler auto-detects `<folder>/<tag>.obj`.
- When OBJ import is used, `height.bmp` is skipped, but `passability.bmp`, `water.bmp`, `animation.bmp`, and `terrain.bmp` still apply.

### `myth_mesh`
```
myth_mesh <tags.gor> <meshtag> [output.obj] [heightscale]
```

Exports the terrain grid as a Wavefront OBJ for editing in Blender.

### `myth_mesh_import`
```
myth_mesh_import <tag_folder> <input.obj> [heightscale]
```

Standalone OBJ importer for patching `raw/mesh_tag.bin` directly.

### `myth2_dump`
```
myth2_dump <file>
myth2_dump <file> list [type|all]
myth2_dump <file> entrypoints
```

Lists tags in Myth II `dng2` plugin files and `mth2` local tag files.

### `myth2_extract`
```
myth2_extract <tags_folder> <meshtag> [output_folder] [--ora]
myth2_extract <tags_folder> <meshtag> --out <output_folder> [--ora]
```

First-pass Myth II map extractor. It scans the supplied Myth II tags folder,
finds the requested `mesh` tag and its referenced assets, and writes to
`output_folder`, or to `<meshtag>/` when no output folder is supplied:

- `raw/mesh_tag.bin`
- `terrain/terrain_tag.bin`
- `terrain/terrain.bmp`
- `terrain/shadow.bmp`
- `terrain/water.bmp`
- `terrain/reflection.bmp`
- `terrain/animation.bmp`
- `terrain/passability.bmp`
- `terrain/passability_tri0.bmp`
- `terrain/passability_tri1.bmp`
- `screens/overhead.bmp` / `pregame.bmp` / `postgame.bmp` when present
- `strings/name.txt`
- `layers/` ordered bundle copies plus `layers/manifest.txt`
- `manifest.json`
- `layers/map_layers.ora` when `--ora` is used

The extra `passability_tri0.bmp` and `passability_tri1.bmp` files are
diagnostic exports. They show the per-cell terrain type if only one stored
triangle nibble is used across the whole cell, which is useful when comparing
against tools that appear to collapse mixed cells to a single representative
terrain/passability value.

When `--ora` is enabled, the extractor also writes a layered OpenRaster archive
containing the terrain-side layer stack:

- terrain
- shadow
- water
- passability
- reflection
- animation

### `myth2_mesh`
```
myth2_mesh <tag_folder> [output.obj] [heightscale]
```

Exports an extracted Myth II mesh folder as Wavefront `OBJ`. The exporter uses
the Myth II alternating cell diagonal pattern and a default height scale of
`1/512`.

### `myth2_water_mesh`
```
myth2_water_mesh <tag_folder> [output.obj] [heightscale]
```

Exports the current Myth II water surface as an OBJ aligned to the terrain OBJ.
It uses `media_height` for vertex Y and writes only wet triangles, with an MTL
that points to `terrain/water.bmp`.

### `myth2_water_depth`
```
myth2_water_depth <tag_folder> <terrain.obj> <water.obj> [level1] [level2] [level3] [output.bmp] [heightscale] [--smooth]
```

Generates a `water.bmp`-style media-type map from the depth between the terrain
OBJ and the water-surface OBJ. Wet triangle placement comes from the water OBJ.
The average triangle depth starts at type `0`, then steps up to types `1`, `2`,
and `3` at the optional raw-height thresholds you provide. `--smooth` runs a
single image-space cleanup pass to reduce isolated jagged triangle spikes.

### `myth2_mesh_import`
```
myth2_mesh_import <tag_folder> <input.obj> [heightscale]
```

Imports an edited Myth II OBJ back into `raw/mesh_tag.bin` by patching
`physical_height` only. Other per-cell fields are preserved.

### `myth2_mesh_diff`
```
myth2_mesh_diff <folder> <mesh_tag.bin|plugin>
```

Compares the extracted `raw/mesh_tag.bin` in a Myth II folder against another
mesh tag or a rebuilt plugin and reports which per-cell fields changed.

### `myth2_mesh_dump`
```
myth2_mesh_dump <folder> [mesh_tag.bin|plugin] [all|wet]
```

Dumps Myth II mesh cell fields as CSV-style text, with `wet` mode focusing on
cells whose media bits are set.

### `myth2_mesh_summary`
```
myth2_mesh_summary <folder> [mesh_tag.bin|plugin] [all|wet]
```

Groups Myth II mesh cells into repeated flag/height patterns and prints counts
plus a few sample coordinates for each pattern.

### `myth2_normal_analyze`
```
myth2_normal_analyze <folder> [index]
```

Empirically correlates the two stored 8-bit normal indices in each Myth II mesh
cell with geometric triangle normals computed from the displacement mesh.
Without an `index`, it prints one summary row per used normal index. With an
`index` in `0..255`, it prints detailed combined/high-byte/low-byte stats and a
few sample triangles for that index.

### `myth2_normal_table`
```
myth2_normal_table [index]
```

Reconstructs the Myth II 256-entry precalculated mesh normal table from the
engine-side startup logic reverse-engineered from `Myth II.exe`. Without an
argument, it prints all 256 entries. With an `index` in `0..255`, it prints the
decoded entry with fixed-point components and normalized floating-point values.

### `myth2_normal_compare`
```
myth2_normal_compare <folder>
```

Compares the reconstructed runtime normal table against empirical per-index
triangle-normal buckets from an extracted Myth II mesh. It scores simple
axis/sign permutations and reports the best basis alignment plus the lowest-
error matching indices.

### `myth2_media_height`
```
myth2_media_height <folder> <value>
```

Sets `media_height` to a single signed 16-bit value for every cell in an
extracted Myth II `raw/mesh_tag.bin`. This is a direct experiment tool for
testing what a flat water-surface height does to a map.

### `myth2_media_dump`
```
myth2_media_dump <file> list
myth2_media_dump <file> <id>
```

Dumps a Myth II `medi` tag in human-readable form from a foundation, plugin,
or local tag file. `list` enumerates the available `medi` tags in a file.
Useful for inspecting stock media definitions such as `wate`, `wagr`, `wamu`,
and `wame`.

### `myth2_core_dump`
```
myth2_core_dump <file> list
myth2_core_dump <file> <id>
```

Dumps a Myth II `core` collection-reference tag in human-readable form.
Useful for inspecting the collection/tint side paired with `medi` tags such as
`wate`, `wagr`, and `wamu`.

### `myth2_proj_dump`
```
myth2_proj_dump <file> list
myth2_proj_dump <file> <id>
```

Dumps a Myth II `proj` tag with an emphasis on bounce/collision-relevant
fields such as inertia, detonation/media-detonation frequencies, rebound type,
and projectile flags.

### `myth2_assemble`
```
myth2_assemble <folder> [output] [--edit] [--obj <input.obj>] [--water-obj <input.obj>] [--heightscale <n>] [--water] [--water-flags] [--mask]
```

Rebuilds a Myth II `dng2` plugin from an extracted map folder. The first pass
packs the mesh plus any extracted terrain, name, and screen tags.

- `--edit` reapplies edited assets from the extracted folder before packing.
- `--obj` imports Myth II terrain displacement from an OBJ into `raw/mesh_tag.bin`.
- If `--obj` is omitted, `myth2_assemble --edit` auto-detects `<folder>/displacement.obj` when present.
- `--water-obj` imports Myth II water-surface heights from an OBJ into wet cells' `media_height`.
- If `--water-obj` is omitted, `myth2_assemble --edit` auto-detects `<folder>/<mesh_tag>_water.obj` when present.
- During `--edit`, `terrain/water.bmp` is safely reapplied by default as flags/types only.
- `--water` experimentally imports `terrain/water.bmp` with media-height changes as well.
- `--water-flags` explicitly selects the same safe flags-only `terrain/water.bmp` path.
- `--mask` experimentally imports `terrain/animation.bmp` back into the mesh during `--edit`.
- The assembler now prefers the primary edit paths first and uses `layers/` only as fallback:
  - `terrain/terrain.bmp`
  - `terrain/passability.bmp`
  - `terrain/water.bmp`
  - `terrain/animation.bmp`
  - `screens/overhead.bmp`
  - `screens/pregame.bmp`
  - `screens/postgame.bmp`
  - `strings/name.txt`
- `terrain/terrain.bmp` is reinjected into the terrain `.256` when present.
- `terrain/passability.bmp` is converted back into Myth II terrain-type flags.
- `terrain/water.bmp` is safely reinserted in flags-only form during `--edit`.
- `terrain/reflection.bmp` is reinserted into the mesh reflection bit during `--edit`.
- `terrain/reflection.bmp` marks reflective cells from the mesh: black = none, orange = reflective.
- `terrain/animation.bmp` marks cells with the animated-media bit: black = none, white = animated.
- In normal engine behavior, the animated-media bit is derived from wet topology: a vertex is marked animated only when the four cells meeting there are all fully wet.
- Because the engine recomputes that bit from topology, `terrain/animation.bmp` is best treated as a diagnostic/reference layer rather than a stable standalone authoring control.
- `screens/pregame.bmp`, `screens/overhead.bmp`, and `screens/postgame.bmp` are reinjected into their `.256` tags when present.
- `strings/name.txt` is rebuilt into the map-name `stli` when present.

---

## Examples

**Training map** (5x5 tiles, tag `00tm`, lives in `artsound.gor`):
```
myth_extract artsound.gor 00tm 5 5 training_map.bmp
```

**A level texture** — find the tag name from `docs/tags.txt` under the `.256` section, and the mesh dimensions from `docs/Maps.txt` or from reading the level's `mesh` tag:
```
myth_extract artsound.gor 03di 5 5 diversion.bmp
```

---

## How it works

Myth stores all game data in `.gor` archive files (`artsound.gor`, `tags.gor`).  
Texture maps live in `.256` tags inside these files.

Each `.256` tag has:
- A **320-byte header** (`_256Header`) describing the layout
- A **2080-byte palette** (256 RGB colors, each stored as `uint8_t r, _, g, _, b, _, flag, _`)
- A **section table** (128 bytes per entry) — one entry per tile in the mesh grid
- The **tile data** itself: `meshWidth * meshHeight` tiles, each 256×256 pixels, 8-bit indexed, preceded by a 52-byte tile header

The tiles are interleaved with shadow map tiles in the section table (even indices = color texture, the extractor skips odd ones). Each row of scanlines is also stored right-to-left on disk and reversed on read.

The output is a standard 8-bit indexed Windows BMP.

---

## Finding tag names and mesh sizes

- `docs/tags.txt` — full listing of all tags in `tags.gor` with offsets and lengths.  
  Look for the `.256` type entries to find texture tag names.
- `docs/Maps.txt` or the mesh tag for a level — gives `meshWidth` and `meshHeight`.
- The training map is always a good starting point: `00tm`, 5×5.

---

## Notes

- Myth data is **big-endian** (Mac PowerPC). All multi-byte integers in `.gor` files  
  are byte-swapped before use.
- The program scans the `.gor` file to locate the tag rather than using a hard-coded  
  offset table, so it works with any version of the game files.
- Output is always an 8-bit indexed BMP (same format Myth uses internally).
