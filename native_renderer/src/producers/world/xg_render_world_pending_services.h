#ifndef XG_RENDER_WORLD_PENDING_SERVICES_H
#define XG_RENDER_WORLD_PENDING_SERVICES_H

#include "cpu_state.h"
#include "gpu_render.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_ir.h"
#include "xg_render_invalidation_event.h"
#include "xg_world_actor_sprites_source_capture.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderWorldPendingServices {
    bool (*cutover_ready)(void);
    bool (*authentication_generation)(uint64_t *out_generation);
    bool (*authorize_guest_range)(uint32_t address, uint32_t size,
                                  uint32_t alignment, bool allow_scratchpad);
    int32_t (*screen_x_cull_margin)(void);
    bool (*begin_submission)(void);
    bool (*stage_native)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id);
    bool (*stage_temporal)(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy);
    void (*abort_submission)(void);
    bool (*coordinator_in_progress)(void);
    bool (*coordinator_begin)(void);
    void (*coordinator_end)(void);
    void (*coordinator_fail)(void);
    bool (*coordinator_failed)(void);
} XgRenderWorldPendingServices;

void xg_render_world_actor_context_begin(
    CPUState *cpu, const XgRenderWorldPendingServices *services);
void xg_render_world_actor_context_finish(
    CPUState *cpu, const XgRenderWorldPendingServices *services);
bool xg_render_world_actor_context_active(void);
bool xg_render_world_actor_pending_valid(void);
bool xg_render_world_actor_prepare(
    CPUState *cpu, const XgRenderWorldPendingServices *services);
bool xg_render_world_actor_commit(
    CPUState *cpu, const XgRenderWorldPendingServices *services);
void xg_render_world_actor_clear_pending(void);
void xg_render_world_actor_clear_context(void);
void xg_render_world_actor_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
bool xg_render_world_actor_pending_metadata_copy(
    CPUState **out_owner_cpu,
    XgWorldActorSpritesNativePreparation *out_preparation);
bool xg_render_world_actor_pending_packet_copy(
    uint32_t index, uint32_t *out_address, uint32_t *out_tag,
    uint32_t *out_payload_words, uint32_t capacity);
bool xg_render_world_actor_pending_ot_copy(
    uint32_t index, uint32_t *out_address, uint32_t *out_value);
void xg_render_world_actor_reset(void);
void xg_render_world_actor_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

void xg_render_world_clouds_prepare(
    CPUState *cpu, const XgRenderWorldPendingServices *services);
void xg_render_world_clouds_commit(
    CPUState *cpu, const XgRenderWorldPendingServices *services);
void xg_render_world_clouds_clear_pending(void);
void xg_render_world_clouds_reset(void);
void xg_render_world_clouds_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
