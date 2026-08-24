#ifndef XG_WORLD_ENTITY_SHADOWS_SNAPSHOT_TYPES_H
#define XG_WORLD_ENTITY_SHADOWS_SNAPSHOT_TYPES_H

#include "xg_host_3d_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XgWorldEntityShadowsShadowBlocker {
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NONE = 0,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_ENTRY_CONTEXT,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NESTED_BEGIN,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_STALE_GENERATION,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_SOURCE_CAPTURE,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NATIVE_BUILD,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_OUTPUT_RANGE,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_FINISH_CONTEXT,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_COUNTER_SATURATED,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_LIFECYCLE_INVALIDATED,
    XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_LIFECYCLE_BLOCKED,
} XgWorldEntityShadowsShadowBlocker;

typedef struct XgWorldEntityShadowsShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t generation;
    uint64_t begin_count;
    uint64_t completion_count;
    uint64_t invocation_match_count;
    uint64_t invocation_mismatch_count;
    uint64_t source_capture_failure_count;
    uint64_t native_build_failure_count;
    uint64_t source_read_count;
    uint64_t source_read_bytes;
    uint64_t candidate_count;
    uint64_t accepted_packet_count;
    uint64_t rtpt_cull_count;
    uint64_t minimum_depth_cull_count;
    uint64_t packet_match_count;
    uint64_t packet_mismatch_count;
    uint64_t partial_write_match_count;
    uint64_t partial_write_mismatch_count;
    uint64_t pending_count_mismatch_count;
    uint64_t pending_list_mismatch_count;
    uint64_t cursor_mismatch_count;
    uint64_t transform_observation_count;
    uint64_t transform_mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t ot_mismatch_count;
    uint32_t last_candidate_count;
    uint32_t last_accepted_packet_count;
    uint32_t last_mismatch_bits;
    uint32_t first_mismatch_address;
    uint32_t first_mismatch_source_index;
    uint32_t first_expected_word;
    uint32_t first_actual_word;
    XgHost3dMatrix expected_transform;
    XgHost3dMatrix actual_transform;
    XgHost3dMatrix expected_local_transform;
    XgHost3dMatrix actual_local_transform;
    int32_t expected_position[3];
    int32_t actual_position[3];
    uint32_t diagnostic_terrain_chunk;
    uint32_t diagnostic_terrain_cell;
    int16_t diagnostic_pending_x;
    int16_t diagnostic_pending_z;
    int16_t diagnostic_type;
    uint8_t diagnostic_heights[5];
    uint32_t last_source_capture_result;
    uint32_t last_native_build_result;
    uint32_t expected_finish_stack_pointer;
    uint32_t actual_finish_stack_pointer;
    uint32_t expected_saved_return;
    uint32_t actual_saved_return;
    XgWorldEntityShadowsShadowBlocker blocker;
    bool pending;
    bool blocked;
    bool transform_diagnostic_seen;
} XgWorldEntityShadowsShadowSnapshot;

#endif
