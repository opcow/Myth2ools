# Myth II Action Corpus Notes

This note complements [`action_scripts.md`](action_scripts.md). That file
describes the binary format; this one summarizes what we see in the extracted
solo-map corpus and what the common action verbs appear to mean in practice.

Corpus source:

- `out/all_actions/*/actions.json`
- generated summary: `out/all_actions/corpus_report.json`
- generator: [`tools/analyze_action_corpus.py`](../tools/analyze_action_corpus.py)

Current corpus size:

- 27 solo maps
- 9,098 total actions

## Big picture

The "language" is not a text script. It is a graph of action nodes:

- **container/group nodes** with no FourCC type, used for organization
- **trigger/effect nodes** with a FourCC verb like `tuni`, `move`, `ctrl`, or `anim`
- **cross-links** between nodes through action-ID parameters such as `link`,
  `acos`, `acof`, `acoa`, and `deac`

In practice the maps read like a visual mission-logic editor:

- containers organize related behavior
- trigger nodes test conditions
- effect nodes move units, play sounds, change visibility, run animations, or end the level
- action-ID links wire the flow together

## Most common action verbs in the solo corpus

| Type | Count | Maps | Practical reading |
|---|---:|---:|---|
| `<container>` | 2910 | 27 | Editor grouping/folder nodes |
| `tuni` | 1252 | 27 | Trigger on unit/monster counts or presence |
| `move` | 801 | 26 | Move a subject along waypoints |
| `ctrl` | 786 | 27 | Toggle control/visibility/targeting state |
| `atta` | 720 | 27 | Order attack behavior |
| `acli` | 615 | 26 | Activate/deactivate/inhibit other actions |
| `soun` | 321 | 27 | Play a sound |
| `geom` | 242 | 25 | Geometric trigger in polygon/radius/region |
| `squa` | 242 | 18 | Define a squad/group anchor/formation |
| `plmo` | 191 | 18 | Platoon movement behavior |
| `plat` | 180 | 18 | Platoon definition/setup |
| `gene` | 140 | 21 | General behavior/state toggles |
| `anim` | 80 | 16 | Run model animations |
| `endg` | 56 | 27 | End game / victory / defeat |
| `obmo` | 64 | 18 | Object/model movement or camera-like motion |

## Plain-English guide to the common verbs

### `<container>`

Not an executable opcode. These are editor grouping nodes with a human-readable
name and usually one or more `link` parameters pointing at child actions.

Examples:

- `LIGHT FORCES`
- `Escort party`
- `Start of tutorial`

These are the nearest thing to folders in Loathing's action tree.

### `tuni`

The workhorse condition node. In practice this means "trigger when some unit or
monster count/condition matches a threshold in an area or scope."

Typical parameters:

- `link`
- `___>`, `___<`, `___=`
- `poly`, `clpo`
- `acos`, `acof`

Examples:

- `one warrior at right location?`
- `warrior screwing around?`
- `Is the Escort party dead?`

Working interpretation:

- the comparator parameter sets the threshold test
- spatial parameters like `poly` scope the test
- success/failure action lists decide what happens next

### `move`

Direct movement order for a subject.

Typical parameters:

- `wayp`
- `link`
- `acos`
- `form`
- `fifa`

Examples:

- `one warrior paces`
- `move archer back`

Working interpretation:

- `wayp` is the route
- `link` points at the subject/group this applies to
- `fifa` appears to be final facing
- follow-up actions are usually in `acos`

### `ctrl`

State/visibility/control toggles. This appears to be the main "change how this
thing behaves or whether it can be seen/targeted" verb.

Typical parameters:

- `link`
- `visi`
- `invi`
- `cnba`
- `dnat`
- `igno`

Examples:

- `one warrior visible`
- `1 warrior target dummy visible`

Working interpretation:

- `visi` / `invi` are obvious visibility flags
- `cnba` matches "cannot be auto-targeted"
- the rest likely toggle other unit-control attributes

### `atta`

Attack behavior.

Typical parameters:

- `link`
- `_all`
- `acof`
- `poly`
- `near`

