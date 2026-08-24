#ifndef XG_RENDER_SUBMISSION_H
#define XG_RENDER_SUBMISSION_H

#include "xg_render_invalidation_event.h"

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

typedef enum XgRenderFieldCharacterStageResult {
    XG_RENDER_FIELD_CHARACTER_STAGE_OK = 0,
    XG_RENDER_FIELD_CHARACTER_STAGE_AUTH_FAILED,
    XG_RENDER_FIELD_CHARACTER_STAGE_SUBMISSION_FAILED,
} XgRenderFieldCharacterStageResult;

typedef struct XgRenderSubmissionServices {
    bool (*active_auth_available)(void);
    bool (*active_auth_snapshot)(GpuRenderTransactionId *out_visual_id);
    bool (*active_auth_append)(
        uint32_t packet_address, uint32_t source_primitive_index,
        uint32_t ot_bucket, uint8_t payload_word_count,
        const XgRenderIrNativePrimitive *primitive,
        bool force_pending_capture,
        uint32_t *out_failure_detail);
    bool (*standalone_scene_config)(GuestRenderSceneConfig *out_config);
    bool (*presentation_gate)(void);
    uint64_t (*interpolation_generation)(void);
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
void xg_render_submission_set_observer(
    XgRenderSubmissionObserver observer, void *user_data);
void xg_render_submission_reset(void);
void xg_render_submission_reset_transaction(void);
void xg_render_submission_reject_producer(void);
void xg_render_submission_disarm(void);
void xg_render_submission_prepare_authenticated_scene(void);
void xg_render_submission_scene_boundary(void);
void xg_render_submission_source_reset(void);

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

#endif
