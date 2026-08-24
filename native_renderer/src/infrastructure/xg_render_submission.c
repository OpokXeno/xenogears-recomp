#include "xg_render_submission.h"

#include "guest_render_bridge.h"
#include "guest_render_native_stream.h"
#include "xg_field_character_adapter.h"
#include "xg_render_backend.h"
#include "xg_render_instrumentation.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_temporal_submission.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static XgRenderPreSceneState pre_scene;
static XgRenderStandaloneSubmissionState standalone_submission;
static uint32_t standalone_stage_failure_detail;
static XgRenderSubmissionServices submission_services;
static bool submission_services_configured;
static XgRenderSubmissionObserver submission_observer;
static void *submission_observer_user_data;

static uint64_t interpolation_generation(void) {
    return submission_services_configured &&
            submission_services.interpolation_generation != NULL
        ? submission_services.interpolation_generation() : 0u;
}

static bool authenticated_ir_failure(uint32_t reason, uint64_t index,
                                     uint32_t packet_address,
                                     uint32_t status) {
    xg_render_instrumentation_record_flush_failure(
        reason, index, packet_address, status);
    return false;
}

bool xg_render_submission_validate_authenticated_ir(
        const XgRenderAuthenticatedIrAccess *access) {
    XgRenderAuthenticatedIrDescription description = {0};

    xg_render_instrumentation_record_flush_attempt();
    if (access == NULL || access->describe == NULL || access->item_get == NULL)
        return authenticated_ir_failure(1u, 0u, 0u, 0u);
    if (!access->describe(&description))
        return authenticated_ir_failure(2u, 0u, 0u, 0u);
    if (description.render_mode != GUEST_RENDER_RENDER_NATIVE)
        return authenticated_ir_failure(
            3u, 0u, 0u, (uint32_t)description.render_mode);
    for (size_t index = 0u; index < description.item_count; ++index) {
        XgRenderIrNativeItem item = {0};
        GpuRenderSemantic semantic;
        uint32_t packet_address;

        if (!access->item_get(index, &item))
            return authenticated_ir_failure(4u, index, 0u, 0u);
        packet_address = item.base.ordering.packet_guest_address &
            UINT32_C(0x001ffffc);
        if (xg_render_backend_translate_primitive(&item.native, &semantic) !=
                XG_RENDER_BACKEND_OK)
            return authenticated_ir_failure(
                5u, index, packet_address, 0u);
    }
    return true;
}

static void clear_standalone(void) {
    standalone_submission = (XgRenderStandaloneSubmissionState){0};
}

void xg_render_submission_configure(
        const XgRenderSubmissionServices *services) {
    submission_services = services != NULL
        ? *services : (XgRenderSubmissionServices){0};
    submission_services_configured = services != NULL;
}

void xg_render_submission_set_observer(
        XgRenderSubmissionObserver observer, void *user_data) {
    submission_observer = observer;
    submission_observer_user_data = user_data;
}

static void notify_primitive_staged(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t source_primitive_index) {
    if (submission_observer != NULL)
        submission_observer(
            primitive, source_primitive_index, submission_observer_user_data);
}

void xg_render_submission_pre_scene_clear(void) {
    /* count bounds every record access, so stale large primitive records are
     * unreachable after a lifecycle reset. */
    pre_scene.count = 0u;
    pre_scene.blocker = 0u;
    pre_scene.blocked = false;
}

void xg_render_submission_pre_scene_block(
        uint32_t blocker, bool preserve_existing_blocker) {
    pre_scene.blocked = true;
    if (!preserve_existing_blocker || pre_scene.blocker == 0u)
        pre_scene.blocker = blocker;
}

bool xg_render_submission_pre_scene_blocked(void) {
    return pre_scene.blocked;
}

bool xg_render_submission_pre_scene_available(uint32_t primitive_count) {
    return !pre_scene.blocked &&
        primitive_count <= XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY &&
        pre_scene.count <=
            XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY - primitive_count;
}

uint32_t xg_render_submission_pre_scene_count(void) {
    return pre_scene.count;
}

