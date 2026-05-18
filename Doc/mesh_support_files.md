# Mesh Support Files

`extract_map` now exports the preserved parts of a Myth II `mesh_tag.bin` into explicit sidecar files under `mesh_support/`, and `build_mesh` prefers those files over `raw/mesh_tag.bin`.

This is the current bridge between a pure rebuild and the older "patch an extracted mesh blob" workflow.

## Exported files

For an extracted map folder like `out/le3e/`, these files are emitted:

- `mesh_support/header.bin`
  - Debug-only raw 1024-byte mesh header.
  - No longer required for normal rebuilds.

- `mesh_support/cell_grid.bin`
  - Debug-only raw cell grid (`cellW * cellH * 12` bytes).
  - No longer required for normal rebuilds.

- `mesh_support/cell_grid.json`
  - Readable export of the same cell grid.
  - Each cell includes decoded fields plus `raw_hex` for exact roundtrip safety.

- `mesh_support/unit_types.bin`
  - Raw 32-byte marker palette / unit type records.

- `mesh_support/unit_types.json`
  - Readable export of the same unit type records.
  - Includes decoded fields plus `raw_hex` for exact roundtrip safety.

- `mesh_support/source_instances.bin`
  - Raw 64-byte marker instance records.

- `mesh_support/source_instances.json`
  - Readable export of the same marker instance records.
  - Includes decoded placement/orientation fields plus `raw_hex` for exact roundtrip safety.

- `mesh_support/post_action_tail.bin`
  - Bytes between the end of the action buffer and `1024 + data_size`.
  - Includes cached/runtime-ish sections that Myth II still expects laid out consistently.

- `mesh_support/post_data_appendix.bin`
  - Bytes after `1024 + data_size`.
  - Treated as appendix/editor tail data and preserved byte-for-byte for now.

## Metadata file

`mesh_metadata.json` records the sizes and offsets needed to validate and place the support files:

- `header_size`
- `mesh_offset`
- `mesh_size`
- `unit_type_count`
- `unit_type_offset`
- `unit_type_size`
- `marker_count`
- `marker_offset`
- `marker_size`
- `action_count`
- `action_offset`
- `action_size`
- `media_coverage_offset`
- `media_coverage_size`
- `mesh_lod_offset`
- `mesh_lod_size`
- `trailing_offset_a`
- `trailing_size_a`
- `trailing_offset_b`
- `trailing_size_b`
- `connector_trailing_descriptor_raw_hex`
- `post_action_tail_size`
- `post_data_appendix_size`

## Current build_mesh behavior

When these files are present and size-valid, `build_mesh` uses them as the source of truth for:

- header preservation
- cell grid preservation
- unit type records
- instance templates
- post-action preserved sections
- appendix bytes

The mesh header is now synthesized primarily from `manifest.json` and
`mesh_metadata.json`. The remaining opaque header bytes we still preserve are
carried through `connector_trailing_descriptor_raw_hex` instead of requiring
`mesh_support/header.bin`.

When both are present, `build_mesh` prefers:

- `cell_grid.json` over `cell_grid.bin`
- `unit_types.json` over `unit_types.bin`
- `source_instances.json` over `source_instances.bin`

`raw/mesh_tag.bin` is now only a fallback when one or more support artifacts are missing.

## `--no-blob` build mode

`build_mesh --no-blob` is the transitional mode that gets us closest to a fully semantic rebuild today:

- **Dropped:** editor appendix (past `data_size`), `trailing_a` section. Both confirmed safe by [Doc/mesh_post_action_sections.md](mesh_post_action_sections.md).
- **Regenerated from semantic data:** the 0x120 fence connector payload is rebuilt from `assets/terrain/fences.json`. Each 64-byte record gets its post identifiers written as uint16 BE plus the post count at byte 63; the unknown bytes 48-62 are zeroed.
- **Preserved from source (transitional):** `media_coverage_region` (~12 KB) and `mesh_LOD_data` (18432 B for le3e). Empirical 1.8.5 testing showed dropping either breaks the engine: LOD drop → black/transparent terrain, MC drop (with LOD also dropped) → crash in `drop_model_for_current_frame`. The Vengeance source / Myth II decomp claim that these are engine-regenerated does NOT hold in gameplay mode.

In `--no-blob` mode, `post_data_appendix.bin` is no longer needed. `post_action_tail.bin` is still needed (as the source of MC and LOD bytes) until we decode those formats.

## Remaining gap

This does **not** mean the mesh is fully rebuilt from semantic assets yet. The `--no-blob` mode currently still depends on `post_action_tail.bin` for MC and LOD bytes.

The long-term direction is:

1. export the remaining important structure in documented form (MC, LOD)
2. replace raw preservation with semantic rebuilds
3. make `raw/mesh_tag.bin` unnecessary even as a fallback
