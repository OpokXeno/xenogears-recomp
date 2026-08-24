#ifndef XG_WORLD_DECORATIONS_SNAPSHOT_TYPES_H
#define XG_WORLD_DECORATIONS_SNAPSHOT_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum XgWorldDecorationsShadowPhase {
    XG_WORLD_DECORATIONS_SHADOW_PHASE_IDLE = 0,
    XG_WORLD_DECORATIONS_SHADOW_PHASE_OUTER,
    XG_WORLD_DECORATIONS_SHADOW_PHASE_HELPER,
    XG_WORLD_DECORATIONS_SHADOW_PHASE_BLOCKED,
} XgWorldDecorationsShadowPhase;

typedef enum XgWorldDecorationsShadowBlocker {
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_NONE = 0,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_EXPLICIT,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_LIFECYCLE_INVALIDATED,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_NESTED_OUTER,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_UNMATCHED_HELPER_BEGIN,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_NESTED_HELPER,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_UNMATCHED_HELPER_FINISH,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_FINISH_WITH_HELPER,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_UNMATCHED_OUTER_FINISH,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_INVALID_OBSERVATION,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_AUTHENTICATION,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_SITE,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_RETURN,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OUTER_STACK,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_SITE,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_RETURN,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_HELPER_STACK,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_READ_FAILED,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_BUFFER_INDEX,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_SHARED_COUNT,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_PACKET_BASE,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_CONTEXT,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_OT_BASE,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_SOURCE_CAPTURE,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_BUILD,
    XG_WORLD_DECORATIONS_SHADOW_BLOCKER_COUNTER_SATURATED,
} XgWorldDecorationsShadowBlocker;

typedef struct XgWorldDecorationsShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t outer_begin_count;
    uint64_t outer_finish_count;
    uint64_t outer_match_count;
    uint64_t outer_mismatch_count;
    uint64_t helper_begin_count;
    uint64_t helper_finish_count;
    uint64_t helper_match_count;
    uint64_t helper_mismatch_count;
    uint64_t source_capture_count;
    uint64_t source_capture_failure_count;
    uint64_t source_read_count;
    uint64_t source_read_bytes;
    uint64_t candidate_count;
    uint64_t primitive_count;
    uint64_t match_count;
    uint64_t mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t payload_word_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t ot_match_count;
    uint64_t ot_mismatch_count;
    uint64_t count_mismatch_count;
    uint64_t a3_mismatch_count;
    uint64_t s0_mismatch_count;
    uint64_t argument_mismatch_count;
    uint64_t buffer_mismatch_count;
    uint64_t lifecycle_invalidation_count;
    uint64_t block_count;
    uint64_t first_mismatch_helper;
    uint32_t first_mismatch_kind;
    uint32_t first_mismatch_packet;
    uint32_t first_mismatch_source;
    uint32_t first_mismatch_word;
    uint32_t first_mismatch_address;
    uint32_t first_expected_word;
    uint32_t first_actual_word;
    uint32_t last_buffer_index;
    uint32_t last_packet_base;
    uint32_t last_ot_base;
    uint32_t last_position_address;
    uint32_t last_position_count;
    uint32_t last_initial_count;
    uint32_t last_final_count;
    uint32_t last_a3;
    uint32_t last_s0;
    uint32_t last_capture_result;
    uint32_t last_build_result;
    uint32_t blocker_detail;
    XgWorldDecorationsShadowPhase phase;
    XgWorldDecorationsShadowBlocker blocker;
    bool has_first_mismatch;
    bool outer_active;
    bool helper_active;
    bool blocked;
} XgWorldDecorationsShadowSnapshot;

#endif
