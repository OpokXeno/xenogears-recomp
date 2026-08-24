#ifndef XG_RENDER_MODEL_SPRITE_PIPELINE_H
#define XG_RENDER_MODEL_SPRITE_PIPELINE_H

#include "cpu_state.h"
#include "guest_render_types.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_ir.h"
#include "xg_render_model_repository.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_submission.h"
#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    XG_RENDER_MODEL_DISPATCH_CALLER_NONE = 0u,
    XG_RENDER_MODEL_DISPATCH_CALLER_RESIDENT,
    XG_RENDER_MODEL_DISPATCH_CALLER_OVERLAY,
};

typedef struct XgRenderModelSpritePipelineServices {
    const XgRenderProducerLifecycleServices *lifecycle;
    const XgRenderModelRepositoryServices *repository;
    int32_t (*screen_x_cull_margin)(void);
    bool (*pre_scene_available)(uint32_t count);
    bool (*stage_pre_scene)(const XgRenderPreScenePrimitive *record);
    bool (*stage_standalone)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_detail);
    void (*watch_resource)(uint32_t address, uint32_t size);
} XgRenderModelSpritePipelineServices;

void xg_render_model_sprite_pipeline_begin_packet_copy(
    CPUState *cpu, GuestRenderRenderMode render_mode);
void xg_render_model_sprite_pipeline_finish_packet_copy(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_observe_ft4_template(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_observe_ft3_template(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_capture_ft3_link(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_finish_ft3_link(
    CPUState *cpu, const XgRenderModelSpritePipelineServices *services);

void xg_render_model_sprite_pipeline_model_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_model_ft4_seam(
    CPUState *cpu, uint32_t pc, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_model_ft3_seam(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_observe_ft4_guest_pass(
    CPUState *cpu, bool average_mode);
void xg_render_model_sprite_pipeline_observe_ft3_guest_pass(CPUState *cpu);
void xg_render_model_sprite_pipeline_model_finish(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_model_end(void);

void xg_render_model_sprite_pipeline_sprite_begin(
    CPUState *cpu, bool wrapper_scope, GuestRenderRenderMode render_mode);
void xg_render_model_sprite_pipeline_sprite_geometry_seam(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_sprite_material_seam(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_sprite_end(
    bool nonwrapper_only, GuestRenderRenderMode render_mode,
    const XgRenderModelSpritePipelineServices *services);

void xg_render_model_sprite_pipeline_clear_model(void);
void xg_render_model_sprite_pipeline_clear_sprite(void);
void xg_render_model_sprite_pipeline_invalidate_model_code(void);
void xg_render_model_sprite_pipeline_invalidate_model_data(void);
void xg_render_model_sprite_pipeline_invalidate_sprite_code(void);
void xg_render_model_sprite_pipeline_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_render_model_sprite_pipeline_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
bool xg_render_model_sprite_pipeline_resident_lifecycle_pc(uint32_t pc);
void xg_render_model_sprite_pipeline_record_ft3_replay(
    XgRenderModelReplayResult result,
    const GuestRenderNativeStreamMissContext *context);
void xg_render_model_sprite_pipeline_record_ft4_replay(
    XgRenderModelReplayResult result, bool sprite_opcode);

void xg_render_model_sprite_pipeline_ft4_snapshot(
    PsxXgRenderModelFt4ShadowSnapshot *out_snapshot);
void xg_render_model_sprite_pipeline_ft3_snapshot(
    PsxXgRenderModelFt3ShadowSnapshot *out_snapshot);
void xg_render_model_sprite_pipeline_sprite_snapshot(
    PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot);
void xg_render_model_sprite_pipeline_reset(
    const XgRenderModelSpritePipelineServices *services);
void xg_render_model_sprite_pipeline_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
