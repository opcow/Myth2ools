# Overhead Tag Fix — `buildCollection256FromFolder`

The overhead `.256` tag rebuilt via `buildCollection256FromFolder` was missing
several fields that the game engine requires, causing a crash-to-menu when
loading the overhead map. The original (extracted) tag had these fields;
the collection builder left them zero.

## Changes in `build_plugin.cpp`

### Bitmap Instance (`ins + 0`, `ins + 6`)
- `writeBE32To(ins + 0, 16)` — struct type/ID marker
- `writeBE16To(ins + 6, 0xFFFFu)` — integrity sentinel

### Bitmap Header Flags (`img + 6`)
- `writeBE16To(img + 6, 0x0002u)` — undocumented flag bit 1

### Sequence Data (`sd + 18..51`)
- `sd + 18..22`: runtime sound indices set to `0xFFFF` (no sound)
- `sd + 24..34`: sound tags set to `0xFFFF` (no tag)
- `sd + 36`: transfer mode set to `4`
- `sd + 40`: radius set to `0xFFFFFFFF` (sentinel -1)

### AUX Section (`aux + 12`)
- `writeBE16To(aux + 12, 0xFFFFu)` — sentinel matching original tag

### Instance High-Res Index (`ins + 30`)
- Changed from `0xFFFF` to `0` to match original (was `highres_bitmap_index = -1`)

## Symptom

Game crashed to menu when selecting a map whose overhead `.256` tag was
rebuilt via the collection builder. All edit modes (`--edit over`,
`--edit screens`, bare `--edit`) were affected.
