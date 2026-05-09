# Myth II Sprite and .256 Bitmap Storage

Status: practical extractor notes from `export_models.cpp`, checked against
unit and scenery sprites on `le3e`.

All integer fields described here are big-endian.

## What `.256` Contains

Myth II stores both model textures and sprite frames in `.256` collection tags.
The exported PNGs are only our interchange format; the game data is palette
indexed bitmap data inside the `.256` tag.

The `.256` header is 320 bytes. Offsets below are from the start of the tag
data unless otherwise noted:

```
[4:8]     collection type
            256 = colormap / texture collection
             12 = sprite collection
[68:72]   color_tables_offset        (relative to bulk_offset)
[96:100]  bitmap_reference_count
[100:104] bitmap_references_offset   (relative to bulk_offset)
[112:116] bitmap_instance_count
[116:120] bitmap_instances_offset    (relative to bulk_offset)
[128:132] sequence_count
[132:136] sequence_references_offset (relative to bulk_offset)
[248:252] bulk_offset                (absolute from tag start)
```

Most tables referenced by the header live at:

```
absolute_table_offset = bulk_offset + relative_table_offset
```

## Palette

The palette/color table is at:

```
bulk_offset + color_tables_offset
```

The extractor skips a 32-byte color table header. Palette entries then follow
as 256 entries of 8 bytes each:

```
[0] red
[1] red fractional/unused
[2] green
[3] green fractional/unused
[4] blue
[5] blue fractional/unused
[6] flags
[7] flags fractional/unused
```

Palette index `0` is treated as transparent for sprite-style exports. The PNG
writer zeros transparent RGB to avoid color bleed.

## Sequence Lookup

Sprites and model textures are addressed through sequences. The lookup chain is:

```
sequence_reference
  -> sequence_data
    -> sequence_frame_data
      -> bitmap_instance_index for the requested view
        -> bitmap_instance_data
          -> bitmap_reference
            -> bitmap_data pixels
```

A sequence reference is 128 bytes:

```
[0:32]   sequence name
[64:68]  sequence_data_offset (relative to bulk_offset)
```

The sequence data block begins at `bulk_offset + sequence_data_offset`:

```
[8:10]   number_of_views
[10:12]  frames_per_view
```

The extractor currently uses the first `sequence_frame_data`, which begins
immediately after the 64-byte sequence data block. The bitmap instance index
array begins 46 bytes into that first frame block:

```
bitmap_instance_indexes = sequence_data_abs + 64 + 46
bitmap_instance_index[view] is int16
```

For model permutation textures, authored view selectors can exceed
`number_of_views`; those are normalized by modulo in the model permutation path.
For direct sprite extraction, the view index passed into the `.256` decoder must
already be within `0..number_of_views-1`.

## Bitmap Instance and Bitmap Reference

A bitmap instance record is 64 bytes:

```
bitmap_instance_abs = bulk_offset + bitmap_instances_offset + index * 64
[28:30] bitmap_reference_index (int16)
```

A bitmap reference record is 128 bytes:

```
bitmap_reference_abs = bulk_offset + bitmap_references_offset + index * 128
[64:68] image_data_offset  (relative to bulk_offset)
[68:72] image_data_length
[76:78] width              (int16)
[78:80] height             (int16)
```

The bitmap data begins at:

```
image_abs = bulk_offset + image_data_offset
```

## Bitmap Data and Row Pointers

Bitmap data begins with a 52-byte bitmap header. The image dimensions also
appear in nearby header fields, but the extractor trusts the bitmap reference
width/height.

The row pointer table begins at byte 48 of bitmap data. The first row pointer
is part of the 52-byte header; the remaining `height - 1` pointers immediately
follow the header:

```
row_pointer[0] at image_abs + 48
row_pointer[1] at image_abs + 52
...
row_pointer[h-1] at image_abs + 48 + (h-1) * 4
```

Because the first row pointer points past the row pointer table, the extractor
computes a virtual base:

```
first_row_offset = 52 + (height - 1) * 4
virtual_base = row_pointer[0] - first_row_offset
row_start = row_pointer[y] - virtual_base
row_end = row_pointer[y + 1] - virtual_base
```

For the last row, the synthetic end pointer is:

```
row_pointer[height] = virtual_base + image_data_length
```

If row-pointer decoding fails, the extractor falls back to treating bytes after
the 52-byte header as tightly packed raw indexes. That fallback is useful for
older/simple bitmap data, but sprite collections generally need row decoding.

## Row Encodings

Rows can be encoded in either of two forms.

Raw row:

```
row_length == width
row bytes are direct palette indexes
```

Span row:

```
[0:2] span_count       (int16)
[2:4] pixel_count      (int16)
[4:]  span table, span_count records of:
        [0:2] x_start  (int16, inclusive)
        [2:4] x_end    (int16, exclusive)
      then pixel_count palette indexes
```

The output row is initialized to palette index `0` (transparent). Spans copy
only their covered pixels into the row.

## Unit Sprite Resolution

Placed unit markers in the mesh (`marker type 3`) reference `unit` tags. The
current exporter resolves unit sprites roughly as:

```
unit tag -> monster tag (`mons`) -> sprite collection reference (`.256`)
```

It then chooses a static-looking sequence by scoring sequence names. Preferred
names include `stand_all`, `stand`, `idle`, `sit`, `glide`, and `flight`.
Action/damage names like `death`, `attack`, `run`, `walk`, `flinch`, and body
part fragments are penalized.

The selected sequence is exported as a set of view PNGs:

```
assets/sprites/textures/<unit_tag>_<collection_tag>_<sequence>_<view>.png
```

Unit billboards use the unit marker yaw to choose the closest exported view.
If a sprite has fewer views than expected, the exporter wraps/falls back within
the available views.

## Scenery Sprite Resolution

Placed sprite scenery markers in the mesh (`marker type 1`) reference `scen`
tags. The current exporter reads the scenery tag's collection reference and
sequence index, exports sequence view 0, then builds a crossed billboard in
`assets/sprites/scenery.obj`.

This is sufficient for fixed scenery like trees, bushes, signs, and posts.
Some apparent world details may be inferred by the engine rather than stored as
individual scenery markers. For example, fencing between fence posts on `le3e`
appears to be inferred from placed fence post markers rather than represented as
separate sprite scenery markers.

## Current Export Products

`export_models` writes:

```
assets/sprites/units.obj
assets/sprites/units.json
assets/sprites/scenery.obj
assets/sprites/scenery.json
assets/sprites/textures/*.png
```

The PNG files are RGBA exports from palette-indexed Myth data. They are not the
native Myth storage format.
