#ifndef XG_RENDER_WORLD_MODELS_PIPELINE_H
#define XG_RENDER_WORLD_MODELS_PIPELINE_H

#include "xg_render_invalidation_event.h"

#include "cpu_state.h"
#include "gpu_render.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_ir.h"
#include "xg_render_world_model_repository.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderWorldModelsPipelineServices {
    const XgRenderWorldModelRepositoryServices *repository;
    bool (*cutover_ready)(void);
    bool (*authentication_generation)(uint64_t *out_generation);
    bool (*authorize_guest_range)(uint32_t address, uint32_t size,
                                  uint32_t alignment, bool allow_scratchpad);
    bool (*stack_address_is_valid)(uint32_t address);
    uint64_t (*interpolation_scene_generation)(void);
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
} XgRenderWorldModelsPipelineServices;

bool xg_render_world_models_prepare(
    CPUState *cpu, const XgRenderWorldModelsPipelineServices *services);
bool xg_render_world_models_commit(
    CPUState *cpu, const XgRenderWorldModelsPipelineServices *services);
void xg_render_world_models_clear_pending(void);
void xg_render_world_models_snapshot(
    PsxXgRenderWorldNativeSnapshot *out_snapshot);
bool xg_render_world_models_pending_metadata_copy(
    CPUState **out_owner_cpu, uint32_t *out_entry_stack_pointer,
    XgWorldModelsNativePreparation *out_preparation,
    XgWorldModelsNativeCommit *out_commit);
bool xg_render_world_models_pending_node_copy(
    uint32_t index, XgWorldModelsNodeSideEffect *out_effect);
bool xg_render_world_models_pending_packet_copy(
    uint32_t index, uint32_t *out_address, uint32_t *out_words,
    uint32_t capacity, uint32_t *out_word_count, uint32_t *out_write_mask);
bool xg_render_world_models_pending_ot_copy(
    uint32_t index, uint32_t *out_address, uint32_t *out_value);
void xg_render_world_models_reset(void);
void xg_render_world_models_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