uint32_t xg_render_submission_pre_scene_blocker(void) {
    return pre_scene.blocker;
}

bool xg_render_submission_pre_scene_item_copy(
        uint32_t index, XgRenderPreScenePrimitive *out_record) {
    if (out_record == NULL || index >= pre_scene.count) return false;
    *out_record = pre_scene.records[index];
    return true;
}

bool xg_render_submission_pre_scene_stage(
        const XgRenderPreScenePrimitive *record) {
    const uint32_t packet_address = record != NULL
        ? record->packet_address & UINT32_C(0x001ffffc) : 0u;

    if (record == NULL || pre_scene.blocked) return false;
    for (uint32_t index = 0u; index < pre_scene.count; ++index) {
        if ((record->temporal_only && pre_scene.records[index].temporal_only &&
             pre_scene.records[index].interpolation_producer_id ==
                 record->interpolation_producer_id &&
             pre_scene.records[index].interpolation_primitive_id ==
                 record->interpolation_primitive_id) ||
            (!record->temporal_only &&
             !pre_scene.records[index].temporal_only &&
             (pre_scene.records[index].packet_address &
              UINT32_C(0x001ffffc)) == packet_address)) {
            pre_scene.records[index] = *record;
            return true;
        }
    }
    if (pre_scene.count == XG_RENDER_PRE_SCENE_PRIMITIVE_CAPACITY)
        return false;
    pre_scene.records[pre_scene.count++] = *record;
    return true;
}

GuestRenderTransactionStatus xg_render_submission_stage_exact(
        GpuRenderTransactionId visual_id, uint64_t exact_command_id,
        const GpuRenderSemantic *semantic) {
    GuestRenderNativeStreamStatus status;

    if (semantic == NULL)
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    if (!guest_render_native_stream_enabled())
        return guest_render_transaction_stage_exact(
            visual_id, exact_command_id, semantic);

    status = guest_render_native_stream_stage_exact(
        visual_id, exact_command_id, semantic);
    switch (status) {
    case GUEST_RENDER_NATIVE_STREAM_OK:
        return GUEST_RENDER_TRANSACTION_OK;
    case GUEST_RENDER_NATIVE_STREAM_INVALID_ARGUMENT:
        return GUEST_RENDER_TRANSACTION_INVALID_ARGUMENT;
    case GUEST_RENDER_NATIVE_STREAM_CAPACITY_EXCEEDED:
        return GUEST_RENDER_TRANSACTION_CAPACITY_EXCEEDED;
    case GUEST_RENDER_NATIVE_STREAM_DUPLICATE_COMMAND:
        return GUEST_RENDER_TRANSACTION_DUPLICATE_TARGET;
    case GUEST_RENDER_NATIVE_STREAM_STALE_VISUAL_ID:
        return GUEST_RENDER_TRANSACTION_STALE_VISUAL_ID;
    case GUEST_RENDER_NATIVE_STREAM_DISABLED:
    case GUEST_RENDER_NATIVE_STREAM_NOT_FOUND:
    default:
        return GUEST_RENDER_TRANSACTION_INVALID_TRANSITION;
    }
}

static bool record_interpolation_anchors(const GpuRenderSemantic *semantic) {
    GpuRenderInterpolationVertexAnchor anchors[
        GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY * 3u];
    size_t anchor_count = 0u;

    for (uint32_t triangle = 0u; triangle < semantic->triangle_count;
         ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const GpuRenderSemanticVertex *candidate =
                &semantic->triangles[triangle].vertices[vertex];
            bool duplicate = false;

            if (!candidate->interpolation_vertex_identity_valid) continue;
            for (size_t prior = 0u; prior < anchor_count; ++prior) {
                duplicate |= anchors[prior].vertex.interpolation_group_id ==
                        candidate->interpolation_group_id &&
                    anchors[prior].vertex.interpolation_vertex_id ==
                        candidate->interpolation_vertex_id;
            }
            if (duplicate) continue;
            anchors[anchor_count++] = (GpuRenderInterpolationVertexAnchor){
                .scene_id = semantic->interpolation_identity.scene_id,
                .producer_id = semantic->interpolation_identity.producer_id,
                .material = semantic->material,
                .vertex = *candidate,
            };
        }
    }
    return anchor_count == 0u ||
        gr_record_interpolation_anchors(anchors, anchor_count) ==
            GPU_RENDER_TRANSACTION_OK;
}

