# Myth II Action Scripts

Reverse-engineered format of the action/script data embedded in `mesh_tag.bin`. This is the trigger/event system Bungie's editor (Loathing) edits — every map's mission logic, victory conditions, scripted sequences, and AI behavior live here.

Decoder reference: [`myth2/export_map_actions.cpp`](../myth2/export_map_actions.cpp).

Corpus/practical companion: [`action_corpus.md`](action_corpus.md).

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

Verified against Loathing 1.8.4 — `FUN_00479220` and `FUN_0041dfa0` both contain a 21-case switch on the parameter type (cases 0..20). The descriptor table at `0x519030` (21 × 12-byte entries) confirms the labels.

| ID | Name | Encoding | Tag-group lookup | Scale |
|---|---|---|---|---|
| 0 | flag | 1 byte (0/1), padded to 4 | — | — |
| 1 | string | `count` bytes, padded to 4 | — | UTF-8/ASCII |
| 2 | monster_identifier | u16 × count, padded to 2 | — | raw monster ID |
| 3 | action_identifier | u16 × count, padded to 2 | — | raw action ID |
| 4 | angle | u16 × count, padded to 2 | — | ÷ (65536/360) → degrees |
| 5 | integer | s32 × count | — | — |
| 6 | world_distance | s32 × count | — | ÷ 512 → world units |
| 7 | field_name | FourCC × count (4 bytes/entry) | — | — |
| 8 | fixed | s32 × count | — | ÷ 65536 |
| 9 | projectile | FourCC × count (4 bytes/entry) | `prgr` (projectile group tag) | — |
| 10 | string_list | FourCC × count (4 bytes/entry) | `stli` | — |
| 11 | sound | FourCC × count (4 bytes/entry) | `soun` | — |
| 12 | projectile | FourCC × count (4 bytes/entry) | `proj` (distinct from type 9!) | — |
| 13 | world_point_2d | 2 × s32 per point (8 bytes/entry) | — | both ÷ 512 |
| 14 | world_rectangle_2d | 4 × s32 per rectangle (16 bytes/entry) | — | each ÷ 512 |
| 15 | object_identifier | u16 × count, padded to 2 | — | — |
| 16 | model_identifier | u16 × count, padded to 2 | — | — |
| 17 | sound_source_identifier | u16 × count, padded to 2 | — | — |
| 18 | world_point_3d | 3 × s32 per point (12 bytes/entry) | — | each ÷ 512 |
| 19 | local_projectile_group_identifier | u16 × count, padded to 2 | — | — |
| 20 | model_animation_identifier | u16 × count, padded to 2 | — | — |

**Note on types 9 and 12:** Both are labeled "projectile" in Loathing's UI (the descriptor strings at `0x4f56b4` are both `"projectile"`) but read different tag groups. Our JSON exporter writes both as `"type": "projectile"` and disambiguates via `"type_id": 9` vs `"type_id": 12`.

**Corrections to earlier versions of this table (now fixed in code):**

- **Type 10 (string_list):** Previously documented as `u16 × count (padded to 2)`. Loathing's formatter reads a `uint32` and looks it up in the `stli` tag group — so the encoding is `FourCC × count`, same shape as types 7/9/11/12.
- **Type 14 (world_rectangle_2d):** Previously documented as `u16 × count (padded)`. Loathing's case 0xe in `FUN_00479220` reads four 32-bit world distances per rectangle (printed as `(%3.3f,%3.3f,%3.3f,%3.3f)`) and advances by 16 bytes per iteration.

Both corrections are reflected in `myth2/export_map_actions.cpp` (decoder) and `myth2/build_plugin.cpp::actionParameterValueBytes` / `appendActionParamValues` (encoder). Neither type appears in any extracted Bungie map in our corpus, so the prior bugs never surfaced — but anyone authoring an action with these types would have hit them.

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

1. ~~**Flags bits 5–31** — not documented in the extractor, but the corpus uses values up to ~`0x3F` so there are likely 1–2 more flags.~~ **Resolved.** Loathing 1.8.4's flag parser (`FUN_0041d880`) recognizes only the 5 documented names — no fallback for unknown flag names, and the 5 globals at `0x5257cc..dc` are the only flag bits the editor manipulates. If the corpus has flag values beyond `0x1F`, those bits are either game-engine-only or unused.
2. **The semantic meaning of every action type FourCC** — Loathing does NOT have a per-type dispatch table; action types are treated opaquely (just FourCC labels in the UI). For 35 of the 36 FourCCs found in the solo-map corpus we now have Loathing's display names — see [`action_corpus.md`](action_corpus.md). The remaining `snif` (training-map-only) and the precise PARAMETER semantics (which params each verb requires, what they do at runtime) still live in `Myth II.exe`, not the editor — best path forward there is Ghidra on `Myth II.exe`.
3. ~~**Whether parameter blocks have any header beyond what's known**~~ **Resolved.** The per-type encoding is the 21-case switch verified above; no hidden header bytes.

## Verified via Ghidra of Loathing 1.8.4

Sources: `..\loathing\map_action_parser.c`, `..\loathing\map_actions_export.c`, `..\Myth 2\map_actions\map_actions.c`, `..\Myth 2\map_actions\map_action_ui.c` (46 functions touched).

Key functions:
- `FUN_00419220` (parser entry) and `FUN_00479220` (display formatter) — both contain the 21-case parameter-type switch.
- `FUN_0048d7e0(group_tag, sub_tag)` — the tag-table lookup used by types 9/10/11/12. Walks a tag-entry array at `*(0x5bfba0 + 0x18)` looking for matching `group_tag` and `sub_tag`. Returns `(entry_short0 << 16) | index` or `0xFFFFFFFF` if not found.
- `FUN_0041d880` — flag-name parser; recognizes only the 5 documented flag strings.

Parameter-type descriptor table at `0x519030` (21 × 12-byte entries) contains, per type: a `short0` "kind" code (0–8 or `0xFFFF`), a pointer to a sub-table (used for types 3/7/9/10/11/12), and a pointer to the human-readable type name string.

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
