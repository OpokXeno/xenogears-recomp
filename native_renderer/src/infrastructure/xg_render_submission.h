#ifndef XG_RENDER_SUBMISSION_H
#define XG_RENDER_SUBMISSION_H

#include "xg_render_invalidation_event.h"
#include "xg_render_source_frame.h"

#include "gpu_render.h"
#include "guest_render_types.h"
#include "guest_render_transaction.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY = XG_RENDER_IR_ITEM_CAPACITY,
};

typedef struct XgRenderPreScenePrimitive {
    XgRenderIrNativePrimitive primitive;
    uint32_t packet_address;
    uint32_t source_primitive_index;
    uint32_t ot_bucket;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint8_t payload_word_count;
    bool interpolation_identity_valid;
    bool temporal_only;
    GpuRenderTemporalCullPolicy temporal_cull;
} XgRenderPreScenePrimitive;

typedef struct XgRenderPreSceneState {
    XgRenderPreScenePrimitive records[XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY];
    uint32_t count;
    uint32_t blocker;
    bool blocked;
} XgRenderPreSceneState;

typedef struct XgRenderStandaloneSubmissionState {
    GuestRenderVisualStateId visual_id;
    GuestRenderProducerHandle producer;
    bool open;
} XgRenderStandaloneSubmissionState;

/* One GP0 draw in submission order. Packet-adapter draws deliberately have no
 * producer provenance; an authenticated OT is not an independent producer. */
typedef struct XgRenderSubmissionCommand {
    GpuRenderSemantic semantic;
    XgRenderIrProvenanceKey provenance;
    uint32_t command_id;
    uint32_t source_primitive_index;
    uint8_t opcode;
    bool has_provenance;
    bool producer_captured;
} XgRenderSubmissionCommand;

typedef struct XgRenderSubmissionDiagnostics {
    uint64_t geometry_matched;
    uint64_t geometry_rejected;
    uint64_t layout_rejected;
    uint64_t source_missing;
    uint64_t attributes_updated;
    uint64_t work_captures;
    uint64_t work_temporal_skipped;
    uint32_t last_rejected_command;
} XgRenderSubmissionDiagnostics;

void xg_render_submission_diagnostics(XgRenderSubmissionDiagnostics *out);

typedef struct XgRenderTemporalCommandBinding {
    uint32_t command_id;
    uint64_t component_id;
} XgRenderTemporalCommandBinding;

uint64_t xg_render_submission_temporal_scene(void);
/* Publish AFTER the entire authenticated producer invocation has been staged.
 * Command IDs refer to those exact captures, never to future/reused packet slots.
 * New publications clear old bindings for this scope, including disappeared objects. */
bool xg_render_submission_publish_temporal_coverage(
    uint32_t producer_scope, const XgRenderTemporalComponent *components,
    uint32_t component_count, const XgRenderTemporalSample *samples, uint32_t sample_count,
    const XgRenderTemporalCommandBinding *bindings, uint32_t binding_count);
bool xg_render_submission_temporal_binding(
    const GpuRenderSemantic *semantic, XgRenderTemporalBinding *out_binding);

typedef enum XgRenderFieldCharacterStageResult {
    XG_RENDER_FIELD_CHARACTER_STAGE_OK = 0,
    XG_RENDER_FIELD_CHARACTER_STAGE_AUTH_FAILED,
    XG_RENDER_FIELD_CHARACTER_STAGE_SUBMISSION_FAILED,
} XgRenderFieldCharacterStageResult;

typedef struct XgRenderSubmissionServices {
    bool (*active_auth_available)(void);
    bool (*active_auth_snapshot)(
        GpuRenderTransactionId *out_visual_id,
        XgRenderIrProvenanceKey *out_provenance);
    bool (*active_auth_append)(
        uint32_t packet_address, uint32_t source_primitive_index,
        uint32_t ot_bucket, uint8_t payload_word_count,
        const XgRenderIrNativePrimitive *primitive,
        bool force_pending_capture,
        uint32_t *out_failure_detail);
    bool (*standalone_scene_config)(GuestRenderSceneConfig *out_config);
    bool (*source_frame_description)(
        XgRenderSourceFrameDescription *out_description);
    bool (*standalone_source_frame_description)(
        XgRenderSourceFrameDescription *out_description);
    bool (*source_frame_target)(
        const XgRenderSourceFrameDescription *description,
        XgSemanticResourceRef *out_target);
    bool (*presentation_gate)(void);
    uint64_t (*interpolation_generation)(void);
    uint64_t (*scene_generation)(void);
    /* Composition supplies the central materializer, which preserves all
     * geometry/modes and leaves order, resources and provenance untouched. */
    bool (*materialize_semantic_draw)(
        const GpuRenderSemantic *semantic, XgSemanticDrawRecord *out_draw);
} XgRenderSubmissionServices;

typedef struct XgRenderAuthenticatedIrDescription {
    GuestRenderRenderMode render_mode;
    size_t item_count;
} XgRenderAuthenticatedIrDescription;

typedef struct XgRenderAuthenticatedIrAccess {
    bool (*describe)(XgRenderAuthenticatedIrDescription *out_description);
    bool (*item_get)(size_t index, XgRenderIrNativeItem *out_item);
} XgRenderAuthenticatedIrAccess;

typedef void (*XgRenderSubmissionObserver)(
    const XgRenderIrNativePrimitive *primitive,
    uint32_t source_primitive_index,
    void *user_data);

