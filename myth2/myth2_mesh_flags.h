#pragma once

#include <stdint.h>

// Myth II / Fear+Loathing / Vengeance mesh-cell flag bits.
// These names follow the source-lineage symbols where we have them.
// Runtime behavior for some bits (especially bit 10) is still based partly on testing.

static constexpr uint16_t MYTH2_MESH_VERTEX_IS_MEDIA_FLAG                = (1u << 8);
static constexpr uint16_t MYTH2_MESH_CELL_WAS_OR_IS_ON_FIRE_FLAG         = (1u << 9);
static constexpr uint16_t MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG       = (1u << 10);
static constexpr uint16_t MYTH2_MESH_CELL_TRIANGLE0_IS_MEDIA_FLAG        = (1u << 11);
static constexpr uint16_t MYTH2_MESH_CELL_TRIANGLE1_IS_MEDIA_FLAG        = (1u << 12);
static constexpr uint16_t MYTH2_MESH_CELL_HAS_REFLECTION_FLAG            = (1u << 13);
static constexpr uint16_t MYTH2_MESH_CELL_IS_NOT_RENDERED_FLAG           = (1u << 14);
static constexpr uint16_t MYTH2_MESH_CELL_WILL_BE_MARKED_IMPASSABLE_FLAG = (1u << 15); // LOATHING editor-only
