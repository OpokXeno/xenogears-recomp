#ifndef XG_RENDER_MODEL_REPOSITORY_H
#define XG_RENDER_MODEL_REPOSITORY_H

#include "guest_render_native_stream.h"
#include "guest_render_types.h"
#include "xg_render_ir.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

typedef struct XgRenderModelFt3SourceRecord {
    XgRenderIrNativePrimitive primitive;
    XgRenderProducerLifecycle lifecycle;
    uint32_t source_id;
    uint32_t descriptor_address;
    uint32_t material_word;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint16_t uv[3];
    uint16_t tpage;
    uint16_t clut;
    bool interpolation_identity_valid;
    bool geometry_ready;
    bool link_pending;
    bool valid;
} XgRenderModelFt3SourceRecord;

typedef struct XgRenderModelFt4SourceRecord {
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t source_id;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t opcode;
    bool interpolation_identity_valid;
    bool semantic_ready;
    bool valid;
} XgRenderModelFt4SourceRecord;

typedef struct XgRenderModelFt4Template {
    XgRenderProducerLifecycle lifecycle;
    uint32_t packet_address;
    uint32_t descriptor_address;
    uint32_t material_word;
    uint16_t uv[4];
    uint16_t tpage;
    uint16_t clut;
    bool valid;
} XgRenderModelFt4Template;

typedef struct XgRenderModelSubmissionCallbacks {
    void (*register_ft3_replay_source)(uint32_t source_id);
    void (*register_ft4_replay_source)(uint32_t source_id);
    uint64_t (*interpolation_scene)(void);
    bool (*record_interpolation_anchors)(const GpuRenderSemantic *semantic);
} XgRenderModelSubmissionCallbacks;

typedef struct XgRenderModelResourceCallbacks {
    void (*watch)(uint32_t address, uint32_t size);
    void (*watch_ft3_descriptor)(uint32_t address, uint32_t size);
    bool (*ft3_descriptor_overlaps)(uint32_t address, uint32_t size);
    void (*reset_ft3_descriptors)(void);
} XgRenderModelResourceCallbacks;

typedef struct XgRenderModelRepositoryServices {
    const XgRenderProducerLifecycleServices *lifecycle;
    XgRenderModelSubmissionCallbacks submission;
    XgRenderModelResourceCallbacks resources;
} XgRenderModelRepositoryServices;

typedef struct XgRenderModelSourcePublication {
    uint32_t resource_address;
    uint32_t resource_size;
    uint32_t descriptor_address;
    uint32_t descriptor_size;
    bool register_replay;
} XgRenderModelSourcePublication;

typedef enum XgRenderModelReplayResult {
    XG_RENDER_MODEL_REPLAY_NOT_APPLICABLE = 0,
    XG_RENDER_MODEL_REPLAY_LOOKUP_ABSENT,
    XG_RENDER_MODEL_REPLAY_LOOKUP_INVALID,
    XG_RENDER_MODEL_REPLAY_RECORD_REJECTED,
    XG_RENDER_MODEL_REPLAY_CONTAINER_REJECTED,
    XG_RENDER_MODEL_REPLAY_LIFECYCLE_REJECTED,
    XG_RENDER_MODEL_REPLAY_TRANSLATE_REJECTED,
    XG_RENDER_MODEL_REPLAY_RESOLVED,
} XgRenderModelReplayResult;

void xg_render_model_repository_clear_ft3_sources(
    const XgRenderModelRepositoryServices *services);
void xg_render_model_repository_clear_ft4_sources(void);
uint32_t xg_render_model_repository_ft3_source_count(void);
bool xg_render_model_repository_ft3_writer_is_authorized(
    const GuestRenderNativeStreamMissContext *context);
bool xg_render_model_repository_ft4_writer_is_authorized(
    const GuestRenderNativeStreamMissContext *context);
void xg_render_model_repository_retain_resident_sources(
    const XgRenderModelRepositoryServices *services);

bool xg_render_model_repository_store_ft3_source(
    const XgRenderModelFt3SourceRecord *record,
    const XgRenderModelSourcePublication *publication,
    const XgRenderModelRepositoryServices *services);
bool xg_render_model_repository_store_ft4_source(
    const XgRenderModelFt4SourceRecord *record,
    const XgRenderModelSourcePublication *publication,
    const XgRenderModelRepositoryServices *services);
bool xg_render_model_repository_store_ft4_sources(
    const XgRenderModelFt4SourceRecord *records,
    const XgRenderModelSourcePublication *publications, uint32_t count,
    const XgRenderModelRepositoryServices *services);
const XgRenderModelFt3SourceRecord *xg_render_model_repository_find_ft3_source(
    uint32_t source_id);
bool xg_render_model_repository_record_resolved_producer_anchors(
    uint64_t command_id, const GpuRenderSemantic *resolved,
    const XgRenderModelRepositoryServices *services);
bool xg_render_model_repository_record_active_producer_anchors(
    const XgRenderModelRepositoryServices *services);
void xg_render_model_repository_finish_ft3_link(
    uint32_t source_id, bool linked,
    const XgRenderModelSourcePublication *publication,
    const XgRenderModelRepositoryServices *services);

void xg_render_model_repository_reset_templates(void);
bool xg_render_model_repository_store_template(
    const XgRenderModelFt4Template *captured);
const XgRenderModelFt4Template *xg_render_model_repository_find_packet_template(
    uint32_t packet_address, GuestRenderRenderMode render_mode,
    const XgRenderModelRepositoryServices *services);
const XgRenderModelFt4Template *
xg_render_model_repository_find_descriptor_template(
    uint32_t descriptor_address, GuestRenderRenderMode render_mode,
    const XgRenderModelRepositoryServices *services);
bool xg_render_model_repository_packet_template_present(
    uint32_t packet_address);
bool xg_render_model_repository_descriptor_template_present(
    uint32_t descriptor_address);

void xg_render_model_repository_invalidate_overlapping(
    uint32_t address, uint32_t size,
    const XgRenderModelRepositoryServices *services);
void xg_render_model_repository_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

void xg_render_model_repository_begin_packet_copy(
    CPUState *cpu, GuestRenderRenderMode render_mode);
void xg_render_model_repository_finish_packet_copy(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderModelRepositoryServices *services);

XgRenderModelReplayResult xg_render_model_repository_resolve_ft3(
    const GuestRenderNativeStreamMissContext *context,
    GuestRenderRenderMode render_mode, GpuRenderSemantic *out_semantic,
    const XgRenderModelRepositoryServices *services);
XgRenderModelReplayResult xg_render_model_repository_resolve_ft4(
    const GuestRenderNativeStreamMissContext *context,
    GuestRenderRenderMode render_mode, GpuRenderSemantic *out_semantic,
    const XgRenderModelRepositoryServices *services);

#endif