Examples:

- `attack chucky`
- `Wave 1 Deer Attack`

Working interpretation:

- usually an order to engage a target or class of targets
- may use area/nearby constraints

### `acli`

Action-list management. This looks like the routing/control verb for other
actions rather than the game world directly.

Typical parameters:

- `acti`
- `deac`
- `inhi`
- `rand`
- `acoa`
- `prer`

Examples:

- `Start of tutorial`
- `Camera movement drill`

Working interpretation:

- activate some actions
- deactivate others
- inhibit or gate a branch
- possibly choose a random branch with `rand`

### `soun`

Play a sound or narration cue.

Typical parameters:

- `soun`
- `subj`
- `over`
- `acos`

Examples:

- `"Welcome to Myth II."`
- `"The camera is your point of view."`

### `geom`

Geometric/region trigger.

Typical parameters:

- `poly`
- `radi`
- `clpo`
- `insi`
- `resu`
- `_map`

Examples:

- `satchel charge still there?`
- `Are World Knot big pieces gone?`

Working interpretation:

- test whether something is inside a polygon/radius
- or whether some map object/state still exists in a region

### `squa`

Squad definition/group anchor.

Typical parameters:

- `cent`
- `faci`
- `form`
- `link`

Examples:

- `Southwest ghol squad`
- `Southeast ghol squad`

Working interpretation:

- sets a center, facing, and formation for a grouped set of units

### `plat`

Platoon/group setup.

Typical parameters:

- `stat`
- `init`
- `cent`
- `faci`
- `alli`

Examples:

- `Southwest ghol platoon`
- `Southeast ghol platoon`

Working interpretation:

- define an initial group/platoon state before further orders are issued

### `plmo`

Platoon movement.

Typical parameters:

- `wayp`
- `wayr`
- `radi`
- `loop`
- `ntrp`
- `spac`

Examples:

- `Southwest ghol move`
- `Southeast ghol move`

Working interpretation:

- move a platoon/group along one or more routes
- with radius/loop/spacing-ish controls layered on top

### `gene`

General behavior/state toggles.

Typical parameters:

- `type`
- `link`
- `subj`
- `acos`

Examples:

- `5 heroes stop`
- `North cheerleaders cheer`

Working interpretation:

- a generic "change behavior mode" action for the linked subject

### `anim`

Model animation trigger.

Typical parameters:

- `subj`
- `stba`
- `stfo`
- `acoe`
- `acoa`

Examples:

- `Open the gates!`
- `Close the gates!`

This is the opcode behind the `le3e` gate behavior.

### `endg`

End-of-level outcome.

Typical parameters:

- `ligh`
- `dark`
- `fail`
- `noce`

Examples:

- `Tutorial over, you did it!`
- `Light Victory`

Working interpretation:

- level success/failure/victory presentation and exit behavior

## Less common but still interesting verbs

These deserve more attention later because they are concentrated but not rare:

| Type | Count | Maps | Loathing's display name |
|---|---:|---:|---|
| `obmo` | 64 | 18 | **Observer Movement** |
| `mung` | 63 | 6 | **Munger** |
| `legi` | 55 | 8 | **Legion (defensive)** |
| `mele` | 45 | 7 | **Melee** (for Thrall, Myrmidons, etc.) |
| `moef` | 38 | 5 | **Model Effect** |
| `snif` | 34 | 1 | (training-map-only; no SSR text — likely an AI sniffer/sensor verb) |
| `lpgr` | 34 | 4 | **Local Projectile Group Action** |
| `suic` | 33 | 5 | **Suicide** (for wights) |
| `girl` | 33 | 7 | **Harass** (for ghols) |
| `moma` | 27 | 4 | **Move Marker** |
| `part` | 25 | 4 | **Particle System Control** |

## Complete FourCC ↔ display-name catalog

Joined across the 27 solo maps in the corpus and the SSR Map Action Texts
(`reference_source/Solo_Script_Rebuild_final/Solo Script Rebuild/Extras/SSR Map Action Texts/`).
Each FourCC was paired with the human label Loathing prints in `[brackets]` for
that action type. The join is by action display-name; counts in parens are
agreement counts vs total occurrences.

