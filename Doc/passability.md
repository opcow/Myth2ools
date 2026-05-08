# Passability Map Color Legend

The passability byte in each grid point stores two 4-bit terrain-type indices packed into one byte:
- Low nibble = triangle A terrain type
- High nibble = triangle B terrain type

Values range from 0–15.

---

## extract_map.cpp palette (`TERRAIN_TYPE_COLORS`)

Used by the modern extractor to write `layers/02_passability.png`.
Colors are stored as RGB in the source.

| Index | RGB | Name |
|-------|-----|------|
| 0 | `rgb(0, 0, 255)` | media dwarf |
| 1 | `rgb(0, 0, 176)` | media human |
| 2 | `rgb(0, 0, 128)` | media giant |
| 3 | `rgb(0, 0, 64)` | media deep |
| 4 | `rgb(128, 0, 0)` | sloped |
| 5 | `rgb(255, 0, 0)` | steep |
| 6 | `rgb(0, 0, 0)` | grass |
| 7 | `rgb(255, 255, 0)` | desert |
| 8 | `rgb(32, 32, 32)` | rocky |
| 9 | `rgb(128, 64, 0)` | marsh |
| 10 | `rgb(160, 160, 160)` | snow |
| 11 | `rgb(32, 112, 32)` | forest |
| 12 | `rgb(255, 0, 255)` | loathing special |
| 13 | `rgb(0, 0, 0)` | unused |
| 14 | `rgb(0, 128, 0)` | walking impassable |
| 15 | `rgb(0, 255, 0)` | flying impassable |

Source: [extract_map.cpp:729-746](../myth2/extract_map.cpp#L729-L746)

---