bool xg_render_submission_pre_scene_flush(void) {
    GpuRenderTransactionId visual_id;

    if (pre_scene.blocked || !submission_services_configured ||
        submission_services.active_auth_snapshot == NULL ||
        submission_services.active_auth_append == NULL ||
        !submission_services.active_auth_snapshot(&visual_id))
        return false;

    for (uint32_t index = 0u; index < pre_scene.count; ++index) {
        const XgRenderPreScenePrimitive *record = &pre_scene.records[index];
        GpuRenderSemantic semantic;

        if (xg_render_backend_translate_primitive(
                &record->primitive, &semantic) != XG_RENDER_BACKEND_OK) {
            pre_scene.blocker = 7u;
            return false;
        }
        if (record->interpolation_identity_valid)
            xg_render_semantic_set_interpolation_identity(
                &semantic, interpolation_generation(),
                record->interpolation_producer_id,
                record->interpolation_primitive_id);
        if (record->interpolation_identity_valid &&
            !record_interpolation_anchors(&semantic)) {
            pre_scene.blocker = 8u;
            return false;
        }
        if (record->temporal_only) {
            if (!record->interpolation_identity_valid ||
                !xg_render_temporal_submission_stage(
                    &semantic, &record->temporal_cull)) {
                pre_scene.blocker = 10u;
                return false;
            }
            continue;
        }
        uint32_t append_detail = 0u;
        if (!submission_services.active_auth_append(
                record->packet_address & UINT32_C(0x001ffffc),
                record->source_primitive_index, record->ot_bucket,
                record->payload_word_count, &record->primitive,
                true,
                &append_detail)) {
            pre_scene.blocker = 11u + append_detail;
            return false;
        }
        if (xg_render_submission_stage_exact(
                visual_id,
                (record->packet_address & UINT32_C(0x001ffffc)) + 4u,
                &semantic) != GUEST_RENDER_TRANSACTION_OK) {
            pre_scene.blocker = 12u;
            return false;
        }
    }
    xg_render_submission_pre_scene_clear();
    return true;
}

bool xg_render_submission_stage_active_primitive(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        uint8_t payload_word_count, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_blocker) {
    GuestRenderBridgeSnapshot bridge = {0};
    GpuRenderSemantic semantic;
    GpuRenderTransactionId visual_id;

    if (failure_blocker != NULL) *failure_blocker = 0u;
    if (primitive == NULL || !submission_services_configured ||
        submission_services.active_auth_available == NULL ||
        !submission_services.active_auth_available()) {
        if (failure_blocker != NULL) *failure_blocker = 58u;
        return false;
    }
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK) {
        if (failure_blocker != NULL) *failure_blocker = 59u;
        return false;
    }
    if (bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE) {
        if (failure_blocker != NULL) *failure_blocker = 60u;
        return false;
    }
    if (submission_services.active_auth_snapshot == NULL ||
        !submission_services.active_auth_snapshot(&visual_id)) {
        if (failure_blocker != NULL) *failure_blocker = 61u;
        return false;
    }
    if (xg_render_backend_translate_primitive(primitive, &semantic) !=
            XG_RENDER_BACKEND_OK) {
        if (failure_blocker != NULL) *failure_blocker = 62u;
        return false;
    }
    xg_render_semantic_set_interpolation_identity(
        &semantic, interpolation_generation(),
        interpolation_producer_id, interpolation_primitive_id);
    if (submission_services.active_auth_append == NULL ||
        !submission_services.active_auth_append(
            packet_address & UINT32_C(0x001ffffc), source_primitive_index,
            ot_bucket, payload_word_count, primitive, false, NULL)) {
        if (failure_blocker != NULL) *failure_blocker = 63u;
        return false;
    }
    if (xg_render_submission_stage_exact(
            visual_id, (packet_address & UINT32_C(0x001ffffc)) + 4u,
            &semantic) != GUEST_RENDER_TRANSACTION_OK) {
        if (failure_blocker != NULL) *failure_blocker = 64u;
        return false;
    }
    notify_primitive_staged(primitive, source_primitive_index);
    return true;
}

XgRenderFieldCharacterStageResult
xg_render_submission_stage_field_character(
        const XgRenderIrNativePrimitive *primitive,
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t ot_bucket,
        GpuRenderTransactionId visual_id) {
    if (!submission_services_configured || primitive == NULL ||
        semantic == NULL || submission_services.active_auth_append == NULL ||
        !submission_services.active_auth_append(
            packet_address, source_primitive_index, ot_bucket,
            XG_FIELD_CHARACTER_PACKET_WORD_COUNT, primitive, false, NULL))
        return XG_RENDER_FIELD_CHARACTER_STAGE_AUTH_FAILED;
    if (xg_render_submission_stage_exact(
            visual_id, packet_address + 4u, semantic) !=
        GUEST_RENDER_TRANSACTION_OK)
        return XG_RENDER_FIELD_CHARACTER_STAGE_SUBMISSION_FAILED;
    return XG_RENDER_FIELD_CHARACTER_STAGE_OK;
}

bool xg_render_submission_standalone_open(void) {
    return standalone_submission.open;
}

static bool begin_standalone_scene(void) {
    GuestRenderSceneConfig config;
    GuestRenderTransactionPendingSnapshot pending = {0};
    GuestRenderTransactionSnapshot transaction = {0};

    if (!submission_services_configured ||
        submission_services.standalone_scene_config == NULL ||
        !submission_services.standalone_scene_config(&config) ||
        guest_render_transaction_pending_snapshot(&pending) !=
            GUEST_RENDER_TRANSACTION_OK ||
        pending.binding_count != 0u ||
        guest_render_transaction_snapshot(&transaction) !=
            GUEST_RENDER_TRANSACTION_OK ||
        transaction.phase == GUEST_RENDER_TRANSACTION_ACTIVE ||
        guest_render_bridge_begin_scene(&config) != GUEST_RENDER_OK)
        return false;
    if (submission_services.presentation_gate == NULL ||
        !submission_services.presentation_gate())
        guest_render_bridge_force_original(
            GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    return true;
}

bool xg_render_submission_standalone_begin(void) {
    const GuestRenderProducerProvenance provenance = {
        GUEST_RENDER_PRODUCER_NATIVE,
        {0},
    };
    GuestRenderBridgeSnapshot bridge = {0};

    if (standalone_submission.open) return true;
    if (!begin_standalone_scene())
        return false;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK ||
        bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE ||
        guest_render_bridge_begin_state(&standalone_submission.visual_id) !=
            GUEST_RENDER_OK ||
        guest_render_bridge_producer_begin(
            standalone_submission.visual_id, &provenance,
            &standalone_submission.producer) != GUEST_RENDER_OK) {
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
        clear_standalone();
        return false;
    }
    standalone_submission.open = true;
    return true;
}

void xg_render_submission_standalone_abort(void) {
    if (standalone_submission.open) {
        guest_render_native_stream_abandon_visual(
            (GpuRenderTransactionId){
                standalone_submission.visual_id.scene_epoch,
                standalone_submission.visual_id.state_sequence,
            });
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
        guest_render_transaction_clear_pending();
        clear_standalone();
    }
    xg_render_temporal_submission_reset();
}

bool xg_render_submission_standalone_finalize(void) {
    GuestRenderProducerSlot slot = {0};
    GuestRenderCompletedState completed = {0};
    GuestRenderBridgeSnapshot completed_snapshot = {0};

    if (!standalone_submission.open) {
        if (guest_render_bridge_last_completed(
                &completed_snapshot, &completed) == GUEST_RENDER_OK &&
            completed.binding_count != 0u &&
            guest_render_native_stream_has_staged_predecessor(
                (GpuRenderTransactionId){
                    completed.id.scene_epoch, completed.id.state_sequence,
                }))
            (void)guest_render_native_stream_activate_visual(
                (GpuRenderTransactionId){
                    completed.id.scene_epoch, completed.id.state_sequence,
                });
        return true;
    }
    if (guest_render_bridge_producer_end(
            standalone_submission.producer, &slot) != GUEST_RENDER_OK ||
        !guest_render_bridge_id_equal(
            slot.handle.state_id, standalone_submission.visual_id) ||
        guest_render_bridge_finalize_state(
            standalone_submission.visual_id, &completed) != GUEST_RENDER_OK ||
        !guest_render_bridge_id_equal(
            completed.id, standalone_submission.visual_id)) {
        xg_render_submission_standalone_abort();
        return false;
    }
    if (completed.binding_count == 0u) {
        xg_render_submission_standalone_abort();
        return true;
    }
    if (guest_render_native_stream_enabled() &&
        guest_render_native_stream_activate_visual(
            (GpuRenderTransactionId){
                completed.id.scene_epoch, completed.id.state_sequence,
            }) != GUEST_RENDER_NATIVE_STREAM_OK) {
        xg_render_submission_standalone_abort();
        return false;
    }
    clear_standalone();
    return true;
}

static bool stage_standalone_semantic(
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index) {
    GuestRenderStatus bind_status;
    GuestRenderTransactionStatus stage_status;

    standalone_stage_failure_detail = 0u;
    if (semantic == NULL) {
        standalone_stage_failure_detail = 1u;
        return false;
    }
    if (!xg_render_submission_standalone_begin()) {
        standalone_stage_failure_detail = 2u;
        return false;
    }
    bind_status = guest_render_bridge_bind_packet(
        standalone_submission.producer,
        packet_address & UINT32_C(0x001ffffc), source_primitive_index);
    if (bind_status != GUEST_RENDER_OK) {
        standalone_stage_failure_detail = 100u + (uint32_t)bind_status;
        xg_render_submission_standalone_abort();
        return false;
    }
    stage_status = xg_render_submission_stage_exact(
        (GpuRenderTransactionId){
            standalone_submission.visual_id.scene_epoch,
            standalone_submission.visual_id.state_sequence,
        },
        (packet_address & UINT32_C(0x001ffffc)) + 4u, semantic);
    if (stage_status != GUEST_RENDER_TRANSACTION_OK) {
        standalone_stage_failure_detail = 200u + (uint32_t)stage_status;
        xg_render_submission_standalone_abort();
        return false;
    }
    return true;
}

uint32_t xg_render_submission_standalone_failure_detail(void) {
    return standalone_stage_failure_detail;
}

bool xg_render_submission_stage_standalone_semantic_identified(
        const GpuRenderSemantic *semantic, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id) {
    GpuRenderSemantic identified;

    if (semantic == NULL) return false;
    identified = *semantic;
    xg_render_semantic_set_interpolation_identity(
        &identified, interpolation_generation(),
        interpolation_producer_id, interpolation_primitive_id);
    return stage_standalone_semantic(
        &identified, packet_address, source_primitive_index);
}

bool xg_render_submission_stage_standalone_primitive_identified(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id) {
    GpuRenderSemantic semantic;
    XgRenderBackendStatus translate_status;

    if (primitive == NULL) {
        standalone_stage_failure_detail = 1u;
        return false;
    }
    translate_status = xg_render_backend_translate_primitive(
        primitive, &semantic);
    if (translate_status != XG_RENDER_BACKEND_OK) {
        standalone_stage_failure_detail = 300u + (uint32_t)translate_status;
        return false;
    }
    xg_render_semantic_set_corner_identities(
        &semantic, interpolation_producer_id, interpolation_primitive_id);
    if (!xg_render_submission_stage_standalone_semantic_identified(
            &semantic, packet_address, source_primitive_index,
            interpolation_producer_id, interpolation_primitive_id))
        return false;
    notify_primitive_staged(primitive, source_primitive_index);
    return true;
}

bool xg_render_submission_stage_standalone_primitive_with_detail(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id, uint32_t *failure_detail) {
    const bool staged =
        xg_render_submission_stage_standalone_primitive_identified(
            primitive, packet_address, source_primitive_index,
            interpolation_producer_id, interpolation_primitive_id);

    if (failure_detail != NULL)
        *failure_detail = xg_render_submission_standalone_failure_detail();
    return staged;
}

bool xg_render_submission_stage_temporal_primitive_identified(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy) {
    GpuRenderSemantic semantic;

    if (primitive == NULL || policy == NULL ||
        xg_render_backend_translate_primitive(primitive, &semantic) !=
            XG_RENDER_BACKEND_OK)
        return false;
    xg_render_semantic_set_interpolation_identity(
        &semantic, interpolation_generation(),
        interpolation_producer_id, interpolation_primitive_id);
    xg_render_semantic_set_corner_identities(
        &semantic, interpolation_producer_id, interpolation_primitive_id);
    return xg_render_temporal_submission_stage(&semantic, policy);
}

bool xg_render_submission_cover_temporal_current(
        const GpuRenderSemantic *semantic) {
    return xg_render_temporal_submission_cover_current(semantic);
}

bool xg_render_submission_finalize_temporal(void) {
    return xg_render_temporal_submission_flush();
}

void xg_render_submission_reset(void) {
    xg_render_submission_pre_scene_clear();
    clear_standalone();
    standalone_stage_failure_detail = 0u;
    xg_render_temporal_submission_reset();
}

void xg_render_submission_reset_transaction(void) {
    GuestRenderTransactionSnapshot snapshot = {0};

    if (guest_render_native_stream_enabled()) {
        GpuRenderTransactionId visual_id;

        if (submission_services_configured &&
            submission_services.active_auth_available != NULL &&
            submission_services.active_auth_available() &&
            submission_services.active_auth_snapshot != NULL &&
            submission_services.active_auth_snapshot(&visual_id))
            guest_render_native_stream_abandon_visual(visual_id);
        return;
    }
    if (guest_render_transaction_snapshot(&snapshot) ==
            GUEST_RENDER_TRANSACTION_OK &&
        snapshot.phase == GUEST_RENDER_TRANSACTION_ACTIVE)
        (void)guest_render_transaction_abort_before_observation(
            GUEST_RENDER_TRANSACTION_OBSERVATION_CALLER_ABORT);
    guest_render_transaction_invalidate_deferred();
    guest_render_transaction_clear_pending();
}

void xg_render_submission_reject_producer(void) {
    guest_render_transaction_invalidate_deferred();
    guest_render_transaction_clear_pending();
}

void xg_render_submission_disarm(void) {
    xg_render_submission_reset_transaction();
    xg_render_submission_pre_scene_clear();
    xg_render_submission_standalone_abort();
}

void xg_render_submission_prepare_authenticated_scene(void) {
    if (xg_render_submission_standalone_open())
        xg_render_submission_standalone_abort();
    /* A rejected cutover cannot be flushed into the new authenticated
     * producer, while valid pre-scene records remain eligible. */
    if (xg_render_submission_pre_scene_blocked())
        xg_render_submission_pre_scene_clear();
    xg_render_submission_reset_transaction();
    /* Packet addresses are reusable guest command sources. Preserve stream
     * allocation, but start each authenticated scene with no occupied IDs. */
    guest_render_native_stream_clear();
}

void xg_render_submission_scene_boundary(void) {
    guest_render_native_stream_clear();
    xg_render_submission_reset_transaction();
    xg_render_submission_pre_scene_clear();
    xg_render_submission_standalone_abort();
}

void xg_render_submission_source_reset(void) {
    xg_render_submission_pre_scene_clear();
}

void xg_render_submission_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.watched_range_mutation ||
            event->mutation.artifact_mutation)
            xg_render_submission_standalone_abort();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_render_submission_scene_boundary();
    } else if (event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST) {
        xg_render_submission_disarm();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_submission_reset_transaction();
        xg_render_submission_reset();
    }
}
