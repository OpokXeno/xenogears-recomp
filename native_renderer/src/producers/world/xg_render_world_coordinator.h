#ifndef XG_RENDER_WORLD_COORDINATOR_H
#define XG_RENDER_WORLD_COORDINATOR_H

#include "cpu_state.h"
#include "guest_render_types.h"
#include "gpu_render.h"
#include "xg_native_view.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderModelSpritePipelineServices
    XgRenderModelSpritePipelineServices;

typedef struct XgRenderWorldCoordinatorPolicy {
    uint32_t (*readiness_blocker)(void);
    bool (*authorize_direct_dispatch)(void);
    bool (*authentication_generation)(uint64_t *out_generation);
    uint64_t (*interpolation_scene_generation)(void);
    int32_t (*screen_x_cull_margin)(void);
    const XgNativeView *(*native_view)(void);
    bool (*authorize_guest_range)(uint32_t address, uint32_t size,
                                  uint32_t alignment, bool allow_scratchpad);
    bool (*stack_address_is_valid)(uint32_t address);
    bool (*authorize_guest_word)(uint32_t address);
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
    bool (*finalize_submission)(void);
    bool (*finalize_temporal)(void);
} XgRenderWorldCoordinatorPolicy;

typedef enum XgRenderWorldCoordinatorResult {
    XG_RENDER_WORLD_COORDINATOR_NONE = 0,
    XG_RENDER_WORLD_COORDINATOR_OBSERVED,
    XG_RENDER_WORLD_COORDINATOR_BYPASS,
    XG_RENDER_WORLD_COORDINATOR_FAILURE,
} XgRenderWorldCoordinatorResult;

XgRenderWorldCoordinatorResult xg_render_world_coordinator_observe_route(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    GuestRenderRenderMode render_mode, uint64_t scene_generation,
    const XgRenderModelSpritePipelineServices *model_sprite,
    const XgRenderWorldCoordinatorPolicy *policy);

void xg_render_world_coordinator_set_exec_phase_exchange(
    int (*exchange)(int phase));
void xg_render_world_coordinator_before_gpu_submission(
    const XgRenderWorldCoordinatorPolicy *policy);
void xg_render_world_coordinator_complete_gpu_source_frame(
    const XgRenderWorldCoordinatorPolicy *policy);
void xg_render_world_coordinator_disable(
    const XgRenderWorldCoordinatorPolicy *policy);
void xg_render_world_coordinator_scene_boundary(bool generation_advanced);
void xg_render_world_coordinator_reset(void);

#endif
