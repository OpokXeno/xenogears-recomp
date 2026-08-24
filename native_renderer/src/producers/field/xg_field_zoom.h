#ifndef XG_FIELD_ZOOM_H
#define XG_FIELD_ZOOM_H

#include "guest_render_types.h"
#include "guest_render_native_stream.h"
#include "gpu_render.h"
#include "xg_render_snapshot_types.h"
#include "xg_render_ir.h"
#include "xg_render_submission.h"
#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

enum {
    XG_RENDER_ZOOM_QUAD_COUNT = 5u,
    XG_RENDER_ZOOM_BUFFER_COUNT = 2u,
    XG_RENDER_ZOOM_OT_BUCKET = 0x2002u,
    XG_RENDER_ZOOM_TEMPLATE_STORE_PC = 0x800a6884u,
};

typedef struct XgRenderZoomQuadSource {
    int16_t projection_x[4];
    int16_t projection_y[4];
    int16_t projection_z[4];
    int16_t x[4];
    int16_t y[4];
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint16_t tpage;
} XgRenderZoomQuadSource;

typedef struct XgRenderZoomSource {
    XgRenderZoomQuadSource quads[XG_RENDER_ZOOM_BUFFER_COUNT]
                              [XG_RENDER_ZOOM_QUAD_COUNT];
    uint64_t generation;
    uint64_t artifact_generation;
    uint32_t producer_store_pc;
    uint8_t command;
    bool semi_transparent;
    bool authenticated;
    bool valid;
} XgRenderZoomSource;

typedef struct XgRenderZoomNativeRecord {
    XgRenderIrNativePrimitive primitive;
    int16_t x[4];
    int16_t y[4];
    uint32_t packet_address;
    uint32_t draw_mode_address;
    uint32_t packet_tag;
    uint32_t draw_mode_tag;
} XgRenderZoomNativeRecord;

typedef struct XgFieldZoomPipelineServices {
    bool (*proof_active)(void);
    bool (*pre_scene_available)(uint32_t primitive_count);
    bool (*stage_active)(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        uint8_t payload_word_count, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_blocker);
    bool (*stage_pre_scene)(const XgRenderPreScenePrimitive *record);
    bool (*authorize_replay)(
        const XgRenderZoomSource *source,
        const GuestRenderNativeStreamMissContext *context);
    uint64_t (*interpolation_scene)(void);
    void (*reject_policy)(uint32_t blocker);
} XgFieldZoomPipelineServices;

typedef struct XgFieldZoomLocalProducerServices {
    void (*watch_resource)(uint32_t address, uint32_t size);
} XgFieldZoomLocalProducerServices;

void xg_field_zoom_reset(void);
void xg_field_zoom_classify_code_write(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
void xg_field_zoom_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_field_zoom_reset_pending(void);
void xg_field_zoom_reset_source_and_invocation(void);
bool xg_field_zoom_source_valid(void);
void xg_field_zoom_template_contract_snapshot(
    PsxXgRenderZoomTemplateContractSnapshot *out_snapshot);
void xg_field_zoom_register_resource_watches(
    void (*watch_resource)(uint32_t address, uint32_t size));
void xg_field_zoom_counters_reset(void);
void xg_field_zoom_note_cutover_attempt(void);
void xg_field_zoom_note_native_invocation(uint32_t primitive_count);
void xg_field_zoom_note_replay_invocation(uint32_t primitive_count);
void xg_field_zoom_note_rejection(uint32_t blocker);
bool xg_field_zoom_code_write_overlaps(uint32_t address, uint32_t size);
void xg_field_zoom_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));
void xg_field_zoom_invalidate_overlapping(uint32_t address, uint32_t size);
bool xg_field_zoom_caller_is_authorized(uint32_t return_address);
void xg_field_zoom_observe_initializer_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t artifact_generation);
void xg_field_zoom_observe_initializer_commit(
    CPUState *cpu, uint64_t artifact_generation);
bool xg_field_zoom_local_producer_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t artifact_generation);
bool xg_field_zoom_local_producer_writer(CPUState *cpu,
                                         uint32_t writer_index);
bool xg_field_zoom_local_producer_commit(
    CPUState *cpu, uint64_t artifact_generation,
    const XgFieldZoomLocalProducerServices *services);
void xg_field_zoom_observe_rgb_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t artifact_generation);
void xg_field_zoom_observe_rgb_commit(CPUState *cpu, uint64_t artifact_generation);
void xg_field_zoom_observe_entry(
    CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t artifact_generation);
bool xg_field_zoom_reject(
    uint32_t blocker, const XgFieldZoomPipelineServices *services);
bool xg_field_zoom_cutover(
    CPUState *cpu, uint32_t continuation,
    const XgFieldZoomPipelineServices *services);
bool xg_field_zoom_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
    const XgFieldZoomPipelineServices *services);

#endif
