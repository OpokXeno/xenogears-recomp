#ifndef XG_RENDER_FIELD_CHARACTER_PIPELINE_H
#define XG_RENDER_FIELD_CHARACTER_PIPELINE_H

#include "xg_render_invalidation_event.h"

#include "cpu_state.h"
#include "gpu_render.h"
#include "xg_field_character_source_capture_types.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_source_types.h"
#include "xg_render_ir.h"
#include "xg_render_submission.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderFieldCharacterPipelineServices {
    bool (*source_context_matches)(XgRenderAuthTier tier,
                                   uint32_t *out_context_bits);
    bool (*source_site_lookup)(
        uint32_t pc, uint32_t instruction_word,
        PsxXgRenderSourceSiteMetadata *out_metadata);
    bool (*capture_auth_context)(
        XgFieldCharacterSourceCaptureRequest *request);
    uint64_t (*scene_generation)(void);
    uint64_t (*interpolation_scene_generation)(void);
    XgRenderFieldCharacterStageResult (*stage_candidate)(
        const XgRenderIrNativePrimitive *primitive,
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        GpuRenderTransactionId visual_id);
    void (*reject_auth)(void);
} XgRenderFieldCharacterPipelineServices;

bool xg_render_field_character_observe_source(
    CPUState *cpu, XgRenderAuthTier tier, PsxXgRenderSourceStage stage,
    uint32_t pc, uint32_t instruction_word, uint32_t auxiliary,
    const XgRenderFieldCharacterPipelineServices *services);
bool xg_render_field_character_inner_call_matches(
    XgRenderAuthTier tier, uint32_t hook, uint32_t pc,
    uint32_t instruction_word,
    const XgRenderFieldCharacterPipelineServices *services);
bool xg_render_field_character_native_actor_cutover(
    CPUState *cpu, uint32_t continuation, XgRenderAuthTier tier,
    const XgRenderFieldCharacterPipelineServices *services);
bool xg_render_field_character_native_bypass(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    const XgRenderFieldCharacterPipelineServices *services);
bool xg_render_field_character_resident_ft4_observe(
    CPUState *cpu, uint32_t stage, uint32_t pc, uint32_t instruction_word,
    const XgRenderFieldCharacterPipelineServices *services);

void xg_render_field_character_reject(
    uint32_t blocker,
    const XgRenderFieldCharacterPipelineServices *services);
void xg_render_field_character_disarm(void);
void xg_render_field_character_scene_boundary(void);
void xg_render_field_character_source_reset(void);
void xg_render_field_character_reset(void);
void xg_render_field_character_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_render_field_character_ft4_geometry_enable(bool enabled);
void xg_render_field_character_producer_family_enable(bool enabled);
void xg_render_field_character_note_pre_scene_blocker(uint32_t blocker);

bool xg_render_field_character_source_blocked(void);
bool xg_render_field_character_producer_family_enabled(void);
void xg_render_field_character_source_snapshot(
    PsxXgRenderSourceSnapshot *out_snapshot);
void xg_render_field_character_source_collector_snapshot(
    FieldCharacterShadowSummary *out_summary);
bool xg_render_field_character_ft4_geometry_pop(
    PsxXgRenderFt4Geometry *out_geometry);
void xg_render_field_character_ft4_geometry_snapshot(
    PsxXgRenderFt4GeometrySnapshot *out_snapshot);
void xg_render_field_character_producer_family_snapshot(
    PsxXgRenderProducerFamilySnapshot *out_snapshot);

#endif
