# Myth II Sound Storage

Status: practical extractor notes from `export_models.cpp`, cross-checked with
the Vengeance-era `soun.h`/`soun.cpp` reference source and tested on `le3e`
ambient/random sound markers.

All Myth tag fields described here are big-endian. Exported WAV files are
standard little-endian RIFF/WAVE PCM files.

## Placed Sound Markers

Placed sound sources are mesh object markers with marker type `5`. Their
palette entries resolve to `amso` tags:

```
marker type 5 -> type palette entry -> amso tag
```

For the ambient/random sounds tested on `le3e`, the `amso` subgroup tag also
matches the actual `soun` subgroup tag. Example placed `amso` tags:

```
amri  ambient river
amka  ambient katydids
ratr  random trees
rasq  random squirrels
rawi  random wind
```

The actual audio payloads are in `soun` tags, commonly in `international small
install`.

`export_models` currently uses the placed sound marker's tag as the `soun`
subgroup to extract audio permutations. It writes placeholder geometry plus a
JSON manifest:

```
assets/sounds/sounds.obj
assets/sounds/sounds.json
assets/sounds/wav/*.wav
```

## `soun` Tag Layout

The root sound definition is 64 bytes:

```
[0:4]    flags
[4:6]    loudness / priority
[6:8]    play_fraction
[8:10]   external_frequency_modifier
[10:12]  pitch_lower_bound
[12:14]  pitch_delta
[14:16]  volume_lower_bound
[16:18]  volume_delta
[18:20]  first_subtitle_within_string_list_index
[20:24]  sound_offset
[24:28]  sound_size
[28:32]  subtitle_string_list_tag
[32:34]  subtitle_string_list_index
[34:36]  unused
[36:40]  number_of_permutations
[40:44]  permutations_offset
[44:48]  permutations_size
[48:52]  runtime sound_permutations pointer, zero on disk
[52:54]  runtime reference_count
[54:56]  runtime permutations_played_flags
[56:60]  runtime local_tick_last_played
[60:64]  runtime sampled_sound_headers pointer, zero on disk
```

Sanity expectations from the reference source:

```
number_of_permutations <= 5
permutations_offset == 64
permutations_size == number_of_permutations * 32
sound_offset == 64 + number_of_permutations * 32
```

`sound_offset` points to the sampled sound header table, not directly to sample
bytes. Sample bytes begin after that header table:

```
sample_data_offset = sound_offset + number_of_permutations * 32
```

## Sound Permutation Records

Each permutation record is 32 bytes:

```
[0:2]   flags
[2:4]   skip_fraction
[4:6]   unused
[6:32]  name, 26-byte null-terminated string
```

Permutation names become part of the exported WAV filename when safe:

```
assets/sounds/wav/<tag>_<index>_<permutation_name>.wav
```

Example:

```
assets/sounds/wav/amri_0_river1.wav
assets/sounds/wav/amri_1_river2.wav
assets/sounds/wav/amri_2_river3.wav
```

## Sampled Sound Headers

Each sampled sound header is 32 bytes and begins at:

```
sound_offset + permutation_index * 32
```

Layout:

```
[0:4]    sample flags
           bit 0 = Apple IMA compressed
[4:6]    logical_bits_per_sample
[6:8]    physical_bytes_per_sample_minus_one
[8:10]   channels
[10:12]  pad
[12:16]  sample_rate as fixed 16.16
[16:20]  number_of_samples
[20:24]  loop_start
[24:28]  loop_end
[28:32]  runtime samples pointer, zero/ignored on disk
```

For the shipped Myth II sounds observed so far:

```
logical_bits_per_sample = 16
channels = 1 for tested ambient/random sounds
sample_rate = 22050 << 16
sample flags bit 0 set, meaning Apple IMA ADPCM
```

The field named `number_of_samples` is a packet count for Apple IMA compressed
sounds. Each mono packet expands to 64 PCM samples.

## Apple IMA Packet Format

Compressed sample data for a permutation is a sequence of 34-byte packets:

```
[0:2]   state (int16)
[2:34]  32 bytes of ADPCM nibbles
```

Each packet decodes to 64 signed 16-bit PCM samples. The state field stores:

```
low 7 bits       = IMA step index
upper bits       = predicted sample, masked with ~0x7f
```

The decoder uses the standard IMA step table and index table:

```
index delta table:
  -1,-1,-1,-1, 2,4,6,8, -1,-1,-1,-1, 2,4,6,8

step table:
  7, 8, 9, 10, 11, 12, ... , 32767  (89 entries)
```

Nibbles are read low nibble first, then high nibble. For each nibble:

```
diff = step >> 3
if code & 4: diff += step
if code & 2: diff += step >> 1
if code & 1: diff += step >> 2
if code & 8: diff = -diff

predicted_sample = clamp16(predicted_sample + diff)
index = clamp(index + index_delta[code], 0, 88)
```

The exporter writes decoded samples as little-endian 16-bit PCM WAV.

## Stereo Note

The reference source stores stereo Apple IMA as alternating left and right
packets:

```
left packet 0, right packet 0, left packet 1, right packet 1, ...
```

The current exporter supports this layout and interleaves decoded PCM as:

```
L0, R0, L1, R1, ...
```

Most Myth II ambient/random sounds seen during testing are mono.

## Uncompressed Note

The exporter also has a conservative path for uncompressed 16-bit samples:

```
stored_size = number_of_samples << physical_bytes_per_sample_minus_one
```

Those samples are interpreted as big-endian signed 16-bit PCM and written to
WAV. This path exists for completeness; the tested map sounds used Apple IMA.

## Blender Preview

`create_blend.py` creates Blender speaker objects from `assets/sounds/sounds.json`.
Speakers are muted by default so timeline playback does not trigger all map
sounds at once.

The generated `.blend` embeds a helper text block:

```
myth2_sound_tools.py
```

Blender may disable embedded scripts when opening the file. Allow script
execution or run that text block manually. After it is registered, select a
speaker and use the `Myth II` sidebar tab or the `Myth II: Play Selected Sound`
operator to preview only the selected source.

## Validated Example

On `le3e`, the placed sound marker export produced:

```
40 placed sound markers
35 unique WAV permutations
all 40 markers had at least one audio path
sample WAV: mono, 22050 Hz, 16-bit PCM
```
