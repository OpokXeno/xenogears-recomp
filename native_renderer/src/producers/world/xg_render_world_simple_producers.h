#ifndef XG_RENDER_WORLD_SIMPLE_PRODUCERS_H
#define XG_RENDER_WORLD_SIMPLE_PRODUCERS_H

#include "cpu_state.h"
#include "guest_render_types.h"
#include "gpu_render.h"
#include "xg_native_view.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_ir.h"
#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderWorldSimpleServices {
    uint32_t (*coordinated_dispatch_blocker)(void);
    bool (*authorize_direct_dispatch)(void);
    bool (*authentication_generation)(uint64_t *out_generation);
    uint64_t (*interpolation_scene_generation)(void);
    int32_t (*screen_x_cull_margin)(void);
    const XgNativeView *(*native_view)(void);
    bool (*authorize_guest_range)(uint32_t address, uint32_t size,
                                  uint32_t alignment, bool allow_scratchpad);
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
} XgRenderWorldSimpleServices;

void xg_render_world_simple_set_terrain_temporal_coverage(bool enabled);
void xg_render_world_terrain_water_note_entry(
    CPUState *cpu, bool native_enabled);

bool xg_render_world_terrain_water_cutover(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
bool xg_render_world_entity_shadows_cutover(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
bool xg_render_world_decorations_cutover(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
bool xg_render_world_horizon_cutover(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
bool xg_render_world_effects_cutover(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
bool xg_render_world_minimap_cutover(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);

void xg_render_world_terrain_water_shadow_begin(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
void xg_render_world_terrain_water_shadow_finish(CPUState *cpu);
void xg_render_world_entity_shadows_shadow_begin(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
void xg_render_world_entity_shadows_shadow_finish(CPUState *cpu);
void xg_render_world_entity_shadows_shadow_observe_transform(CPUState *cpu);
void xg_render_world_decorations_shadow_outer_begin(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
void xg_render_world_decorations_shadow_helper_begin(CPUState *cpu);
void xg_render_world_decorations_shadow_helper_finish(CPUState *cpu);
void xg_render_world_decorations_shadow_outer_finish(CPUState *cpu);
void xg_render_world_horizon_shadow_begin(
    CPUState *cpu, bool enabled, const XgRenderWorldSimpleServices *services);
void xg_render_world_horizon_shadow_finish(CPUState *cpu);
void xg_render_world_effects_shadow_begin(
    CPUState *cpu, bool enabled, const XgRenderWorldSimpleServices *services);
void xg_render_world_effects_shadow_finish(CPUState *cpu);
void xg_render_world_minimap_shadow_begin(
    CPUState *cpu, const XgRenderWorldSimpleServices *services);
void xg_render_world_minimap_shadow_finish(CPUState *cpu);

void xg_render_world_simple_invalidate_semantic_shadows(void);
void xg_render_world_horizon_shadow_clear_pending(void);
void xg_render_world_horizon_shadow_invalidate_code(void);
void xg_render_world_effects_shadow_clear_pending(void);
void xg_render_world_effects_shadow_invalidate_code(void);
void xg_render_world_simple_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_render_world_simple_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
void xg_render_world_simple_scene_boundary(void);

void xg_render_world_horizon_snapshot(
    PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot);
void xg_render_world_effects_snapshot(
    PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot);
void xg_render_world_terrain_water_snapshot(
    PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot);
void xg_render_world_entity_shadows_snapshot(
    PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot);
void xg_render_world_decorations_snapshot(
    PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot);
void xg_render_world_minimap_snapshot(
    PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot);
void xg_render_world_simple_reset(void);
void xg_render_world_simple_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
