# Myth II Action Scripts

Reverse-engineered format of the action/script data embedded in `mesh_tag.bin`. This is the trigger/event system Bungie's editor (Loathing) edits — every map's mission logic, victory conditions, scripted sequences, and AI behavior live here.

Decoder reference: [`myth2/export_map_actions.cpp`](../myth2/export_map_actions.cpp).

## Storage layout

Map actions live inside `mesh_tag.bin`. Three 32-bit big-endian fields at the start of the mesh header point at the action buffer:

| Mesh offset | Field | Meaning |
|---|---|---|
| `128` | `action_count` | Number of actions |
| `132` | `action_buffer_offset` | Offset from end of 1024-byte mesh header |
| `136` | `action_buffer_size` | Total buffer size in bytes |

The action buffer itself is laid out as **two contiguous regions**:

```
┌──────────────────────────────────────┐
│  N × 64-byte action header records   │  ← fixed-size table
├──────────────────────────────────────┤
│  Variable-length parameter blobs     │  ← each action's params
│  referenced by header.offset/size    │
└──────────────────────────────────────┘
```

## Action header (64 bytes, big-endian)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | `id` (e.g. 35551) |
| 2 | u16 | `expiration_mode` (0=trigger, 1=execution, 2=successful_execution, 3=never, 4=failed_execution) |
| 4 | 4 chars | `type` FourCC (`tuni`, `move`, `anim`, `ctrl`, `gene`, `geom`, `atta`, `acli`, `squa`, etc.) — empty/0xFFFFFFFF means "container/group" with no behavior |
| 8 | u32 | `flags` bitfield |
| 12 | u32 | `trigger_time_lower` (ticks; ÷30 = seconds) |
| 16 | u32 | `trigger_time_delta` |
| 20 | u16 | `num_params` |
| 22 | u16 | `param_data_size` |
| 24 | u32 | `param_data_offset` (relative to end of header table) |
| 28 | u16 | `indent` (UI nesting level — Loathing displays this as tree depth) |

Flag bits 0–4: `initially_active`, `activates_only_once`, `no_initial_delay`, `only_initial_delay`, `deleted_on_deactivation`.

## Parameter block

Each parameter starts with an 8-byte header:

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | `type` (see table below) |
| 2 | u16 | `count` (number of values) |
| 4 | 4 chars | `name` FourCC |

