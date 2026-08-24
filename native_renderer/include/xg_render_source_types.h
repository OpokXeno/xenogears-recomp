#ifndef XG_RENDER_SOURCE_TYPES_H
#define XG_RENDER_SOURCE_TYPES_H

#include "xg_field_character_source_types.h"
#include "xg_host_3d_types.h"
#include "xg_render_auth_types.h"

#include <stdbool.h>
#include <stdint.h>

#define PSX_XG_RENDER_SOURCE_EVENT_CAPACITY 28u
#define PSX_XG_RENDER_FT4_GEOMETRY_CAPACITY 256u

typedef enum PsxXgRenderSourceOperation {
    PSX_XG_RENDER_SOURCE_OPERATION_READ = 0,
    PSX_XG_RENDER_SOURCE_OPERATION_WRITE = 1,
    PSX_XG_RENDER_SOURCE_OPERATION_SWC2 = 2,
    PSX_XG_RENDER_SOURCE_OPERATION_CALL = 3,
    PSX_XG_RENDER_SOURCE_OPERATION_BUCKET = 4,
} PsxXgRenderSourceOperation;

typedef enum PsxXgRenderSourceAuxiliaryRule {
    PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS = 0,
    PSX_XG_RENDER_SOURCE_AUXILIARY_NONE = 1,
    PSX_XG_RENDER_SOURCE_AUXILIARY_RESULT_REGISTER = 2,
} PsxXgRenderSourceAuxiliaryRule;

typedef enum PsxXgRenderSourceStage {
    PSX_XG_RENDER_SOURCE_STAGE_PRE = 0,
    PSX_XG_RENDER_SOURCE_STAGE_COMMIT = 1,
} PsxXgRenderSourceStage;

typedef struct PsxXgRenderSourceSiteMetadata {
    PsxXgRenderSourceOperation operation;
    PsxXgRenderSourceAuxiliaryRule auxiliary_rule;
    uint8_t width;
} PsxXgRenderSourceSiteMetadata;

typedef struct PsxXgRenderSourceEvent {
    uint64_t sequence;
    uint32_t pc;
    uint32_t auxiliary;
    PsxXgRenderSourceOperation operation;
    PsxXgRenderSourceStage stage;
    uint8_t width;
} PsxXgRenderSourceEvent;

typedef struct PsxXgRenderSourceSnapshot {
    PsxXgRenderSourceEvent events[PSX_XG_RENDER_SOURCE_EVENT_CAPACITY];
    uint64_t next_sequence;
    uint32_t count;
    uint32_t blocker;
    uint32_t context_bits;
    bool blocked;
    bool overflowed;
} PsxXgRenderSourceSnapshot;

typedef struct PsxXgRenderFt4Geometry {
    uint64_t sequence;
    uint32_t packet_guest_address;
    uint32_t semantic_template;
    int16_t x[4];
    int16_t y[4];
    XgHost3dRotAverage4Input pre_transform;
    uint16_t ordering_depth;
    int16_t depth_cue;
    uint32_t projection_flags;
    XgRenderAuthTier tier;
    XgFieldCharacterSourceSnapshot source_snapshot;
    uint32_t source_capture_result;
    bool source_captured;
    bool host_transformed;
} PsxXgRenderFt4Geometry;

enum {
    PSX_XG_RENDER_FT4_SEMANTIC_STATIC_TABLE = 0u,
    PSX_XG_RENDER_FT4_SEMANTIC_DYNAMIC_ACTOR = 1u,
};

#endif
