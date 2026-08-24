#ifndef XG_WORLD_MINIMAP_SNAPSHOT_TYPES_H
#define XG_WORLD_MINIMAP_SNAPSHOT_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum XgWorldMinimapShadowResult {
    XG_WORLD_MINIMAP_SHADOW_OK = 0,
    XG_WORLD_MINIMAP_SHADOW_NOT_APPLICABLE,
    XG_WORLD_MINIMAP_SHADOW_NOT_PENDING,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH,
    XG_WORLD_MINIMAP_SHADOW_BLOCKED,
    XG_WORLD_MINIMAP_SHADOW_INVALID_ARGUMENT,
    XG_WORLD_MINIMAP_SHADOW_SOURCE_CAPTURE_FAILED,
    XG_WORLD_MINIMAP_SHADOW_BUILD_FAILED,
    XG_WORLD_MINIMAP_SHADOW_LIFECYCLE_ERROR,
} XgWorldMinimapShadowResult;

typedef enum XgWorldMinimapShadowBlocker {
    XG_WORLD_MINIMAP_SHADOW_BLOCK_NONE = 0,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_ARGUMENT,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_UNEXPECTED_BEGIN,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_SOURCE_CAPTURE,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_SOURCE_BUILD,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_CONTEXT,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_INVALID_STACK,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_SAVED_RETURN_ADDRESS,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_COUNTER_OVERFLOW,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_LIFECYCLE_INVALIDATED,
    XG_WORLD_MINIMAP_SHADOW_BLOCK_EXTERNAL,
} XgWorldMinimapShadowBlocker;

typedef enum XgWorldMinimapShadowMismatchKind {
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_NONE = 0,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_G3_TAG,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_G3_PAYLOAD,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_DRAW_MODE_TAG,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_DRAW_MODE_PAYLOAD,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_MARKER_TAG,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_MARKER_PAYLOAD,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_INACTIVE_MARKER_MUTATION,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_PANEL_TAG,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_PANEL_PAYLOAD,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_OT_HEAD,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_SCRATCH,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_CONTEXT,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_OT_POINTER,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_STACK_POINTER,
    XG_WORLD_MINIMAP_SHADOW_MISMATCH_SAVED_RETURN_ADDRESS,
} XgWorldMinimapShadowMismatchKind;

typedef struct XgWorldMinimapShadowFirstMismatch {
    uint64_t generation;
    XgWorldMinimapShadowMismatchKind kind;
    uint32_t address;
    uint32_t word_index;
    uint32_t source_index;
    uint32_t expected;
    uint32_t actual;
} XgWorldMinimapShadowFirstMismatch;

typedef struct XgWorldMinimapShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t entry_count;
    uint64_t rejected_caller_count;
    uint64_t begin_count;
    uint64_t finish_count;
    uint64_t completion_count;
    uint64_t invocation_match_count;
    uint64_t invocation_mismatch_count;
    uint64_t source_capture_count;
    uint64_t source_capture_failure_count;
    uint64_t source_read_count;
    uint64_t source_read_bytes;
    uint64_t compared_word_count;
    uint64_t mismatched_word_count;
    uint64_t primitive_count;
    uint64_t g3_match_count;
    uint64_t g3_mismatch_count;
    uint64_t draw_mode_match_count;
    uint64_t draw_mode_mismatch_count;
    uint64_t active_marker_count;
    uint64_t marker_match_count;
    uint64_t marker_mismatch_count;
    uint64_t inactive_marker_count;
    uint64_t inactive_marker_match_count;
    uint64_t inactive_marker_mutation_count;
    uint64_t panel_match_count;
    uint64_t panel_mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t ot_mismatch_count;
    uint64_t scratch_mismatch_count;
    uint64_t context_mismatch_count;
    uint64_t ot_pointer_mismatch_count;
    uint64_t stack_mismatch_count;
    uint64_t saved_return_mismatch_count;
    uint64_t invalidation_count;
    uint64_t block_count;
    uint64_t active_generation;
    uint32_t last_marker_mask;
    uint32_t last_active_marker_count;
    uint32_t last_ordering_count;
    uint32_t last_context;
    uint32_t last_ot_address;
    uint32_t last_initial_ot_word;
    uint32_t last_capture_result;
    uint32_t last_build_result;
    XgWorldMinimapShadowResult last_result;
    XgWorldMinimapShadowBlocker blocker;
    XgWorldMinimapShadowFirstMismatch first_mismatch;
    bool pending;
    bool blocked;
    bool counter_overflowed;
} XgWorldMinimapShadowSnapshot;

#endif