Then `count` values follow in a type-dependent encoding, padded to alignment. Parameter blocks for different actions can **share storage** (multiple headers pointing to the same offset is common — that's why offsets repeat in the JSON).

## Parameter type table (the 21 known types)

| ID | Name | Encoding | Scale |
|---|---|---|---|
| 0 | flag | 1 byte (0/1), padded to 4 | — |
| 1 | string | `count` bytes, padded to 4 | UTF-8/ASCII |
| 2 | monster_identifier | u16 × count | raw monster ID |
| 3 | action_identifier | u16 × count | raw action ID |
| 4 | angle | u16 × count | ÷ (65536/360) → degrees |
| 5 | integer | s32 × count | — |
| 6 | world_distance | u32 × count | ÷ 512 → world units |
| 7 | field_name | 4-char FourCC × count | — |
| 8 | fixed | s32 × count | ÷ 65536 |
| 9, 12 | projectile | FourCC × count | — |
| 10 | string_list | u16 × count (padded to 2) | — |
| 11 | sound | FourCC × count | — |
| 13 | world_point_2d | u32 x, u32 y per point | both ÷ 512 |
| 14 | world_rectangle_2d | u16 × count (padded) | — |
| 15 | object_identifier | u16 × count | — |
| 16 | model_identifier | u16 × count | — |
| 17 | sound_source_identifier | u16 × count | — |
| 18 | world_point_3d | 3 × u32 per point | each ÷ 512 |
| 19 | local_projectile_group_identifier | u16 × count | — |
| 20 | model_animation_identifier | u16 × count | — |

## Special parameter name: `name`

Every action has a `name` string parameter that holds the human-readable label. The exporter intentionally hides it from the dumped parameter list because it's stored separately as the action's display name.

## Action types (FourCC) — the verbs

These are the script "opcodes." From the corpus, the most common are:

- **`tuni`** — *trigger unit count* — checks unit/monster counts; uses comparators `___>`, `___<`, `___=` and an integer threshold
- **`move`** — *movement* — `wayp` waypoints (world_point_2d list), `subj` who moves
- **`atta`** — *attack* — combat targeting
- **`anim`** — *animation* — `mode` (model_identifier), `subj` (model_animation), `stba`/`stfo` (start backward/forward flags)
- **`ctrl`** — *control* — sets unit attributes (e.g., `cnba` = "cannot be auto-targeted")
- **`gene`** — *general* — global behavior toggles like "don't move"
- **`geom`** — *geometric trigger* — fires when units enter `poly` (polygon) or `radi` (radius)
- **`acli`** — *action list* — manages other actions
- **`squa`** — *squad* — formation/grouping
- **`plat`/`plmo`/`plsc`** — *place* (units/monsters/scenery)
- **`endg`** — *end game* — victory/defeat
- **`soun`** — *play sound*
- **`mung`/`suic`/`mele`/`legi`** — various unit-state changes

## Parameter names (FourCC) — the operands

These describe what role each value plays inside an action:

| Name | Role |
|---|---|
| `link` | "this action belongs to / is grouped under" — cross-reference |
| `subj` | subject of the action (who) |
| `obje` | object/target |
| `acos` / `acof` | actions to activate-on-success / activate-on-failure |
| `acoa` / `acoe` / `acod` | activate-on-activation / -on-execution / -on-deactivation |
| `deac` / `deoa` / `deos` | deactivate (variants) |
| `inhi` | inhibit |
| `prer` | prerequisites |
| `wayp` | waypoint list |
| `poly` / `radi` | polygon / radius for geom triggers |
| `cent` / `dest` | center / destination |
| `faci` / `fifa` | facing / final facing angle |
| `___>` / `___<` / `___=` | numeric comparators (greater/less/equal) |
| `clpo` / `cnba` / `visi` / `invi` | flag toggles (closed-poly, can't-be-attacked, visible, invisible) |
| `init` | initial value |
| `<vit` | vitality threshold |

## How execution probably works (inferred)

Each action is a node with:

- a **trigger condition** (the `type` opcode evaluates whether to fire — `tuni` checks counts, `geom` checks positions, etc.)
- a **payload** (`acos`/`acof`/`acoa` lists the action IDs that get activated when this one fires successfully or fails)
- a **lifecycle** (`expiration_mode` controls whether it runs once, repeats forever, etc.)
- **flags** like `initially_active` decide whether it's armed at map start

Actions form a **DAG** wired by ID references in `link`/`acos`/`acof`/`subj`/`obje` parameters. Container actions (empty FourCC type) just group leaves under a name for organization in the editor — they have a `link` list pointing at children.

## What's still unknown

1. **Flags bits 5–31** — not documented in the extractor, but the corpus uses values up to ~`0x3F` so there are likely 1–2 more flags.
2. **The semantic meaning of every type/4cc** — there are ~100 distinct action types. The ones above are the common ones; rarer types (`mung`, `legi`, `girl`, `moef`, `snif`, `wand`, `pick`, `lpgr`, `obmo`, `moma`, `rout`, `part`) are best understood by reading Loathing's UI labels (Bungie's mapping editor).
3. **Whether parameter blocks have any header beyond what's known** — the extractor handles a few alignment quirks (`PARAM_STRING` aligned to 4, default case aligned to 2) which suggests the format is consistent across all known data.

## Authoring (writing) actions

The format is fully decodable. To **author** new actions from scratch the inverse is straightforward:

1. Emit 64-byte action headers in a fixed table.
2. Pack parameter blobs immediately after, observing the per-type alignment rules.
3. Patch `action_count` / `action_buffer_offset` / `action_buffer_size` at mesh offsets 128/132/136.

The parameter alignment rules and big-endian encoding are the only fiddly bits — everything else is a straightforward serialization.

## Example

A simple "is the escort party dead?" trigger from `le3e` (the town-gates mission):

```
[35555] tuni.Is the Escort party dead? [initially_active expiry=successful_execution]
    - link action_identifier=[35552]    ← scoped to "Escort party" (action 35552)
    - ___< integer=[1]                  ← fires when count < 1
    - acos action_identifier=[35601]    ← on success, activate action 35601 (game-over)
```

In binary, this is one 64-byte header (id=35555, type='tuni', flags=1, expiration_mode=2, num_params=4, plus offsets) followed by a parameter blob containing four parameter records: `name` (string), `link` (action_identifier), `___<` (integer), `acos` (action_identifier).
