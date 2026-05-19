
### Quick compile (MSVC)

```bash
cl /EHsc /O2 tfl\myth_extract.cpp /Fe:myth_extract.exe
cl /EHsc /O2 tfl\myth_assemble.cpp /Fe:myth_assemble.exe
cl /EHsc /O2 tfl\myth_mesh.cpp /Fe:myth_mesh.exe
cl /EHsc /O2 tfl\myth_mesh_import.cpp /Fe:myth_mesh_import.exe
```
### Quick compile (GCC / Clang)

```bash
g++ -std=c++17 -O2 tfl/myth_extract.cpp -o myth_extract
g++ -std=c++17 -O2 tfl/myth_assemble.cpp -o myth_assemble
g++ -std=c++17 -O2 tfl/myth_mesh.cpp -o myth_mesh
g++ -std=c++17 -O2 tfl/myth_mesh_import.cpp -o myth_mesh_import
```
### `myth_extract`

```bash
myth_extract [--overwrite] <tags.gor> <meshtag>
```

| Argument | Description |
|----------|-------------|
| `--overwrite` | Overwrite existing extracted files instead of skipping them |
| `tags.gor` | Path to `tags.gor` for the map set you want to extract |
| `meshtag` | 4-character mesh tag name (for example `sega`, `balo`, `00tm`) |

### `myth_assemble`

```bash
myth_assemble <folder> [output.gor] [--edit] [--obj <input.obj>] [--heightscale <n>]
```

- `--edit` reapplies editable assets from the extracted folder before rebuilding.
- `--obj` imports terrain heights from a Wavefront OBJ and recomputes slope bytes.
- If `--edit` is used and `--obj` is omitted, the assembler auto-detects `<folder>/<tag>.obj`.
- When OBJ import is used, `height.bmp` is skipped, but `passability.bmp`, `water.bmp`, `animation.bmp`, and `color.bmp` still apply.

### `myth_mesh`

```bash
myth_mesh <tags.gor> <meshtag> [output.obj] [heightscale]
```

Exports the terrain grid as a Wavefront OBJ for editing in Blender.

### `myth_mesh_import`

```bash
myth_mesh_import <tag_folder> <input.obj> [heightscale]
```

Standalone OBJ importer for patching `raw/mesh_tag.bin` directly.

---

## Examples

**Training map** (5x5 tiles, tag `00tm`, lives in `artsound.gor`):

```bash
myth_extract artsound.gor 00tm 5 5 training_map.bmp
```

**A level texture** — find the tag name from `docs/tags.txt` under the `.256` section, and the mesh dimensions from `docs/Maps.txt` or from reading the level's `mesh` tag:

```bash
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

The extractor writes a standard 8-bit indexed Windows BMP as an editing
container. Myth's internal texture data is palette-indexed pixel data, not BMP.

---

## Finding tag names and mesh sizes

- `docs/tags.txt` — full listing of all tags in `tags.gor` with offsets and lengths.  
  Look for the `.256` type entries to find texture tag names.
- `docs/Maps.txt` or the mesh tag for a level — gives `meshWidth` and `meshHeight`.
- The training map is always a good starting point: `00tm`, 5×5.

---

## Notes

- Myth and Myth II data are **big-endian** (Mac PowerPC lineage). All multi-byte
  integers in tag/archive data are byte-swapped before use.
- The program scans the `.gor` file to locate the tag rather than using a hard-coded  
  offset table, so it works with any version of the game files.
- Image outputs use standard BMP files as editable containers. The games store
  palette-indexed image data internally.
