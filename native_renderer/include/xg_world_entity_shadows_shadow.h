#ifndef XG_WORLD_ENTITY_SHADOWS_SHADOW_H
#define XG_WORLD_ENTITY_SHADOWS_SHADOW_H

#include "cpu_state.h"
#include "gpu.h"
#include "xg_world_entity_shadows.h"
#include "xg_world_entity_shadows_snapshot_types.h"

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
