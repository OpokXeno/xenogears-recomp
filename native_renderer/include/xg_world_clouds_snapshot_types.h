#ifndef XG_WORLD_CLOUDS_SNAPSHOT_TYPES_H
#define XG_WORLD_CLOUDS_SNAPSHOT_TYPES_H

#include <stdbool.h>
#include <stdint.h>

enum { XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT = 10 };

typedef enum XgWorldCloudLod {
    XG_WORLD_CLOUD_LOD_NEAR = 0,
    XG_WORLD_CLOUD_LOD_MIDDLE = 1,
    XG_WORLD_CLOUD_LOD_FAR = 2,
} XgWorldCloudLod;

typedef struct XgWorldCloudPosition {
    int32_t x;
    int32_t y;
    int32_t z;
} XgWorldCloudPosition;

typedef struct XgWorldCloudsBuildStats {
    uint32_t clouds_entered;
    uint32_t clouds_world_culled;
    uint32_t clouds_anchor_culled;
    uint32_t near_groups_culled;
    uint32_t quad_attempt_count;
    uint32_t quad_projection_culled;
    uint32_t quad_screen_culled;
    uint32_t far_preinsert_depth_stops;
    uint32_t far_postinsert_depth_stops;
    uint32_t first_world_accepted_source;
    uint32_t first_anchor_flags;
    int32_t first_anchor_translation_x;
    int32_t first_anchor_translation_y;
    int32_t first_anchor_translation_z;
    int32_t first_world_relative_x;
    int32_t first_world_relative_z;
    bool has_world_accepted_source;
    bool packet_entry_limit_stopped;
} XgWorldCloudsBuildStats;

typedef enum XgWorldCloudsShadowResult {
    XG_WORLD_CLOUDS_SHADOW_OK = 0,
    XG_WORLD_CLOUDS_SHADOW_MISMATCH,
    XG_WORLD_CLOUDS_SHADOW_INVALID_ARGUMENT,
    XG_WORLD_CLOUDS_SHADOW_BLOCKED,
} XgWorldCloudsShadowResult;

typedef enum XgWorldCloudsShadowPhase {
    XG_WORLD_CLOUDS_SHADOW_IDLE = 0,
    XG_WORLD_CLOUDS_SHADOW_PENDING,
    XG_WORLD_CLOUDS_SHADOW_PHASE_BLOCKED,
} XgWorldCloudsShadowPhase;

typedef enum XgWorldCloudsShadowBlocker {
    XG_WORLD_CLOUDS_SHADOW_BLOCK_NONE = 0,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_INVALID_ARGUMENT,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_NESTED_BEGIN,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_UNMATCHED_FINISH,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_CALLER_RETURN,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_CALLBACK_POINTER,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_STALE_GENERATION,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_SOURCE_CAPTURE,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_NATIVE_BUILD,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_OUTPUT_CONTEXT,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_FINISH_CONTEXT,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_LIFECYCLE_INVALIDATED,
    XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW,
} XgWorldCloudsShadowBlocker;

typedef struct XgWorldCloudsShadowPacketMismatch {
    uint32_t packet_address;
    uint32_t packet_index;
    uint32_t source_index;
    uint32_t lod_quad_index;
    uint16_t word_mismatch_mask;
    XgWorldCloudLod lod;
    uint32_t expected_words[XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT];
    uint32_t actual_words[XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT];
} XgWorldCloudsShadowPacketMismatch;

typedef struct XgWorldCloudsShadowOtMismatch {
    uint32_t address;
    uint32_t bucket;
    uint32_t expected_word;
    uint32_t actual_word;
} XgWorldCloudsShadowOtMismatch;

typedef struct XgWorldCloudsShadowPositionMismatch {
    uint32_t address;
    uint32_t position_index;
    uint8_t component_mask;
    XgWorldCloudPosition expected;
    XgWorldCloudPosition actual;
} XgWorldCloudsShadowPositionMismatch;

typedef struct XgWorldCloudsShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t begin_attempt_count;
    uint64_t begin_count;
    uint64_t completion_count;
    uint64_t invocation_match_count;
    uint64_t invocation_mismatch_count;
    uint64_t callback_rejection_count;
    uint64_t lifecycle_invalidation_count;
    uint64_t source_capture_failure_count;
    uint64_t native_build_failure_count;
    uint64_t source_read_count;
    uint64_t source_read_bytes;
    uint64_t candidate_count;
    uint64_t primitive_count;
    uint64_t packet_match_count;
    uint64_t packet_mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t unexpected_packet_write_count;
    uint64_t ot_mismatch_count;
    uint64_t cursor_mismatch_count;
    uint64_t position_mismatch_count;
    uint64_t scratch_emitted_mismatch_count;
    uint64_t scratch_attempt_mismatch_count;
    uint64_t anchor_observation_count;
    uint64_t anchor_mismatch_count;
    uint64_t active_generation;
    uint64_t last_generation;
    uint32_t last_callback_pointer;
    uint32_t last_entry_stack_pointer;
    uint32_t expected_finish_stack_pointer;
    uint32_t actual_finish_stack_pointer;
    uint32_t expected_saved_return;
    uint32_t actual_saved_return;
    uint32_t last_buffer_index;
    uint32_t last_packet_base;
    uint32_t last_ot_base;
    uint32_t last_position_base;
    uint32_t last_candidate_count;
    uint32_t last_primitive_count;
    uint32_t expected_final_cursor;
    uint32_t actual_final_cursor;
    uint32_t expected_scratch_emitted;
    uint32_t actual_scratch_emitted;
    uint32_t expected_scratch_attempts;
    uint32_t actual_scratch_attempts;
    uint32_t source_capture_result;
    uint32_t native_build_result;
    uint32_t diagnostic_anchor_source;
    uint32_t expected_anchor_flags;
    uint32_t actual_anchor_flags;
    int32_t expected_anchor_translation[3];
    int32_t actual_anchor_translation[3];
    XgWorldCloudsBuildStats last_build_stats;
    XgWorldCloudsShadowPacketMismatch first_packet_mismatch;
    XgWorldCloudsShadowOtMismatch first_ot_mismatch;
    XgWorldCloudsShadowPositionMismatch first_position_mismatch;
    XgWorldCloudsShadowBlocker blocker;
    XgWorldCloudsShadowPhase phase;
    bool has_first_packet_mismatch;
    bool has_first_ot_mismatch;
    bool has_first_position_mismatch;
    bool anchor_diagnostic_seen;
    bool pending;
    bool blocked;
} XgWorldCloudsShadowSnapshot;

#endif
