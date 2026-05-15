# Mesh Support Files

`extract_map` now exports the preserved parts of a Myth II `mesh_tag.bin` into explicit sidecar files under `mesh_support/`, and `build_mesh` prefers those files over `raw/mesh_tag.bin`.

This is the current bridge between a pure rebuild and the older "patch an extracted mesh blob" workflow.

## Exported files

For an extracted map folder like `out/le3e/`, these files are emitted:

- `mesh_support/header.bin`
  - Raw 1024-byte mesh header.
  - Used for preserved header fields we do not fully synthesize yet, including the connector/trailing descriptor block.

- `mesh_support/cell_grid.bin`
  - Raw cell grid (`cellW * cellH * 12` bytes).
  - Each cell contains:
    - physical height
    - packed normal word
    - flags
    - first object index
    - media height
    - render height

- `mesh_support/unit_types.bin`
  - Raw 32-byte marker palette / unit type records.

- `mesh_support/source_instances.bin`
  - Raw 64-byte marker instance records.

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

`raw/mesh_tag.bin` is now only a fallback when one or more support artifacts are missing.

## Remaining gap

This does **not** mean the mesh is fully rebuilt from semantic assets yet.

We still preserve several opaque structures byte-for-byte through these support files. The long-term direction is:

1. export the remaining important structure in documented form
2. replace raw preservation with semantic rebuilds
3. make `raw/mesh_tag.bin` unnecessary even as a fallback