void xg_render_submission_configure(
    const XgRenderSubmissionServices *services);
/* Work mode keeps authored command captures, but does not drive the legacy
 * native stream, temporal draw transport, or native scene publication. */
void xg_render_submission_set_native_work_mode(bool enabled);
bool xg_render_submission_native_work_mode(void);
void xg_render_submission_set_observer(
    XgRenderSubmissionObserver observer, void *user_data);
void xg_render_submission_reset(void);
void xg_render_submission_reset_transaction(void);
void xg_render_submission_reject_producer(void);
void xg_render_submission_disarm(void);
void xg_render_submission_prepare_authenticated_scene(void);
void xg_render_submission_scene_boundary(void);
void xg_render_submission_source_reset(void);
bool xg_render_submission_ensure_source_frame(
    const XgRenderSourceFrameDescription *description);
bool xg_render_submission_begin_source_frame(
    const XgRenderSourceFrameDescription *description,
    XgSemanticResourceRef *out_target);

/* Resolves a captured command's layout and canonical XY against the actual
 * packet, then takes final UV/color/material attributes from that packet.
 * Missing/retired
 * captures use the explicit packet adapter; legacy OT preflight rejects a
 * mismatched pending capture. In work mode call at GPU DRAW acceptance: a
 * mismatch keeps the accepted packet semantic and captures retire here, without
 * requiring an OT completion. This does not enable a GPU hard-fail policy.
 * prepare owns immutable resources until complete or cancellation. */
bool xg_render_submission_resolve_command(
    const XgRenderSourceFrameDescription *description, uint32_t command_id,
    const GpuRenderSemantic *packet, XgRenderSubmissionCommand *out_command);
/* Replaces geometry/material/mode fields only. Order, resource handles,
 * provenance and draw-level interpolation metadata remain owned by the caller. */
bool xg_render_submission_materialize_semantic_draw(
    const GpuRenderSemantic *semantic, XgSemanticDrawRecord *out_draw);
bool xg_render_submission_prepare_ordering_table(
    const XgRenderSourceFrameDescription *description,
    uint32_t start_address, uint32_t transferred_words,
    const XgRenderSubmissionCommand *commands, uint32_t command_count);
bool xg_render_submission_complete_ordering_table(
    uint32_t start_address, uint32_t transferred_words);
void xg_render_submission_cancel_ordering_table(void);
void xg_render_submission_note_semantic_current(const GpuRenderSemantic *semantic);
bool xg_render_submission_prepared_draw_copy(
    uint32_t index, XgSemanticDrawRecord *out_draw, uint32_t *out_command_id);
void xg_render_submission_ordering_table_status(
    uint32_t *out_detail, uint32_t *out_index,
    uint32_t *out_tpage, uint32_t *out_clut);

void xg_render_submission_pre_scene_clear(void);
void xg_render_submission_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_render_submission_pre_scene_block(
    uint32_t blocker, bool preserve_existing_blocker);
bool xg_render_submission_pre_scene_blocked(void);
bool xg_render_submission_pre_scene_available(uint32_t primitive_count);
uint32_t xg_render_submission_pre_scene_count(void);
uint32_t xg_render_submission_pre_scene_blocker(void);
bool xg_render_submission_pre_scene_item_copy(
    uint32_t index, XgRenderPreScenePrimitive *out_record);
bool xg_render_submission_pre_scene_stage(
    const XgRenderPreScenePrimitive *record);
bool xg_render_submission_pre_scene_discard(
    const XgRenderPreScenePrimitive *record);
bool xg_render_submission_record_interpolation_anchors(
    const GpuRenderSemantic *semantic);
bool xg_render_submission_pre_scene_flush(void);
bool xg_render_submission_validate_authenticated_ir(
    const XgRenderAuthenticatedIrAccess *access);

GuestRenderTransactionStatus xg_render_submission_stage_exact(
    GpuRenderTransactionId visual_id, uint64_t exact_command_id,
    const GpuRenderSemantic *semantic);
bool xg_render_submission_stage_active_primitive(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t ot_bucket,
    uint8_t payload_word_count, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id, uint32_t *failure_blocker);
XgRenderFieldCharacterStageResult
xg_render_submission_stage_field_character(
    const XgRenderIrNativePrimitive *primitive,
    const GpuRenderSemantic *semantic, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t ot_bucket,
    GpuRenderTransactionId visual_id);

bool xg_render_submission_standalone_open(void);
bool xg_render_submission_standalone_begin(void);
void xg_render_submission_standalone_abort(void);
bool xg_render_submission_standalone_finalize(void);
uint32_t xg_render_submission_standalone_failure_detail(void);
bool xg_render_submission_stage_standalone_semantic_identified(
    const GpuRenderSemantic *semantic, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id);
bool xg_render_submission_stage_standalone_primitive_identified(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id);
bool xg_render_submission_stage_standalone_primitive_deferred_anchors(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id);
bool xg_render_submission_stage_standalone_primitive_with_detail(
    const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id, uint32_t *failure_detail);

bool xg_render_submission_stage_temporal_primitive_identified(
    const XgRenderIrNativePrimitive *primitive,
    uint32_t interpolation_producer_id,
    uint32_t interpolation_primitive_id,
    const GpuRenderTemporalCullPolicy *policy);
bool xg_render_submission_cover_temporal_current(
    const GpuRenderSemantic *semantic);
bool xg_render_submission_finalize_temporal(void);
bool xg_render_submission_complete_source_frame(void);

#endif