| FourCC | Loathing label | Confidence |
|---|---|---|
| `acli` | Action List | 572/572 |
| `ambi` | Ambient Sound Control | 20/20 |
| `anim` | Model Animation | 72/72 |
| `atta` | Attack | 705/709 |
| `ctrl` | Unit Control | 740/740 |
| `dela` | Delay | (no name-match in corpus, but `dela` is the only unmapped FourCC matching `Delay` by spelling) |
| `endg` | Endgame Condition | 55/55 |
| `gene` | General Action | 134/134 |
| `geom` | Geometry Filter | 240/243 |
| `girl` | Harass (for ghols) | 33/33 |
| `lead` | Leading | 2/2 |
| `legi` | Legion (defensive) | 55/55 |
| `ligh` | Lightning | 8/8 |
| `lpgr` | Local Projectile Group Action | 27/27 |
| `mean` | Meander Action (wander aimlessly) | 15/15 |
| `mele` | Melee | 45/49 |
| `miss` | Mission | 4/4 |
| `moef` | Model Effect | 38/38 |
| `moma` | Move Marker | 27/28 |
| `move` | Movement | 790/790 |
| `mung` | Munger | 63/63 |
| `ngty` | Netgame Type | 2/2 |
| `obmo` | Observer Movement | 51/52 |
| `part` | Particle System Control | 25/25 |
| `pick` | Pick Up Object | 19/19 |
| `plat` | Platoon | 180/180 |
| `plmo` | Platoon Movement | 191/191 |
| `plsc` | Platoon Scouting | 3/3 |
| `rout` | Rout | 19/19 |
| `snif` | *(no SSR mapping; 34 occurrences in `00tm` only)* | — |
| `soun` | Sound Action | 206/206 |
| `squa` | Squad | 242/242 |
| `suic` | Suicide (for wights) | 32/32 |
| `surr` | Surround | 9/9 |
| `tuni` | Test Unit | 1190/1195 |
| `wand` | Wandering Movement | 2/2 |

**Notes on the SSR labels not paired with a FourCC above:**

- **Player Count** — appears 3 times in `24 battle 2`, but those action display names (`"1 Player S0L0"`, `"2 Players C0 0P"`, etc.) don't match any action in the extracted `24b2/actions.json` by name. Likely a Loathing UI subcategory that maps to one of the existing FourCCs based on a flag or parameter rather than its own opcode.
- **anxious / frustrated / exhausted / impressed** — each appears once on action names like `"22 (bowmen) Damn [anxious]"` in `22 dam`. Almost certainly Munger initial-state suffixes that Loathing displays as the action's "type" in the editor, not separate opcodes.

## Most common parameter names

These are the connective tissue of the graph:

| Parameter | Count | Practical reading |
|---|---:|---|
| `link` | 4456 | group membership / subject linkage / hierarchy |
| `acos` | 2354 | activate on success |
| `subj` | 1696 | subject |
| `wayp` | 1073 | waypoint list |
| `poly` | 758 | polygon region |
| `form` | 704 | formation |
| `___>` | 643 | comparator greater-than |
| `acof` | 623 | activate on failure |
| `clpo` | 604 | closed polygon-ish region flag |
| `acti` | 589 | activate |
| `faci` | 487 | facing |
| `cent` | 473 | center |

## What this suggests for authoring

The corpus supports a likely authoring model:

1. **Keep the binary serializer conservative.**
2. **Make common action families first-class in JSON/UI:**
   - `tuni`
   - `move`
   - `ctrl`
   - `atta`
   - `acli`
   - `geom`
   - `soun`
   - `anim`
   - `endg`
3. **Leave rarer opcodes in a lower-level escape hatch** until their semantics are
   understood well enough to expose cleanly.

That means the likely future editor is not "free-form script text," but a
structured action graph editor with typed forms for common verbs and a raw/hex
fallback for everything stranger.
