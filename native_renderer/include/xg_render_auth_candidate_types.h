#ifndef XG_RENDER_AUTH_CANDIDATE_TYPES_H
#define XG_RENDER_AUTH_CANDIDATE_TYPES_H

#include "game_identity_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct PsxXgRenderAuthCandidate {
    uint32_t producer_entry;
    uint32_t range_start;
    uint32_t range_size;
    uint32_t dispatch_pc;
    PsxGameIdentity identity;
    uint64_t pair_id;
    uint32_t artifact_base;
    uint32_t artifact_size;
    uint32_t artifact_crc32;
    uint8_t runtime_variant_identity[PSX_GAME_IDENTITY_SHA256_BYTES];
    bool authority_provenance;
    bool pair_bound;
    bool runtime_variant_bound;
} PsxXgRenderAuthCandidate;

typedef enum PsxXgRenderCodeWriteClass {
    PSX_XG_RENDER_CODE_WRITE_DESCRIPTOR = 0,
    PSX_XG_RENDER_CODE_WRITE_DIRECT,
    PSX_XG_RENDER_CODE_WRITE_ZOOM,
    PSX_XG_RENDER_CODE_WRITE_PROJECTED,
    PSX_XG_RENDER_CODE_WRITE_WORLD_SKY,
    PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON,
    PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS,
    PSX_XG_RENDER_CODE_WRITE_MODEL_FT4,
    PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4,
    PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA,
    PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA,
    PSX_XG_RENDER_CODE_WRITE_ARTIFACT,
    PSX_XG_RENDER_CODE_WRITE_CLASS_COUNT,
} PsxXgRenderCodeWriteClass;

#endif
