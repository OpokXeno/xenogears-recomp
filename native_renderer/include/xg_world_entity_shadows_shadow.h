#ifndef XG_WORLD_ENTITY_SHADOWS_SHADOW_H
#define XG_WORLD_ENTITY_SHADOWS_SHADOW_H

#include "cpu_state.h"
#include "gpu.h"
#include "xg_world_entity_shadows.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XG_WORLD_ENTITY_SHADOWS_SHADOW_ENTRY XG_WORLD_ENTITY_SHADOWS_ENTRY_PC
#define XG_WORLD_ENTITY_SHADOWS_SHADOW_FINISH UINT32_C(0x80074e24)
#define XG_WORLD_ENTITY_SHADOWS_SHADOW_CALLSITE                            \
  XG_WORLD_ENTITY_SHADOWS_PRODUCER_CALLSITE
#define XG_WORLD_ENTITY_SHADOWS_SHADOW_RETURN                             \
  XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC

typedef enum XgWorldEntityShadowsShadowResult {
  XG_WORLD_ENTITY_SHADOWS_SHADOW_OK = 0,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_INVALID_ARGUMENT,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_INVALID_STATE,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKED,
} XgWorldEntityShadowsShadowResult;

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

typedef enum XgWorldEntityShadowsShadowMismatch {
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_NONE = 0,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PENDING_COUNT = 1u << 0,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PENDING_LIST = 1u << 1,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_CURSOR = 1u << 2,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PAYLOAD = 1u << 3,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_GEOMETRY = 1u << 4,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_TAG = 1u << 5,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_OT = 1u << 6,
  XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_CULL_WRITE = 1u << 7,
} XgWorldEntityShadowsShadowMismatch;

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

void xg_world_entity_shadows_shadow_reset(void);

XgWorldEntityShadowsShadowResult xg_world_entity_shadows_shadow_begin(
    CPUState *cpu, uint64_t generation, const GpuDrawState *draw);

XgWorldEntityShadowsShadowResult
xg_world_entity_shadows_shadow_finish(CPUState *cpu);
void xg_world_entity_shadows_shadow_observe_transform(CPUState *cpu);

/* A clean lifecycle boundary admits a new generation. Invalidating an open
 * invocation blocks because its finish can no longer be authenticated. */
void xg_world_entity_shadows_shadow_lifecycle_invalidate(void);
void xg_world_entity_shadows_shadow_lifecycle_block(void);
bool xg_world_entity_shadows_shadow_record_native_cutover(
    uint32_t primitive_count);

XgWorldEntityShadowsShadowResult xg_world_entity_shadows_shadow_snapshot(
    XgWorldEntityShadowsShadowSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif
