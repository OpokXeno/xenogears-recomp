#include "xg_render_auth.h"

#include "xg_render_ir.h"

#include <string.h>

/* allow: SIZE_OK - one bounded process-owned authentication state machine. */

#if XG_RENDER_AUTH_TRACE_CAPACITY == 0
#error "XG_RENDER_AUTH_TRACE_CAPACITY must be nonzero"
#endif

typedef enum XgRenderAuthPhase {
    XG_RENDER_AUTH_PHASE_IDLE = 0,
    XG_RENDER_AUTH_PHASE_EXPECT_ENTRY,
    XG_RENDER_AUTH_PHASE_EXPECT_CAPTURE,
    XG_RENDER_AUTH_PHASE_EXPECT_RETURN,
    XG_RENDER_AUTH_PHASE_FINALIZED,
    XG_RENDER_AUTH_PHASE_REJECTED,
} XgRenderAuthPhase;

typedef struct XgRenderAuthTraceOutcome {
    XgRenderAuthReason reason;
    XgRenderAuthEventMode event_mode;
} XgRenderAuthTraceOutcome;

typedef struct XgRenderAuthRejection {
    XgRenderAuthReason reason;
    XgRenderAuthDecision *decision;
} XgRenderAuthRejection;

struct XgRenderAuth {
    XgRenderAuthProfile profile;
    XgRenderAuthLogicalIdentity logical_identity;
    XgRenderAuthTraceEvent trace[XG_RENDER_AUTH_TRACE_CAPACITY];
    XgRenderIr *ir;
    GuestRenderProducerHandle producer_handle;
    uint64_t next_trace_sequence;
    uint64_t minimum_next_scene_epoch;
    size_t trace_write_index;
    size_t trace_count;
    size_t producer_begin_count;
    size_t native_item_count;
    size_t hook_count;
    XgRenderAuthHook hook_sequence[XG_RENDER_AUTH_HOOK_STAGE_COUNT];
    XgRenderAuthTier tier;
    XgRenderAuthPhase phase;
    XgRenderAuthReason reject_reason;
    GuestRenderRenderMode effective_render_mode;
    bool producer_open;
    bool scene_aborted;
    bool ir_usable;
    bool native_use_permitted;
    bool trace_sequence_exhausted;
};

static XgRenderAuth process_owner = {
    .next_trace_sequence = 1u,
};

static bool is_owner(const XgRenderAuth *auth) {
    return auth == &process_owner;
}

static bool digest_equal(const XgRenderAuthDigest *left,
                         const XgRenderAuthDigest *right) {
    return memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool digest_present(const XgRenderAuthDigest *digest) {
    static const XgRenderAuthDigest zero_digest = { { 0 } };

    return !digest_equal(digest, &zero_digest);
}

static bool identity_equal(const XgRenderAuthIdentity *left,
                           const XgRenderAuthIdentity *right) {
    return left->namespace_id == right->namespace_id &&
           digest_equal(&left->full_sha256, &right->full_sha256);
}

static bool identity_present(const XgRenderAuthIdentity *identity) {
    return digest_present(&identity->full_sha256);
}

static bool field_identity_is_distinct(
    const XgRenderAuthIdentity *field_identity,
    const XgRenderAuthIdentity *game_identity) {
    return !digest_equal(&field_identity->full_sha256,
                         &game_identity->full_sha256);
}

static bool tier_valid(XgRenderAuthTier tier) {
    return tier >= XG_RENDER_AUTH_TIER_STATIC &&
           tier <= XG_RENDER_AUTH_TIER_WARM_NATIVE;
}

static bool validation_shape_is_valid(const XgRenderAuthValidation *validation) {
    uint64_t window_end;
    uint64_t return_end;

    if (validation->code_page_generation == 0u ||
        !digest_present(&validation->instruction_window_digest) ||
        validation->instruction_window_start == 0u ||
        validation->instruction_window_size < 8u ||
        validation->caller_site == 0u || validation->callee_entry == 0u ||
        validation->return_site == 0u || validation->jal_target == 0u ||
        validation->required_jal_opcode != 3u ||
        validation->required_delay_slot_instructions != 1u ||
        !validation->delay_slot_complete ||
        validation->delay_slot_is_control_transfer ||
        !digest_present(&validation->manifest_digest) ||
        (validation->instruction_window_start & 3u) != 0u ||
        (validation->caller_site & 3u) != 0u ||
        (validation->callee_entry & 3u) != 0u ||
        (validation->return_site & 3u) != 0u ||
        validation->jal_target != validation->callee_entry)
        return false;
    window_end = (uint64_t)validation->instruction_window_start +
                 validation->instruction_window_size;
    return_end = (uint64_t)validation->caller_site + 8u;
    return return_end <= UINT32_MAX &&
           validation->return_site == (uint32_t)return_end &&
           validation->caller_site >= validation->instruction_window_start &&
           return_end <= window_end;
}

static bool validation_equal(const XgRenderAuthValidation *left,
                             const XgRenderAuthValidation *right) {
    return left->base_crc32 == right->base_crc32 &&
           left->range_crc32 == right->range_crc32 &&
           left->code_page_generation == right->code_page_generation &&
           digest_equal(&left->instruction_window_digest,
                        &right->instruction_window_digest) &&
           left->instruction_window_start == right->instruction_window_start &&
           left->instruction_window_size == right->instruction_window_size &&
           left->caller_site == right->caller_site &&
           left->callee_entry == right->callee_entry &&
           left->return_site == right->return_site &&
           left->required_jal_opcode == right->required_jal_opcode &&
           left->jal_target == right->jal_target &&
           left->required_delay_slot_instructions ==
               right->required_delay_slot_instructions &&
           left->delay_slot_complete == right->delay_slot_complete &&
           left->delay_slot_is_control_transfer ==
               right->delay_slot_is_control_transfer &&
           digest_equal(&left->manifest_digest, &right->manifest_digest);
}

static void clear_scene(XgRenderAuth *auth) {
    memset(&auth->profile, 0, sizeof(auth->profile));
    memset(&auth->logical_identity, 0, sizeof(auth->logical_identity));
    memset(&auth->producer_handle, 0, sizeof(auth->producer_handle));
    memset(auth->hook_sequence, 0, sizeof(auth->hook_sequence));
    auth->producer_begin_count = 0u;
    auth->native_item_count = 0u;
    auth->hook_count = 0u;
    auth->tier = XG_RENDER_AUTH_TIER_STATIC;
    auth->phase = XG_RENDER_AUTH_PHASE_IDLE;
    auth->reject_reason = XG_RENDER_AUTH_REJECT_NONE;
    auth->effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    auth->producer_open = false;
    auth->scene_aborted = false;
    auth->ir_usable = false;
    auth->native_use_permitted = false;
}

static void record_event(XgRenderAuth *auth,
                         const XgRenderAuthExecution *execution,
                         XgRenderAuthTraceOutcome outcome) {
    XgRenderAuthTraceEvent *event = &auth->trace[auth->trace_write_index];

    if (auth->trace_sequence_exhausted) return;
    memset(event, 0, sizeof(*event));
    event->sequence = auth->next_trace_sequence;
    event->state_id = auth->logical_identity.state_id;
    event->producer_record_id = auth->logical_identity.producer_record_id;
    event->site_record_id = auth->logical_identity.site_record_id;
    event->producer_entry = auth->logical_identity.producer_entry;
    event->capture_site = auth->logical_identity.capture_site;
    event->return_site = auth->logical_identity.return_site;
    event->static_callee = auth->logical_identity.static_callee;
    event->tier = execution->tier;
    event->hook = execution->hook;
    event->reason = outcome.reason;
    event->event_mode = outcome.event_mode;
    event->effective_render_mode = auth->effective_render_mode;
    auth->trace_write_index =
        (auth->trace_write_index + 1u) % XG_RENDER_AUTH_TRACE_CAPACITY;
    if (auth->trace_count < XG_RENDER_AUTH_TRACE_CAPACITY) ++auth->trace_count;
    if (auth->next_trace_sequence == UINT64_MAX)
        auth->trace_sequence_exhausted = true;
    else
        ++auth->next_trace_sequence;
}

static XgRenderAuthReason profile_reason(const XgRenderAuthProfile *profile) {
    if (profile->producer_record_id == 0u || profile->site_record_id == 0u ||
        profile->producer_entry == 0u)
        return XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH;
    if (!identity_present(&profile->static_game_identity) ||
        !identity_present(&profile->field_image_identity) ||
        !field_identity_is_distinct(&profile->field_image_identity,
                                    &profile->static_game_identity))
        return XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH;
    if (!validation_shape_is_valid(&profile->validation))
        return XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH;
    if (!digest_present(&profile->cache_identity.codegen_digest) ||
        !digest_present(&profile->cache_identity.manifest_digest) ||
        !digest_equal(&profile->validation.manifest_digest,
                      &profile->cache_identity.manifest_digest))
        return XG_RENDER_AUTH_REJECT_CACHE_IDENTITY_MISMATCH;
    return XG_RENDER_AUTH_REJECT_NONE;
}

static XgRenderAuthReason execution_reason(
    const XgRenderAuth *auth,
    const XgRenderAuthExecution *execution) {
    const XgRenderAuthProfile *profile = &auth->profile;

    if (execution->observed_producer_entry == 0u ||
        execution->observed_producer_entry != profile->producer_entry)
        return XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH;
    if (!identity_present(&execution->static_game_identity) ||
        !identity_present(&execution->runtime_game_identity) ||
        !identity_present(&execution->field_image_identity) ||
        !identity_equal(&execution->static_game_identity,
                        &profile->static_game_identity) ||
        !identity_equal(&execution->runtime_game_identity,
                        &profile->static_game_identity) ||
        !identity_equal(&execution->field_image_identity,
                        &profile->field_image_identity) ||
        !field_identity_is_distinct(&execution->field_image_identity,
                                    &execution->static_game_identity) ||
        !field_identity_is_distinct(&execution->field_image_identity,
                                    &execution->runtime_game_identity))
        return XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH;
    if (!digest_present(&execution->cache_identity.codegen_digest) ||
        !digest_present(&execution->cache_identity.manifest_digest) ||
        !digest_equal(&execution->cache_identity.codegen_digest,
                      &profile->cache_identity.codegen_digest) ||
        !digest_equal(&execution->cache_identity.manifest_digest,
                      &profile->cache_identity.manifest_digest))
        return XG_RENDER_AUTH_REJECT_CACHE_IDENTITY_MISMATCH;
    if (!validation_shape_is_valid(&execution->validation) ||
        !validation_equal(&execution->validation, &profile->validation))
        return XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH;
    return XG_RENDER_AUTH_REJECT_NONE;
}

static void abort_transaction(XgRenderAuth *auth, XgRenderAuthReason reason) {
    if (auth->reject_reason == XG_RENDER_AUTH_REJECT_NONE)
        auth->reject_reason = reason;
    auth->phase = XG_RENDER_AUTH_PHASE_REJECTED;
    auth->effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    auth->native_item_count = 0u;
    auth->producer_open = false;
    auth->scene_aborted = true;
    auth->ir_usable = false;
    auth->native_use_permitted = false;
    if (auth->logical_identity.state_id.scene_epoch >
        auth->minimum_next_scene_epoch)
        auth->minimum_next_scene_epoch = auth->logical_identity.state_id.scene_epoch;
    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    if (auth->ir != NULL) (void)xg_render_ir_reset(auth->ir);
}

static void set_decision(const XgRenderAuth *auth,
                         XgRenderAuthDecision *decision) {
    decision->effective_render_mode = auth->effective_render_mode;
    decision->reject_reason = auth->reject_reason;
    decision->native_use_permitted = auth->native_use_permitted;
}

static XgRenderAuthResult reject_hook(XgRenderAuth *auth,
                                      const XgRenderAuthExecution *execution,
                                      XgRenderAuthRejection rejection) {
    abort_transaction(auth, rejection.reason);
    record_event(auth, execution, (XgRenderAuthTraceOutcome){
        auth->reject_reason,
        XG_RENDER_AUTH_EVENT_REJECTED_HOOK,
    });
    set_decision(auth, rejection.decision);
    return XG_RENDER_AUTH_REJECTED;
}

static XgRenderAuthResult begin_transaction(XgRenderAuth *auth,
                                             const XgRenderAuthExecution *execution,
                                             XgRenderAuthDecision *decision) {
    GuestRenderProducerProvenance provenance = {
        GUEST_RENDER_PRODUCER_NATIVE,
        { 0 },
    };
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };

    if (guest_render_bridge_snapshot(&bridge_snapshot) != GUEST_RENDER_OK)
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
            decision,
        });
    if (bridge_snapshot.modes.effective_render_mode !=
        GUEST_RENDER_RENDER_NATIVE)
        provenance.tier = GUEST_RENDER_PRODUCER_SHADOW;

    if (xg_render_ir_begin(auth->ir, auth->logical_identity.state_id,
                           execution->validation.code_page_generation) !=
        XG_RENDER_IR_OK)
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
            decision,
        });
    if (guest_render_bridge_producer_begin(auth->logical_identity.state_id,
                                           &provenance,
                                           &auth->producer_handle) !=
        GUEST_RENDER_OK)
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
            decision,
        });
    auth->producer_open = true;
    ++auth->producer_begin_count;
    return XG_RENDER_AUTH_OK;
}

static XgRenderAuthResult finalize_transaction(XgRenderAuth *auth,
                                               const XgRenderAuthExecution *execution,
                                               XgRenderAuthDecision *decision) {
    GuestRenderProducerSlot slot = { 0 };
    GuestRenderCompletedState completed = { 0 };
    XgRenderIrSnapshot ir_snapshot = { 0 };
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };

    if (!auth->producer_open ||
        guest_render_bridge_producer_end(auth->producer_handle, &slot) !=
            GUEST_RENDER_OK ||
        !guest_render_bridge_id_equal(slot.handle.state_id,
                                      auth->logical_identity.state_id) ||
        slot.handle.slot_index != auth->producer_handle.slot_index)
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
            decision,
        });
    auth->producer_open = false;
    if (xg_render_ir_finalize(auth->ir, auth->logical_identity.state_id,
                               execution->validation.code_page_generation) !=
            XG_RENDER_IR_OK ||
        xg_render_ir_snapshot(auth->ir, &ir_snapshot) != XG_RENDER_IR_OK ||
        !ir_snapshot.usable ||
        guest_render_bridge_finalize_state(auth->logical_identity.state_id,
                                            &completed) != GUEST_RENDER_OK ||
        guest_render_bridge_snapshot(&bridge_snapshot) != GUEST_RENDER_OK ||
        !guest_render_bridge_id_equal(completed.id,
                                      auth->logical_identity.state_id) ||
        completed.slot_count != auth->producer_begin_count ||
        slot.binding_start != 0u ||
        slot.binding_count != completed.binding_count)
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
            decision,
        });
    auth->native_item_count = completed.binding_count;
    auth->ir_usable = true;
    auth->native_use_permitted =
        bridge_snapshot.modes.effective_render_mode ==
            GUEST_RENDER_RENDER_NATIVE &&
        completed.binding_count != 0u;
    if (bridge_snapshot.modes.effective_render_mode ==
        GUEST_RENDER_RENDER_NATIVE) {
        auth->effective_render_mode = GUEST_RENDER_RENDER_NATIVE;
    } else if (bridge_snapshot.modes.effective_render_mode ==
               GUEST_RENDER_RENDER_SHADOW) {
        auth->effective_render_mode = GUEST_RENDER_RENDER_SHADOW;
    } else {
        auth->effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    }
    auth->phase = XG_RENDER_AUTH_PHASE_FINALIZED;
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_process_owner(XgRenderAuth **out_auth) {
    if (out_auth == NULL) return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (process_owner.ir == NULL) {
        if (xg_render_ir_process_owner(&process_owner.ir) != XG_RENDER_IR_OK)
            return XG_RENDER_AUTH_INVALID_TRANSITION;
    }
    *out_auth = &process_owner;
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_scene_begin(
    XgRenderAuth *auth,
    GuestRenderVisualStateId state_id,
    const XgRenderAuthProfile *profile) {
    XgRenderAuthReason reason;

    if (!is_owner(auth) || profile == NULL || state_id.scene_epoch == 0u)
        return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (state_id.scene_epoch <= auth->minimum_next_scene_epoch)
        return XG_RENDER_AUTH_INVALID_TRANSITION;
    if (auth->phase != XG_RENDER_AUTH_PHASE_IDLE &&
        auth->phase != XG_RENDER_AUTH_PHASE_REJECTED)
        return XG_RENDER_AUTH_INVALID_TRANSITION;
    clear_scene(auth);
    auth->profile = *profile;
    auth->logical_identity.producer_record_id = profile->producer_record_id;
    auth->logical_identity.site_record_id = profile->site_record_id;
    auth->logical_identity.producer_entry = profile->producer_entry;
    auth->logical_identity.capture_site = profile->validation.caller_site;
    auth->logical_identity.return_site = profile->validation.return_site;
    auth->logical_identity.static_callee = profile->validation.callee_entry;
    auth->logical_identity.field_image_identity = profile->field_image_identity;
    auth->logical_identity.manifest_identity = profile->validation.manifest_digest;
    auth->logical_identity.state_id = state_id;
    reason = profile_reason(profile);
    if (reason != XG_RENDER_AUTH_REJECT_NONE) {
        abort_transaction(auth, reason);
        return XG_RENDER_AUTH_REJECTED;
    }
    if (auth->ir == NULL || xg_render_ir_reset(auth->ir) != XG_RENDER_IR_OK) {
        abort_transaction(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE);
        return XG_RENDER_AUTH_REJECTED;
    }
    auth->phase = XG_RENDER_AUTH_PHASE_EXPECT_ENTRY;
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_scene_reset(XgRenderAuth *auth) {
    if (!is_owner(auth)) return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (auth->logical_identity.state_id.scene_epoch >
        auth->minimum_next_scene_epoch)
        auth->minimum_next_scene_epoch = auth->logical_identity.state_id.scene_epoch;
    guest_render_bridge_reset_scene();
    if (auth->ir != NULL) (void)xg_render_ir_reset(auth->ir);
    clear_scene(auth);
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_observe_hook(
    XgRenderAuth *auth,
    const XgRenderAuthExecution *execution,
    XgRenderAuthDecision *out_decision) {
    XgRenderAuthHook expected_hook;
    XgRenderAuthReason reason;

    if (!is_owner(auth) || execution == NULL || out_decision == NULL)
        return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (auth->phase == XG_RENDER_AUTH_PHASE_REJECTED) {
        set_decision(auth, out_decision);
        return XG_RENDER_AUTH_REJECTED;
    }
    if (execution->hook == XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR) {
        if (auth->phase < XG_RENDER_AUTH_PHASE_EXPECT_ENTRY ||
            auth->phase > XG_RENDER_AUTH_PHASE_FINALIZED)
            return XG_RENDER_AUTH_INVALID_TRANSITION;
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR,
            out_decision,
        });
    }
    if (!tier_valid(execution->tier) ||
        auth->phase < XG_RENDER_AUTH_PHASE_EXPECT_ENTRY ||
        auth->phase > XG_RENDER_AUTH_PHASE_EXPECT_RETURN)
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_HOOK_SEQUENCE,
            out_decision,
        });
    expected_hook = (XgRenderAuthHook)(auth->phase -
                                      XG_RENDER_AUTH_PHASE_EXPECT_ENTRY);
    if (execution->hook != expected_hook ||
        (auth->hook_count != 0u && execution->tier != auth->tier))
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            XG_RENDER_AUTH_REJECT_HOOK_SEQUENCE,
            out_decision,
        });
    reason = execution_reason(auth, execution);
    if (reason != XG_RENDER_AUTH_REJECT_NONE)
        return reject_hook(auth, execution, (XgRenderAuthRejection){
            reason,
            out_decision,
        });
    if (execution->hook == XG_RENDER_AUTH_HOOK_ENTRY) {
        auth->tier = execution->tier;
        auth->logical_identity.static_game_identity =
            execution->static_game_identity;
        auth->logical_identity.runtime_game_identity =
            execution->runtime_game_identity;
        if (begin_transaction(auth, execution, out_decision) != XG_RENDER_AUTH_OK)
            return XG_RENDER_AUTH_REJECTED;
        auth->phase = XG_RENDER_AUTH_PHASE_EXPECT_CAPTURE;
    } else if (execution->hook == XG_RENDER_AUTH_HOOK_CAPTURE_SITE) {
        auth->phase = XG_RENDER_AUTH_PHASE_EXPECT_RETURN;
    } else {
        if (finalize_transaction(auth, execution, out_decision) != XG_RENDER_AUTH_OK)
            return XG_RENDER_AUTH_REJECTED;
    }
    auth->hook_sequence[auth->hook_count++] = execution->hook;
    record_event(auth, execution, (XgRenderAuthTraceOutcome){
        XG_RENDER_AUTH_REJECT_NONE,
        XG_RENDER_AUTH_EVENT_ACCEPTED_HOOK,
    });
    set_decision(auth, out_decision);
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_bind_packet(
    XgRenderAuth *auth,
    uint32_t packet_address,
    uint32_t source_primitive_index) {
    if (!is_owner(auth)) return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (auth->phase == XG_RENDER_AUTH_PHASE_REJECTED)
        return XG_RENDER_AUTH_REJECTED;
    if (!auth->producer_open ||
        auth->phase != XG_RENDER_AUTH_PHASE_EXPECT_RETURN)
        return XG_RENDER_AUTH_INVALID_TRANSITION;
    if (guest_render_bridge_bind_packet(auth->producer_handle, packet_address,
                                        source_primitive_index) !=
        GUEST_RENDER_OK) {
        abort_transaction(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE);
        return XG_RENDER_AUTH_REJECTED;
    }
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_bind_provisional_packet(
    XgRenderAuth *auth,
    uint32_t packet_address,
    uint32_t source_primitive_index) {
    if (!is_owner(auth)) return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (auth->phase == XG_RENDER_AUTH_PHASE_REJECTED)
        return XG_RENDER_AUTH_REJECTED;
    if (!auth->producer_open ||
        auth->phase != XG_RENDER_AUTH_PHASE_EXPECT_CAPTURE)
        return XG_RENDER_AUTH_INVALID_TRANSITION;
    if (guest_render_bridge_bind_packet(auth->producer_handle, packet_address,
                                        source_primitive_index) !=
        GUEST_RENDER_OK) {
        abort_transaction(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE);
        return XG_RENDER_AUTH_REJECTED;
    }
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_append_native_insertion(
    XgRenderAuth *auth, uint32_t packet_address,
    uint32_t source_primitive_index, uint32_t ot_bucket,
    uint8_t tag_payload_word_count,
    const XgRenderIrNativePrimitive *primitive, bool provisional) {
    XgRenderIrNativeItem item = { 0 };
    XgRenderAuthResult bind_result;

    if (!is_owner(auth) || primitive == NULL)
        return XG_RENDER_AUTH_INVALID_ARGUMENT;
    bind_result = provisional
        ? xg_render_auth_bind_provisional_packet(
              auth, packet_address, source_primitive_index)
        : xg_render_auth_bind_packet(auth, packet_address,
                                     source_primitive_index);
    if (bind_result != XG_RENDER_AUTH_OK) return bind_result;
    item.base.has_provenance = true;
    item.base.provenance_key = auth->producer_handle;
    item.base.source_primitive_index = source_primitive_index;
    item.native = *primitive;
    if (xg_render_ir_append_native_insertion(
            auth->ir, &item, ot_bucket, packet_address,
            tag_payload_word_count) != XG_RENDER_IR_OK) {
        abort_transaction(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE);
        return XG_RENDER_AUTH_REJECTED;
    }
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_note_code_page_mutation(
    XgRenderAuth *auth,
    uint64_t previous_generation,
    uint64_t current_generation) {
    if (!is_owner(auth)) return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (previous_generation == current_generation) return XG_RENDER_AUTH_OK;
    if (auth->phase == XG_RENDER_AUTH_PHASE_REJECTED)
        return XG_RENDER_AUTH_REJECTED;
    if (auth->phase < XG_RENDER_AUTH_PHASE_EXPECT_ENTRY ||
        auth->phase > XG_RENDER_AUTH_PHASE_FINALIZED)
        return XG_RENDER_AUTH_INVALID_TRANSITION;
    abort_transaction(auth, XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION);
    record_event(auth, &(XgRenderAuthExecution){
        .tier = auth->tier,
        .hook = XG_RENDER_AUTH_HOOK_CAPTURE_SITE,
    }, (XgRenderAuthTraceOutcome){
        auth->reject_reason,
        XG_RENDER_AUTH_EVENT_CODE_PAGE_MUTATION,
    });
    return XG_RENDER_AUTH_REJECTED;
}

XgRenderAuthResult xg_render_auth_abort(XgRenderAuth *auth,
                                        XgRenderAuthReason reason) {
    if (!is_owner(auth) || reason == XG_RENDER_AUTH_REJECT_NONE ||
        reason > XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE)
        return XG_RENDER_AUTH_INVALID_ARGUMENT;
    if (auth->phase == XG_RENDER_AUTH_PHASE_REJECTED)
        return XG_RENDER_AUTH_REJECTED;
    if (auth->phase < XG_RENDER_AUTH_PHASE_EXPECT_ENTRY ||
        auth->phase > XG_RENDER_AUTH_PHASE_FINALIZED)
        return XG_RENDER_AUTH_INVALID_TRANSITION;
    abort_transaction(auth, reason);
    record_event(auth, &(XgRenderAuthExecution){
        .tier = auth->tier,
        .hook = XG_RENDER_AUTH_HOOK_CAPTURE_SITE,
    }, (XgRenderAuthTraceOutcome){
        auth->reject_reason,
        XG_RENDER_AUTH_EVENT_REJECTED_HOOK,
    });
    return XG_RENDER_AUTH_REJECTED;
}

XgRenderAuthResult xg_render_auth_snapshot(
    const XgRenderAuth *auth,
    XgRenderAuthSnapshot *out_snapshot) {
    if (!is_owner(auth) || out_snapshot == NULL)
        return XG_RENDER_AUTH_INVALID_ARGUMENT;
    out_snapshot->effective_render_mode = auth->effective_render_mode;
    out_snapshot->reject_reason = auth->reject_reason;
    out_snapshot->field_image_identity = auth->profile.field_image_identity;
    out_snapshot->logical_identity = auth->logical_identity;
    out_snapshot->producer_begin_count = auth->producer_begin_count;
    out_snapshot->native_item_count = auth->native_item_count;
    out_snapshot->hook_count = auth->hook_count;
    memcpy(out_snapshot->hook_sequence, auth->hook_sequence,
           sizeof(out_snapshot->hook_sequence));
    out_snapshot->trace_count = auth->trace_count;
    out_snapshot->next_trace_sequence = auth->next_trace_sequence;
    out_snapshot->scene_aborted = auth->scene_aborted;
    out_snapshot->ir_usable = auth->ir_usable;
    out_snapshot->native_use_permitted = auth->native_use_permitted;
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_native_item_get(
    const XgRenderAuth *auth, size_t index, XgRenderIrNativeItem *out_item) {
    XgRenderIrItem item = { 0 };

    if (!is_owner(auth) || out_item == NULL || auth->ir == NULL ||
        auth->phase != XG_RENDER_AUTH_PHASE_FINALIZED ||
        xg_render_ir_item_get(auth->ir, index, &item) != XG_RENDER_IR_OK ||
        item.kind != XG_RENDER_IR_ITEM_NATIVE)
        return XG_RENDER_AUTH_INVALID_ARGUMENT;
    out_item->base = item.base;
    out_item->native = item.native;
    return XG_RENDER_AUTH_OK;
}

XgRenderAuthResult xg_render_auth_trace_snapshot(
    const XgRenderAuth *auth,
    XgRenderAuthTraceSnapshot *out_snapshot) {
    size_t first;
    size_t index;

    if (!is_owner(auth) || out_snapshot == NULL)
        return XG_RENDER_AUTH_INVALID_ARGUMENT;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->next_sequence = auth->next_trace_sequence;
    out_snapshot->count = auth->trace_count;
    first = (auth->trace_write_index + XG_RENDER_AUTH_TRACE_CAPACITY -
             auth->trace_count) % XG_RENDER_AUTH_TRACE_CAPACITY;
    for (index = 0u; index < auth->trace_count; ++index)
        out_snapshot->events[index] =
            auth->trace[(first + index) % XG_RENDER_AUTH_TRACE_CAPACITY];
    return XG_RENDER_AUTH_OK;
}

size_t xg_render_auth_trace_capacity(void) {
    return XG_RENDER_AUTH_TRACE_CAPACITY;
}

const char *xg_render_auth_reason_name(uint32_t reason) {
    switch (reason) {
    case XG_RENDER_AUTH_REJECT_NONE: return "none";
    case XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH: return "identity_mismatch";
    case XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH: return "validation_mismatch";
    case XG_RENDER_AUTH_REJECT_CACHE_IDENTITY_MISMATCH:
        return "cache_identity_mismatch";
    case XG_RENDER_AUTH_REJECT_HOOK_SEQUENCE: return "hook_sequence";
    case XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION:
        return "code_page_mutation";
    case XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR: return "foreign_interior";
    case XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE:
        return "transaction_failure";
    default: return "unknown";
    }
}
