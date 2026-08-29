#include "xg_render_auth_runtime_control.h"
#include "xg_render_auth_runtime_diagnostics.h"
#include "xg_render_auth_runtime_hooks.h"
#include "xg_render_auth_runtime_invalidation.h"

#include "guest_render_bridge.h"
#include "overlay_api.h"
#include "psx_xg_render_auth_hook_types.h"
#include "xg_render_auth.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_render_runtime_composition.h"
#include "xg_render_runtime_host_services.h"
#include "xg_render_static_auth_metadata.h"
#include "xg_render_instrumentation.h"
#include "guest_render_native_stream.h"
#include "guest_render_transaction.h"
#include "gte_attribution.h"
#include "gpu.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * Authentication and scene-policy runtime. Producer composition is supplied
 * through XgRenderRuntimeAuthSceneServices.
 */

typedef struct XgRenderAuthRuntimeState {
    XgRenderAuth *auth;
    PsxXgRenderAuthCandidate pending_candidate;
    PsxXgRenderAuthCandidate authenticated_artifact_candidate;
    PsxXgRenderAuthRejectionReceipt rejection;
    uint64_t scene_generation;
    uint64_t interpolation_scene_generation;
    uint64_t pending_scene_generation;
    uint64_t authenticated_artifact_scene_generation;
    uint64_t authenticated_artifact_generation;
    uint64_t authenticated_producer_scene_generation;
    uint32_t authenticated_producer_entry;
    uint32_t pending_variant_entry;
    XgRenderAuthTier pending_variant_tier;
    bool armed;
    bool active;
    bool completed;
    bool pending_candidate_valid;
    bool authenticated_artifact_candidate_valid;
    bool pending_variant_sequence;
    bool pending_variant_capture_ready;
    bool candidate_matched;
    bool candidate_dispatched;
    bool gte_attribution_producer_active;
    GuestRenderRenderMode requested_render_mode;
    PsxXgRenderPresentationGate presentation_gate;
    void *presentation_user_data;
    NativeRenderPresentationSnapshot presentation;
    bool configured;
} XgRenderAuthRuntimeState;

static XgRenderAuthRuntimeState state = {
    .armed = true,
    .requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
};

bool g_psx_xg_render_auth_cold_enabled;
static PsxXgRenderAuthCompletedProofReceipt completed_proof;
static uint64_t completed_proof_scene_generation;
static atomic_flag completed_proof_guard = ATOMIC_FLAG_INIT;

static void abort_active(XgRenderAuthReason reason,
                         PsxXgRenderAuthRejectionSource source,
                         bool has_hook, PsxXgRenderAuthHook hook,
                         uint32_t pc);
static uint64_t interpolation_scene_generation(void);
static bool pending_variant_artifact_candidate_matches(uint32_t pc);
static bool current_artifact_is_authorized(void);
static bool completed_proof_matches_tier(XgRenderAuthTier tier);
static bool submission_active_auth_append(
    uint32_t packet_address, uint32_t source_primitive_index,
    uint32_t ot_bucket, uint8_t payload_word_count,
    const XgRenderIrNativePrimitive *primitive, bool force_pending_capture,
    uint32_t *out_failure_detail);
static bool submission_presentation_gate(void);

static void composition_query_state(
        XgRenderRuntimeAuthSceneState *out_state) {
    if (out_state == NULL) return;
    *out_state = (XgRenderRuntimeAuthSceneState){
        .render_mode = state.requested_render_mode,
        .pending_tier = state.pending_variant_tier,
        .scene_generation = state.scene_generation,
        .interpolation_generation = interpolation_scene_generation(),
        .artifact_generation = state.authenticated_artifact_generation,
        .pending_producer_entry = state.pending_variant_entry,
        .armed = state.armed,
        .active = state.active,
        .completed = state.completed,
        .pending_sequence = state.pending_variant_sequence,
        .pending_capture_ready = state.pending_variant_capture_ready,
        .candidate_matched = state.candidate_matched,
        .candidate_dispatched = state.candidate_dispatched,
    };
}

static bool composition_auth_snapshot(XgRenderAuthSnapshot *out_snapshot) {
    return out_snapshot != NULL && state.auth != NULL &&
        xg_render_auth_snapshot(state.auth, out_snapshot) == XG_RENDER_AUTH_OK;
}

static bool composition_auth_ir_item_get(
        size_t index, XgRenderIrNativeItem *out_item) {
    return out_item != NULL && state.auth != NULL &&
        xg_render_auth_native_item_get(state.auth, index, out_item) ==
            XG_RENDER_AUTH_OK;
}

static void composition_reject_auth(uint32_t blocker) {
    (void)blocker;
    abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                 PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                 false, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, 0u);
}

static const XgRenderRuntimeAuthSceneServices *composition_services(void) {
    static XgRenderRuntimeAuthSceneServices services;
    XgRenderRuntimeHostServices host = { 0 };
    const bool host_configured = xg_render_runtime_host_services(&host);

    services = (XgRenderRuntimeAuthSceneServices){
        .query_state = composition_query_state,
        .auth_snapshot = composition_auth_snapshot,
        .auth_ir_item_get = composition_auth_ir_item_get,
        .append_authenticated_ir = submission_active_auth_append,
        .artifact_authorizes_pc = pending_variant_artifact_candidate_matches,
        .artifact_is_authorized = current_artifact_is_authorized,
        .completed_proof_matches_tier = completed_proof_matches_tier,
        .reject_auth = composition_reject_auth,
        .presentation_gate = submission_presentation_gate,
        .frame_count = host_configured ? host.frame_count : NULL,
        .read_guest_word = host_configured ? host.read_word : NULL,
    };
    return &services;
}

static void lock_completed_proof(void) {
    while (atomic_flag_test_and_set_explicit(&completed_proof_guard,
                                             memory_order_acquire)) {}
}

static void unlock_completed_proof(void) {
    atomic_flag_clear_explicit(&completed_proof_guard, memory_order_release);
}

static bool completed_proof_matches_tier(XgRenderAuthTier tier) {
    bool matches;

    lock_completed_proof();
    matches = completed_proof.available && !completed_proof.blocked &&
              completed_proof_scene_generation == state.scene_generation &&
              completed_proof.tier == tier;
    unlock_completed_proof();
    return matches;
}

static bool is_control_transfer(uint32_t instruction) {
    const uint32_t opcode = instruction >> 26u;
    const uint32_t function = instruction & 0x3fu;

    return opcode == 1u || opcode == 2u || opcode == 3u ||
           (opcode >= 4u && opcode <= 7u) ||
           (opcode >= 0x14u && opcode <= 0x17u) ||
           (opcode == 0u && (function == 8u || function == 9u));
}

static XgRenderAuthDigest codegen_identity(void) {
    XgRenderAuthDigest digest = { { 0 } };
    const uint32_t values[] = {
        PSX_OVERLAY_ABI_TAG,
        PSX_OVERLAY_CODEGEN_VER,
        PSX_OVERLAY_CODEGEN_HASH,
    };

    memcpy(digest.bytes, values, sizeof(values));
    return digest;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & 0x1fffffffu) == (right & 0x1fffffffu);
}

static uint32_t guest_address(uint32_t pc) {
    return pc == 0u ? 0u : (pc & UINT32_C(0x1fffffff)) |
        UINT32_C(0x80000000);
}

static bool range_contains(uint32_t range_start, uint32_t range_size,
                           uint32_t value, uint32_t value_size) {
    const uint64_t start = range_start & 0x1fffffffu;
    const uint64_t end = start + range_size;
    const uint64_t physical_value = value & 0x1fffffffu;

    return range_size != 0u && value_size != 0u &&
           physical_value >= start && physical_value + value_size <= end;
}

static bool normalized_ranges_overlap(uint32_t left_start, uint32_t left_size,
                                      uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & 0x1fffffffu;
    const uint64_t left_end = left_begin + left_size;
    const uint64_t right_begin = right_start & 0x1fffffffu;
    const uint64_t right_end = right_begin + right_size;

    return left_size != 0u && right_size != 0u &&
           left_begin < right_end && right_begin < left_end;
}

static bool field_range_contains(uint32_t pc) {
    return range_contains(xg_render_manifest_validation.field_range_start,
                          xg_render_manifest_validation.field_range_size,
                          pc, 1u);
}

static bool field_range_is_bound(void) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;

    return range_contains(validation->field_range_start,
                          validation->field_range_size,
                          validation->producer_entry, 4u) &&
           range_contains(validation->field_range_start,
                          validation->field_range_size,
                          validation->instruction_window_start,
                          validation->instruction_window_size);
}

static bool authentication_range_write_overlaps(uint32_t write_address,
                                                uint32_t write_size) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;

    return normalized_ranges_overlap(validation->producer_entry, 4u,
                                     write_address, write_size) ||
           normalized_ranges_overlap(validation->instruction_window_start,
                                     validation->instruction_window_size,
                                     write_address, write_size);
}

static bool candidate_matches_manifest(
    const PsxXgRenderAuthCandidate *candidate) {
    const XgRenderManifestValidation *validation =
        &xg_render_manifest_validation;

    return candidate != NULL &&
           candidate->authority_provenance && candidate->pair_bound &&
           candidate->pair_id != 0u &&
           memcmp(candidate->identity.game_sha256, xg_render_game_identity,
                  sizeof(candidate->identity.game_sha256)) == 0 &&
           memcmp(candidate->identity.manifest_sha256,
                  xg_render_manifest_identity,
                  sizeof(candidate->identity.manifest_sha256)) == 0 &&
           physical_address_equals(candidate->artifact_base,
                                   validation->field_range_start) &&
           candidate->artifact_size == validation->field_range_size &&
           candidate->artifact_crc32 == validation->field_base_crc32 &&
           physical_address_equals(candidate->producer_entry,
                                   validation->producer_entry) &&
           physical_address_equals(candidate->dispatch_pc,
                                   validation->producer_entry) &&
           field_range_is_bound() &&
           range_contains(validation->field_range_start,
                          validation->field_range_size,
                          candidate->range_start, candidate->range_size) &&
           range_contains(candidate->range_start, candidate->range_size,
                          validation->producer_entry, 4u) &&
           range_contains(candidate->range_start, candidate->range_size,
                          validation->instruction_window_start,
                          validation->instruction_window_size);
}

static void clear_pending_candidate(void) {
    memset(&state.pending_candidate, 0, sizeof(state.pending_candidate));
    state.pending_candidate_valid = false;
    state.pending_scene_generation = 0u;
}

static void clear_authenticated_artifact_candidate(void) {
    memset(&state.authenticated_artifact_candidate, 0,
           sizeof(state.authenticated_artifact_candidate));
    state.authenticated_artifact_candidate_valid = false;
    state.authenticated_artifact_scene_generation = 0u;
}

static void clear_candidate_outcome(void) {
    state.candidate_matched = false;
    state.candidate_dispatched = false;
}

static void clear_pending_variant_sequence(void) {
    state.pending_variant_entry = 0u;
    state.pending_variant_tier = XG_RENDER_AUTH_TIER_STATIC;
    state.pending_variant_sequence = false;
    state.pending_variant_capture_ready = false;
}

static bool pending_candidate_matches(uint32_t pc) {
    return state.pending_candidate_valid &&
           state.pending_scene_generation == state.scene_generation &&
           physical_address_equals(pc,
                                   xg_render_manifest_validation.producer_entry) &&
           candidate_matches_manifest(&state.pending_candidate);
}

static bool pending_variant_candidate_matches(void) {
    return state.pending_candidate_valid &&
           state.pending_scene_generation == state.scene_generation &&
           xg_render_runtime_variant_candidate_matches(&state.pending_candidate);
}

static bool pending_candidate_matches_runtime_variant_artifact(void) {
    return state.pending_candidate_valid &&
           state.pending_scene_generation == state.scene_generation &&
           state.pending_candidate.runtime_variant_bound &&
           xg_render_runtime_variant_artifact_candidate_matches(
               &state.pending_candidate);
}

static bool pending_variant_artifact_candidate_matches(uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return true;

    const bool candidate_authorized =
        state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        (xg_render_runtime_variant_artifact_candidate_authorizes_pc(
             &state.authenticated_artifact_candidate, pc) ||
         xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
              &state.authenticated_artifact_candidate, pc));

    return candidate_authorized;
}

static bool authenticated_variant_hook_matches(uint32_t pc) {
    return pending_variant_artifact_candidate_matches(pc);
}

static bool current_artifact_is_authorized(void) {
    if (xg_render_runtime_variant_no_gates_enabled()) return true;
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        (xg_render_runtime_variant_artifact_candidate_matches(
             &state.authenticated_artifact_candidate) ||
         xg_render_authoritative_overlay_artifact_candidate_matches(
             &state.authenticated_artifact_candidate));
}

static bool current_artifact_code_range_overlaps(uint32_t address,
                                                  uint32_t size) {
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        normalized_ranges_overlap(
            state.authenticated_artifact_candidate.range_start,
            state.authenticated_artifact_candidate.range_size, address, size) &&
        xg_render_authoritative_overlay_artifact_candidate_matches(
            &state.authenticated_artifact_candidate);
}

static bool current_artifact_range_contains_pc(uint32_t pc) {
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        (xg_render_runtime_variant_artifact_candidate_authorizes_pc(
             &state.authenticated_artifact_candidate, pc) ||
         (xg_render_authoritative_overlay_artifact_candidate_matches(
              &state.authenticated_artifact_candidate) &&
          range_contains(
              state.authenticated_artifact_candidate.range_start,
              state.authenticated_artifact_candidate.range_size, pc, 4u)));
}

static bool current_artifact_memory_contains_pc(uint32_t pc) {
    return state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_generation != 0u &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        normalized_ranges_overlap(
            state.authenticated_artifact_candidate.artifact_base,
            state.authenticated_artifact_candidate.artifact_size, pc, 4u);
}

static bool artifact_binary_identity_matches(
        const PsxXgRenderAuthCandidate *left,
        const PsxXgRenderAuthCandidate *right) {
    return left != NULL && right != NULL &&
        left->artifact_size == right->artifact_size &&
        left->artifact_crc32 == right->artifact_crc32 &&
        physical_address_equals(left->artifact_base, right->artifact_base) &&
        memcmp(&left->identity, &right->identity, sizeof(left->identity)) == 0;
}

static void broadcast_resource_overlap(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_RESOURCE_OVERLAP,
    };
    xg_render_runtime_composition_handle_invalidation(&event);
}

static uint64_t interpolation_scene_generation(void) {
    return state.interpolation_scene_generation != 0u
        ? state.interpolation_scene_generation : 1u;
}

static void advance_interpolation_scene(void) {
    if (state.interpolation_scene_generation == 0u)
        state.interpolation_scene_generation = 1u;
    else if (state.interpolation_scene_generation != UINT64_MAX)
        ++state.interpolation_scene_generation;
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
}

static bool submission_active_auth_append(
        uint32_t packet_address, uint32_t source_primitive_index,
        uint32_t ot_bucket, uint8_t payload_word_count,
        const XgRenderIrNativePrimitive *primitive,
        bool force_pending_capture,
        uint32_t *out_failure_detail) {
    const XgRenderAuthResult result = state.auth != NULL
        ? xg_render_auth_append_native_insertion(
            state.auth, packet_address, source_primitive_index, ot_bucket,
            payload_word_count, primitive,
            force_pending_capture ||
                (state.pending_variant_sequence &&
                 !state.pending_variant_capture_ready))
        : XG_RENDER_AUTH_INVALID_TRANSITION;

    if (out_failure_detail != NULL)
        *out_failure_detail = (uint32_t)result;
    return result == XG_RENDER_AUTH_OK;
}

static bool submission_presentation_gate(void) {
    return state.presentation_gate != NULL &&
        state.presentation_gate(state.requested_render_mode,
                                &state.presentation,
                                state.presentation_user_data);
}

static GteAttributionExecutionTier gte_attribution_tier_for(
    XgRenderAuthTier tier) {
    return tier == XG_RENDER_AUTH_TIER_WARM_NATIVE
        ? GTE_ATTRIBUTION_TIER_WARM
        : GTE_ATTRIBUTION_TIER_COLD;
}

static void end_gte_attribution_producer(void) {
    if (!state.gte_attribution_producer_active) return;
    state.gte_attribution_producer_active = false;
    (void)gte_attribution_producer_end();
}

static bool begin_gte_attribution_producer(XgRenderAuthTier tier) {
    XgRenderAuthSnapshot snapshot = { 0 };
    GteAttributionProducerContext context = { 0 };

    end_gte_attribution_producer();
    if (state.auth == NULL ||
        xg_render_auth_snapshot(state.auth, &snapshot) != XG_RENDER_AUTH_OK ||
        snapshot.logical_identity.state_id.scene_epoch == 0u ||
        snapshot.logical_identity.producer_record_id == 0u)
        return false;
    context.visual_state_id.scene_epoch =
        snapshot.logical_identity.state_id.scene_epoch;
    context.visual_state_id.state_sequence =
        snapshot.logical_identity.state_id.state_sequence;
    context.producer_id = snapshot.logical_identity.producer_record_id;
    context.tier = gte_attribution_tier_for(tier);
    if (gte_attribution_producer_begin(&context) != GTE_ATTRIBUTION_OK)
        return false;
    state.gte_attribution_producer_active = true;
    return true;
}

static void disarm(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_AUTHORITY_LOST,
    };

    if (xg_render_runtime_variant_no_gates_enabled()) return;
    xg_render_runtime_composition_handle_invalidation(&event);
    end_gte_attribution_producer();
    state.active = false;
    state.armed = false;
    state.completed = false;
    clear_pending_candidate();
    clear_pending_variant_sequence();
}

static void retire_completed_auth_proof(void) {
    if (!state.completed) return;
    if (state.auth == NULL ||
        xg_render_auth_scene_reset(state.auth) != XG_RENDER_AUTH_OK) {
        disarm();
        return;
    }
    end_gte_attribution_producer();
    state.active = false;
    state.armed = true;
    state.completed = false;
    state.rejection = (PsxXgRenderAuthRejectionReceipt){ 0 };
    clear_pending_candidate();
    clear_pending_variant_sequence();
    clear_candidate_outcome();
    xg_render_runtime_variant_reset();
}

static void block_completed_proof(
    XgRenderAuthReason reason,
    const PsxXgRenderAuthRejectionReceipt *rejection) {
    PsxXgRenderAuthCompletedProofReceipt local;

    lock_completed_proof();
    local = completed_proof;
    if (local.available && !local.blocked) {
        local.blocked = true;
        local.blocker_reason = reason;
        local.blocker_rejection = *rejection;
        completed_proof = local;
    }
    unlock_completed_proof();
}

static void latch_rejection(XgRenderAuthReason reason,
                            PsxXgRenderAuthRejectionSource source,
                            bool has_hook, PsxXgRenderAuthHook hook,
                            uint32_t pc) {
    PsxXgRenderAuthRejectionReceipt rejection;

    if (xg_render_runtime_variant_no_gates_enabled()) return;
    if (!state.active ||
        state.rejection.source != PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE)
        return;
    rejection = (PsxXgRenderAuthRejectionReceipt){
        source,
        hook,
        guest_address(pc),
        has_hook,
    };
    state.rejection = rejection;
    block_completed_proof(reason, &rejection);
}

static void abort_active(XgRenderAuthReason reason,
                         PsxXgRenderAuthRejectionSource source,
                         bool has_hook, PsxXgRenderAuthHook hook,
                         uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    latch_rejection(reason, source, has_hook, hook, pc);
    if (state.auth != NULL && state.active)
        (void)xg_render_auth_abort(state.auth, reason);
    else
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    disarm();
}

static XgRenderAuthExecution execution_for(XgRenderAuthTier tier,
                                            XgRenderAuthHook hook) {
    XgRenderAuthExecution execution = xg_render_static_auth_execution_for(hook);

    execution.tier = tier;
    execution.cache_identity.codegen_digest = codegen_identity();
    return execution;
}

static void publish_completed_proof(XgRenderAuthTier tier) {
    XgRenderAuthSnapshot snapshot = { 0 };
    PsxXgRenderAuthCompletedProofReceipt local = { 0 };

    if (state.auth == NULL ||
        xg_render_auth_snapshot(state.auth, &snapshot) != XG_RENDER_AUTH_OK ||
        snapshot.reject_reason != XG_RENDER_AUTH_REJECT_NONE ||
        snapshot.scene_aborted || !snapshot.ir_usable ||
        snapshot.hook_count != XG_RENDER_AUTH_HOOK_STAGE_COUNT ||
        snapshot.hook_sequence[0] != XG_RENDER_AUTH_HOOK_ENTRY ||
        snapshot.hook_sequence[1] != XG_RENDER_AUTH_HOOK_CAPTURE_SITE ||
        snapshot.hook_sequence[2] != XG_RENDER_AUTH_HOOK_RETURN ||
        snapshot.logical_identity.state_id.scene_epoch == 0u ||
        (tier != XG_RENDER_AUTH_TIER_COLD_INTERPRETER &&
         tier != XG_RENDER_AUTH_TIER_WARM_NATIVE) ||
        snapshot.next_trace_sequence < XG_RENDER_AUTH_HOOK_STAGE_COUNT + 1u ||
        (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE &&
         (!state.candidate_matched || !state.candidate_dispatched)))
        return;

    local.available = true;
    local.producer_record_id =
        snapshot.logical_identity.producer_record_id;
    local.site_record_id = snapshot.logical_identity.site_record_id;
    local.tuple = (PsxXgRenderAuthCompletedProofTuple){
        snapshot.logical_identity.producer_entry,
        snapshot.logical_identity.capture_site,
        snapshot.logical_identity.static_callee,
        snapshot.logical_identity.return_site,
    };
    local.tier = tier;
    local.state_id = snapshot.logical_identity.state_id;
    local.entry_event_sequence =
        snapshot.next_trace_sequence - XG_RENDER_AUTH_HOOK_STAGE_COUNT;
    local.capture_event_sequence = local.entry_event_sequence + 1u;
    local.return_event_sequence = local.capture_event_sequence + 1u;
    local.candidate_matched = state.candidate_matched;
    local.candidate_dispatched = state.candidate_dispatched;

    lock_completed_proof();
    completed_proof = local;
    completed_proof_scene_generation = state.scene_generation;
    unlock_completed_proof();
    xg_render_instrumentation_record_completed_proof();
}

static void begin_scene(XgRenderAuthTier tier, uint32_t producer_entry) {
    const GuestRenderSceneConfig config = {
        state.requested_render_mode,
    };
    GuestRenderVisualStateId state_id = { 0 };
    XgRenderAuthProfile profile;

    if (producer_entry != xg_render_manifest_validation.producer_entry)
        return;
    (void)xg_render_runtime_composition_configure(composition_services());
    xg_render_runtime_composition_prepare_authenticated_scene();
    if (state.active && state.auth != NULL)
        (void)xg_render_auth_scene_reset(state.auth);
    end_gte_attribution_producer();
    state.active = false;
    state.armed = true;
    state.completed = false;
    state.rejection = (PsxXgRenderAuthRejectionReceipt){ 0 };
    if (xg_render_auth_process_owner(&state.auth) != XG_RENDER_AUTH_OK ||
        !xg_render_static_auth_metadata_is_valid() ||
        !xg_render_static_auth_bind_identity(&(bool){ false }, &(bool){ false })) {
        abort_active(XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE, false,
                     PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0u);
        return;
    }
    profile = xg_render_static_auth_profile_from_metadata();
    profile.cache_identity.codegen_digest = codegen_identity();
    if (guest_render_bridge_begin_scene(&config) != GUEST_RENDER_OK) {
        abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE, false,
                     PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0u);
        return;
    }
    guest_render_native_stream_set_enabled(
        state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE);
    if (state.presentation_gate != NULL) {
        if (!state.presentation_gate(state.requested_render_mode,
                                     &state.presentation,
                                     state.presentation_user_data))
            guest_render_bridge_force_original(
                GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    } else if (state.requested_render_mode !=
               GUEST_RENDER_RENDER_ORIGINAL) {
        memset(&state.presentation, 0, sizeof(state.presentation));
        state.presentation.reason =
            NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED;
        guest_render_bridge_force_original(
            GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    }
    if (guest_render_bridge_begin_state(&state_id) != GUEST_RENDER_OK ||
        xg_render_auth_scene_begin(state.auth, state_id, &profile) != XG_RENDER_AUTH_OK) {
        abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE, false,
                     PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0u);
        return;
    }
    state.active = true;
    (void)tier;
}

static void observe_capture(XgRenderAuthTier tier, uint32_t pc,
                            uint32_t instruction_word, uint32_t delay_slot_word,
                            uint32_t return_address) {
    XgRenderAuthDecision decision = { 0 };
    XgRenderAuthExecution execution;

    if (!state.active) return;
    if (!physical_address_equals(return_address, pc + 8u)) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_CAPTURE, pc);
        return;
    }
    execution = execution_for(tier, XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
    execution.validation.caller_site = pc;
    execution.validation.callee_entry =
        (pc & 0xf0000000u) | ((instruction_word & 0x03ffffffu) << 2u);
    execution.validation.return_site = pc + 8u;
    execution.validation.required_jal_opcode = instruction_word >> 26u;
    execution.validation.jal_target = execution.validation.callee_entry;
    execution.validation.delay_slot_complete = true;
    execution.validation.delay_slot_is_control_transfer =
        is_control_transfer(delay_slot_word);
    if (xg_render_auth_observe_hook(state.auth, &execution, &decision) !=
        XG_RENDER_AUTH_OK) {
        latch_rejection(decision.reject_reason,
                        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                        true, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, pc);
        disarm();
    }
}

static void observe_return(XgRenderAuthTier tier, uint32_t return_site,
                           uint32_t return_address) {
    XgRenderAuthDecision decision = { 0 };
    XgRenderAuthExecution execution;

    if (!state.active) return;
    if (!physical_address_equals(return_address, return_site)) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_RETURN, return_site);
        return;
    }
    execution = execution_for(tier, XG_RENDER_AUTH_HOOK_RETURN);
    execution.validation.return_site = return_site;
    if (xg_render_auth_observe_hook(state.auth, &execution, &decision) !=
        XG_RENDER_AUTH_OK) {
        latch_rejection(decision.reject_reason,
                        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                     true, PSX_XG_RENDER_AUTH_HOOK_RETURN, return_site);
        disarm();
    } else if (state.requested_render_mode == GUEST_RENDER_RENDER_NATIVE &&
               !xg_render_runtime_composition_flush_authenticated_ir()) {
        abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_RETURN, return_site);
    } else {
        publish_completed_proof(tier);
        state.completed = true;
        end_gte_attribution_producer();
    }
}

static void observe_entry(XgRenderAuthTier tier, uint32_t pc, bool warm,
                          bool candidate_matched) {
    clear_pending_candidate();
    clear_candidate_outcome();
    begin_scene(tier, xg_render_manifest_validation.producer_entry);
    if (warm && !candidate_matched) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                     true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        if (!xg_render_runtime_variant_no_gates_enabled()) return;
    }
    if (warm && candidate_matched) state.candidate_matched = true;
    if (state.active) {
        XgRenderAuthDecision decision = { 0 };
        XgRenderAuthExecution execution =
            execution_for(tier, XG_RENDER_AUTH_HOOK_ENTRY);

        execution.observed_producer_entry =
            xg_render_manifest_validation.producer_entry;
        if (xg_render_auth_observe_hook(state.auth, &execution, &decision) !=
            XG_RENDER_AUTH_OK) {
            latch_rejection(decision.reject_reason,
                            PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                            true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
            disarm();
        } else if (!begin_gte_attribution_producer(tier)) {
            abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                         true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        } else if (!xg_render_runtime_composition_flush_pre_scene()) {
            abort_active(XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK,
                         true, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        } else if (warm && candidate_matched) {
            state.candidate_dispatched = true;
        }
    }
}

static bool active_scene_is_canonical_entry_only(void) {
    XgRenderAuthSnapshot snapshot = { 0 };

    return state.active && !state.completed && state.auth != NULL &&
           xg_render_auth_snapshot(state.auth, &snapshot) == XG_RENDER_AUTH_OK &&
           !snapshot.scene_aborted &&
           snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE &&
           snapshot.producer_begin_count == 1u &&
           snapshot.native_item_count == 0u &&
           snapshot.hook_count == 1u &&
           snapshot.hook_sequence[0] == XG_RENDER_AUTH_HOOK_ENTRY;
}

static void observe_hook(CPUState *cpu, XgRenderAuthTier tier, uint32_t hook, uint32_t pc,
    uint32_t instruction_word, uint32_t delay_slot_word,
    uint32_t return_address) {
    if (xg_render_runtime_composition_observe_auth_hook(
            cpu, tier, hook, pc, instruction_word, delay_slot_word))
        return;
    const XgRenderRuntimeVariantEvent variant_event =
        xg_render_runtime_variant_observe(hook, pc, instruction_word,
                                           delay_slot_word, return_address,
                                           state.scene_generation);

    xg_render_instrumentation_record_variant_progress(
        variant_event, xg_render_runtime_variant_event_is_exact(variant_event, pc));

    if (variant_event == XG_RENDER_RUNTIME_VARIANT_CONSUMED)
        return;
    if (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE &&
        state.pending_candidate_valid && !state.pending_variant_sequence &&
        variant_event != XG_RENDER_RUNTIME_VARIANT_ACTIVATED &&
        variant_event != XG_RENDER_RUNTIME_VARIANT_ENTRY &&
        (hook != PSX_XG_RENDER_AUTH_HOOK_ENTRY ||
         !physical_address_equals(pc,
                                  xg_render_manifest_validation.producer_entry)))
        clear_pending_candidate();
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_ACTIVATED) {
        clear_pending_variant_sequence();
        if (!state.active) state.armed = true;
        if (!state.armed ||
            (state.active && !state.completed &&
             !active_scene_is_canonical_entry_only()))
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                         true, (PsxXgRenderAuthHook)hook, pc);
        return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_REJECT) {
        abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK, true,
                     (PsxXgRenderAuthHook)hook, pc);
        if (!xg_render_runtime_variant_no_gates_enabled()) return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_ENTRY) {
        const bool warm = tier == XG_RENDER_AUTH_TIER_WARM_NATIVE;
        const bool candidate_matched = !warm ||
            pending_variant_candidate_matches();

        state.pending_variant_entry = pc;
        state.pending_variant_tier = tier;
        state.pending_variant_sequence = true;
        state.pending_variant_capture_ready = false;
        observe_entry(tier, pc, warm, candidate_matched);
        return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_CAPTURE) {
        if (!state.pending_variant_sequence ||
            state.pending_variant_tier != tier) {
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                         true, (PsxXgRenderAuthHook)hook, pc);
            return;
        }
        state.pending_variant_capture_ready = true;
        observe_capture(tier, xg_render_manifest_validation.caller_site,
                        instruction_word, delay_slot_word,
                        xg_render_manifest_validation.return_site);
        return;
    }
    if (variant_event == XG_RENDER_RUNTIME_VARIANT_RETURN) {
        if (!state.pending_variant_sequence ||
            !state.pending_variant_capture_ready ||
            state.pending_variant_tier != tier) {
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK,
                         true, (PsxXgRenderAuthHook)hook, pc);
            return;
        }
        observe_return(tier, xg_render_manifest_validation.return_site,
                       xg_render_manifest_validation.return_site);
        if (state.completed) {
            state.authenticated_producer_entry = state.pending_variant_entry;
            state.authenticated_producer_scene_generation =
                state.scene_generation;
        }
        clear_pending_variant_sequence();
        xg_render_runtime_variant_reset();
        return;
    }
    if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY) {
        const bool warm = tier == XG_RENDER_AUTH_TIER_WARM_NATIVE;
        const bool candidate_matched = warm && pending_candidate_matches(pc);

        if (!physical_address_equals(pc,
                                     xg_render_manifest_validation.producer_entry))
            return;
        if (warm && !candidate_matched &&
            pending_candidate_matches_runtime_variant_artifact())
            return;
        observe_entry(tier, pc, warm, candidate_matched);
    } else if (hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE) {
        if (!physical_address_equals(pc,
                                     xg_render_manifest_validation.caller_site))
            return;
        observe_capture(tier, xg_render_manifest_validation.caller_site,
                        instruction_word, delay_slot_word, return_address);
    } else if (hook == PSX_XG_RENDER_AUTH_HOOK_RETURN) {
        if (!physical_address_equals(pc,
                                     xg_render_manifest_validation.return_site))
            return;
        observe_return(tier, xg_render_manifest_validation.return_site,
                       return_address);
    } else if (hook == PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR &&
               field_range_contains(pc)) {
        abort_active(XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, true,
                     PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR, pc);
    }
}

bool psx_xg_render_auth_configure(
    GuestRenderRenderMode requested_render_mode,
    PsxXgRenderPresentationGate presentation_gate,
    void *presentation_user_data) {
    const bool render_valid =
        requested_render_mode == GUEST_RENDER_RENDER_ORIGINAL ||
        requested_render_mode == GUEST_RENDER_RENDER_SHADOW ||
        requested_render_mode == GUEST_RENDER_RENDER_NATIVE;
    const GuestRenderRenderMode render_mode = render_valid
        ? requested_render_mode : GUEST_RENDER_RENDER_ORIGINAL;
    const bool preserve_producer_family =
        xg_render_runtime_composition_producer_family_enabled();
    const XgRenderAuthRuntimeState previous_state = state;

    if (state.configured)
        return state.requested_render_mode == render_mode &&
            state.presentation_gate == presentation_gate &&
            state.presentation_user_data == presentation_user_data;
    state.requested_render_mode = render_mode;
    state.presentation_gate = presentation_gate;
    state.presentation_user_data = presentation_user_data;
    if (!xg_render_runtime_composition_configure(composition_services())) {
        state = previous_state;
        return false;
    }
    if (state.interpolation_scene_generation == 0u)
        state.interpolation_scene_generation = 1u;
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
    g_psx_xg_render_auth_cold_enabled =
        state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL;
    xg_render_runtime_composition_enable_producer_family(
        preserve_producer_family ||
        state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL);
    state.configured = true;
    return true;
}

bool psx_xg_render_auth_configure_native_view(
    bool enabled, uint16_t aspect_num, uint16_t aspect_den,
    uint16_t canonical_width, uint16_t canonical_height) {
    return xg_render_runtime_composition_configure_native_view(
        enabled, aspect_num, aspect_den, canonical_width, canonical_height);
}

void psx_xg_render_auth_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size)) {
    xg_render_runtime_composition_register_code_watches(set_range);
}

void psx_xg_render_auth_cold_enable(bool enabled) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_DISABLE,
    };

    g_psx_xg_render_auth_cold_enabled = enabled;
    if (!enabled) {
        xg_render_runtime_composition_disable();
        xg_render_runtime_composition_handle_invalidation(&event);
    }
}

bool psx_xg_render_auth_cold_enabled(void) {
    return g_psx_xg_render_auth_cold_enabled;
}

void psx_xg_render_auth_scene_boundary(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_SCENE_BOUNDARY,
    };

    if (xg_render_auth_process_owner(&state.auth) == XG_RENDER_AUTH_OK)
        (void)xg_render_auth_scene_reset(state.auth);
    xg_render_runtime_composition_handle_invalidation(&event);
    end_gte_attribution_producer();
    state.active = false;
    state.armed = true;
    state.completed = false;
    state.authenticated_producer_entry = 0u;
    state.authenticated_producer_scene_generation = 0u;
    state.rejection = (PsxXgRenderAuthRejectionReceipt){ 0 };
    if (state.scene_generation != UINT64_MAX) {
        ++state.scene_generation;
        xg_render_runtime_composition_scene_boundary(true);
    } else {
        xg_render_runtime_composition_scene_boundary(false);
    }
    clear_pending_candidate();
    clear_pending_variant_sequence();
    clear_candidate_outcome();
}

bool psx_xg_render_auth_cold_hook_relevant(uint32_t hook, uint32_t pc,
                                           uint32_t instruction_word) {
    const XgRenderHookRouteKind route =
        xg_render_runtime_composition_hook_route_kind(
            hook, pc, instruction_word);
    bool canonical = false;

    if (!g_psx_xg_render_auth_cold_enabled) return false;
    if (route == XG_RENDER_HOOK_ROUTE_CANONICAL_ENTRY ||
        route == XG_RENDER_HOOK_ROUTE_CANONICAL_CAPTURE)
        canonical = authenticated_variant_hook_matches(pc);
    else if (route == XG_RENDER_HOOK_ROUTE_CANONICAL_RETURN)
        canonical = state.active && authenticated_variant_hook_matches(pc);
    else if (route == XG_RENDER_HOOK_ROUTE_UI_DRAW_OT)
        canonical = true;
    return canonical || xg_render_runtime_variant_hook_relevant(hook, pc);
}

void psx_xg_render_auth_cold_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word) {
    if (!g_psx_xg_render_auth_cold_enabled) return;
    xg_render_instrumentation_record_cold_hook();
    observe_hook(cpu, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, hook, pc,
                 instruction_word, delay_slot_word,
                 cpu != NULL ? cpu->gpr[31] : 0u);
}

void psx_xg_render_auth_warm_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                                  uint32_t instruction_word,
                                  uint32_t delay_slot_word) {
    observe_hook(cpu, XG_RENDER_AUTH_TIER_WARM_NATIVE, hook, pc,
                 instruction_word, delay_slot_word,
                 cpu != NULL ? cpu->gpr[31] : 0u);
}

bool psx_xg_render_auth_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata) {
    if (!g_psx_xg_render_auth_cold_enabled ||
        !authenticated_variant_hook_matches(pc))
        return false;
    return xg_render_runtime_composition_source_site_lookup(
        pc, instruction_word, out_metadata);
}

bool psx_xg_render_auth_cold_source_pc_relevant(uint32_t pc) {
    return g_psx_xg_render_auth_cold_enabled &&
           authenticated_variant_hook_matches(pc) &&
           xg_render_runtime_composition_source_pc_relevant(pc);
}

uint32_t psx_xg_render_auth_cold_instruction_flags(
    uint32_t pc, uint32_t instruction_word) {
    enum { INVARIANT_CACHE_CAPACITY = 4096u };
    typedef struct {
        uint32_t pc;
        uint32_t instruction_word;
        uint32_t flags;
        bool valid;
    } XgRenderColdInvariantCacheEntry;
    static XgRenderColdInvariantCacheEntry
        invariant_cache[INVARIANT_CACHE_CAPACITY];
    XgRenderColdInvariantCacheEntry *cached;
    uint32_t flags;

    if (!g_psx_xg_render_auth_cold_enabled) return 0u;
    cached = &invariant_cache[(pc >> 2u) & (INVARIANT_CACHE_CAPACITY - 1u)];
    if (cached->valid && cached->pc == pc &&
        cached->instruction_word == instruction_word) {
        flags = cached->flags;
    } else {
        flags = 0u;
        if (psx_xg_render_auth_native_cutover_pc_relevant(pc))
            flags |= PSX_XG_RENDER_COLD_NATIVE_PRE;
        if (psx_xg_render_auth_native_cutover_post_pc_relevant(pc))
            flags |= PSX_XG_RENDER_COLD_NATIVE_POST;
        if (psx_xg_render_auth_overlay_cutover_relevant(pc, instruction_word))
            flags |= PSX_XG_RENDER_COLD_OVERLAY;
        *cached = (XgRenderColdInvariantCacheEntry){
            .pc = pc,
            .instruction_word = instruction_word,
            .flags = flags,
            .valid = true,
        };
    }
    if (psx_xg_render_auth_cold_hook_relevant(
            PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc, instruction_word))
        flags |= PSX_XG_RENDER_COLD_ENTRY;
    if (instruction_word >> 26u == 3u &&
        psx_xg_render_auth_cold_hook_relevant(
            PSX_XG_RENDER_AUTH_HOOK_CAPTURE, pc, instruction_word))
        flags |= PSX_XG_RENDER_COLD_CAPTURE;
    if (psx_xg_render_auth_cold_source_pc_relevant(pc))
        flags |= PSX_XG_RENDER_COLD_SOURCE;
    return flags;
}

bool psx_xg_render_auth_cold_source_observe(
    PsxXgRenderSourceStage stage, uint32_t pc, uint32_t instruction_word,
    uint32_t auxiliary) {
    if (!g_psx_xg_render_auth_cold_enabled) return false;
    return xg_render_runtime_composition_source_observe(
        NULL, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, stage, pc,
        instruction_word, auxiliary);
}

bool psx_xg_render_auth_cold_source_observe_cpu(
    CPUState *cpu, PsxXgRenderSourceStage stage, uint32_t pc,
    uint32_t instruction_word, uint32_t auxiliary) {
    if (!g_psx_xg_render_auth_cold_enabled) return false;
    return xg_render_runtime_composition_source_observe(
        cpu, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, stage, pc,
        instruction_word, auxiliary);
}

bool psx_xg_render_auth_native_ft4_bypass(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word) {
    return xg_render_runtime_composition_observe_dispatch(
        cpu, pc, instruction_word) == XG_RENDER_RUNTIME_COMPOSITION_BYPASS;
}

void psx_xg_render_auth_capture_model_ft3_link(CPUState *cpu) {
    xg_render_runtime_composition_capture_model_ft3_link(cpu);
}

bool psx_xg_render_auth_native_cutover_pc_relevant(uint32_t pc) {
    return xg_render_runtime_composition_cutover_pc_relevant(pc);
}

bool psx_xg_render_auth_overlay_cutover_relevant(
        uint32_t pc, uint32_t instruction_word) {
    return xg_render_runtime_composition_overlay_relevant(
        pc, instruction_word);
}

bool psx_xg_render_auth_native_cutover_post_pc_relevant(uint32_t pc) {
    return xg_render_runtime_composition_cutover_post_pc_relevant(pc);
}

static void invalidate_authenticated_authority(void) {
    if (state.authenticated_artifact_candidate_valid)
        clear_authenticated_artifact_candidate();
    state.authenticated_producer_entry = 0u;
    state.authenticated_producer_scene_generation = 0u;
}

void psx_xg_render_auth_note_code_write(uint64_t previous_generation,
                                           uint64_t current_generation,
                                           uint32_t guest_pc,
                                           uint32_t write_size) {
    XgRenderMutationClassification classification;
    XgRenderMutationContext mutation_context;

    if (xg_render_runtime_variant_no_gates_enabled()) return;
    mutation_context = (XgRenderMutationContext){
        .artifact_mutation =
            state.authenticated_artifact_candidate_valid &&
            !state.authenticated_artifact_candidate.runtime_variant_bound &&
            current_artifact_code_range_overlaps(guest_pc, write_size),
        .authentication_range_mutation =
            authentication_range_write_overlaps(guest_pc, write_size),
        .completed_authorization = state.completed,
    };
    xg_render_runtime_composition_classify_code_write(
        guest_pc, write_size, &mutation_context, &classification);
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_CODE_WRITE,
        .address = guest_pc,
        .size = write_size,
        .code_write_mask = classification.code_write_mask,
        .mutation = classification.properties,
    };

    if (event.mutation.authority_loss)
        invalidate_authenticated_authority();
    if (event.mutation.interpolation_reset)
        advance_interpolation_scene();
    xg_render_runtime_composition_handle_invalidation(&event);

    if (!event.mutation.watched_range_mutation &&
        !event.mutation.artifact_mutation &&
        !event.mutation.authentication_mutation &&
        !event.mutation.resource_mutation) {
        if (state.completed) retire_completed_auth_proof();
        return;
    }

    if (state.completed) {
        retire_completed_auth_proof();
        return;
    }
    if (!state.active) {
        if (!event.mutation.runtime_variant_mutation)
            return;
        if (state.pending_variant_sequence) {
            begin_scene(state.pending_variant_tier,
                        xg_render_manifest_validation.producer_entry);
            abort_active(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION,
                         false, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, guest_pc);
        } else {
            clear_pending_candidate();
            clear_pending_variant_sequence();
        }
        return;
    }
    if (!event.mutation.authentication_mutation) return;
    if (state.auth == NULL) {
        disarm();
        return;
    }
    if (xg_render_auth_note_code_page_mutation(state.auth, previous_generation,
                                               current_generation) !=
        XG_RENDER_AUTH_OK) {
        latch_rejection(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION,
                        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION,
                        false, PSX_XG_RENDER_AUTH_HOOK_CAPTURE, guest_pc);
        disarm();
    }
}

void psx_xg_render_auth_loader_mismatch(uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    if (field_range_contains(pc) || current_artifact_range_contains_pc(pc) ||
        xg_render_runtime_composition_authority_authorizes_pc(pc) ||
        xg_render_runtime_composition_pending_authorizes_pc(pc)) {
        const XgRenderInvalidationEvent event = {
            .kind = XG_RENDER_INVALIDATION_LOADER_MISMATCH,
            .address = pc,
        };

        advance_interpolation_scene();
        invalidate_authenticated_authority();
        xg_render_runtime_composition_handle_invalidation(&event);
        /* A rehash miss demotes compiled host code to authoritative guest
         * execution. It aborts the active proof, not resources observed from
         * that guest execution; code writes own their invalidation separately. */
        if (state.active || state.completed) {
            abort_active(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH,
                         PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH,
                         false, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
        } else {
            clear_pending_candidate();
            clear_pending_variant_sequence();
        }
    }
}

void psx_xg_render_auth_native_bad_entry(uint32_t owner, uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled()) return;
    if (physical_address_equals(owner,
                                xg_render_manifest_validation.producer_entry) ||
        field_range_contains(pc))
        abort_active(XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR,
                     PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NATIVE_BAD_ENTRY,
                     false, PSX_XG_RENDER_AUTH_HOOK_ENTRY, pc);
}

void psx_xg_render_auth_note_artifact_candidate(
    const PsxXgRenderAuthCandidate *candidate) {
    bool candidate_matches;

    if (candidate == NULL) return;
    candidate_matches =
        xg_render_runtime_variant_artifact_candidate_matches(candidate) ||
        (!candidate->runtime_variant_bound &&
         xg_render_authoritative_overlay_artifact_candidate_matches(candidate));
    if (!candidate_matches) {
        if (current_artifact_memory_contains_pc(candidate->dispatch_pc) &&
            !artifact_binary_identity_matches(
                &state.authenticated_artifact_candidate, candidate)) {
            broadcast_resource_overlap();
            clear_authenticated_artifact_candidate();
            state.authenticated_producer_entry = 0u;
            state.authenticated_producer_scene_generation = 0u;
        }
        return;
    }
    if (state.authenticated_artifact_candidate_valid &&
        state.authenticated_artifact_scene_generation == state.scene_generation &&
        artifact_binary_identity_matches(
            &state.authenticated_artifact_candidate, candidate)) {
        state.authenticated_artifact_candidate = *candidate;
        return;
    }
    if (state.authenticated_artifact_generation == UINT64_MAX) {
        clear_authenticated_artifact_candidate();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
        return;
    }
    if (state.authenticated_artifact_candidate_valid) {
        broadcast_resource_overlap();
        state.authenticated_producer_entry = 0u;
        state.authenticated_producer_scene_generation = 0u;
    }
    ++state.authenticated_artifact_generation;
    state.authenticated_artifact_candidate = *candidate;
    state.authenticated_artifact_candidate_valid = true;
    state.authenticated_artifact_scene_generation = state.scene_generation;
}

bool psx_xg_render_auth_authenticated_producer_entry(
    uint32_t *out_producer_entry) {
    if (out_producer_entry == NULL ||
        state.authenticated_producer_entry == 0u ||
        state.authenticated_producer_scene_generation != state.scene_generation)
        return false;
    *out_producer_entry = state.authenticated_producer_entry;
    return true;
}

void psx_xg_render_auth_note_candidate_dispatch(
    const PsxXgRenderAuthCandidate *candidate) {
    bool candidate_matches;

    psx_xg_render_auth_note_artifact_candidate(candidate);
    clear_pending_candidate();
    if (candidate == NULL || !state.armed)
        return;
    if (state.completed) retire_completed_auth_proof();
    if (state.active || !state.armed) return;
    candidate_matches = candidate->runtime_variant_bound
        ? xg_render_runtime_variant_candidate_matches(candidate) ||
              xg_render_runtime_variant_artifact_candidate_matches(candidate)
        : candidate_matches_manifest(candidate);
    if (!candidate_matches)
        return;
    state.pending_candidate = *candidate;
    state.pending_scene_generation = state.scene_generation;
    state.pending_candidate_valid = true;
}

void psx_xg_render_auth_provenance_snapshot(
    PsxXgRenderAuthProvenance *out_provenance) {
    bool identity_bound = false;
    bool identity_gate_passed = false;

    if (out_provenance == NULL) return;
    out_provenance->manifest_bound =
        xg_render_static_auth_metadata_is_valid() &&
        xg_render_static_auth_bind_identity(&identity_bound,
                                            &identity_gate_passed);
    out_provenance->range_bound = field_range_is_bound();
    out_provenance->candidate_matched = state.candidate_matched;
    out_provenance->candidate_dispatched = state.candidate_dispatched;
}

void psx_xg_render_auth_rejection_snapshot(
    PsxXgRenderAuthRejectionReceipt *out_receipt) {
    if (out_receipt != NULL) *out_receipt = state.rejection;
}

void psx_xg_render_auth_completed_proof_snapshot(
    PsxXgRenderAuthCompletedProofReceipt *out_receipt) {
    if (out_receipt == NULL) return;
    lock_completed_proof();
    *out_receipt = completed_proof;
    unlock_completed_proof();
}

void psx_xg_render_auth_instrumentation_snapshot(
    PsxXgRenderAuthInstrumentation *out_instrumentation) {
    xg_render_instrumentation_snapshot(out_instrumentation);
}

void psx_xg_render_auth_mode_snapshot(
    PsxXgRenderModeSnapshot *out_snapshot) {
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };
    GuestRenderBridgeSnapshot completed_bridge = { 0 };
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionSnapshot transaction_snapshot = { 0 };
    bool completed_authority;

    if (out_snapshot == NULL) return;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->modes.requested_render_mode = state.requested_render_mode;
    out_snapshot->modes.effective_render_mode = state.requested_render_mode;
    out_snapshot->presentation = state.presentation;
    lock_completed_proof();
    completed_authority = completed_proof.available && !completed_proof.blocked &&
        completed_proof_scene_generation == state.scene_generation;
    unlock_completed_proof();
    if (guest_render_bridge_snapshot(&bridge_snapshot) == GUEST_RENDER_OK) {
        out_snapshot->modes = bridge_snapshot.modes;
        out_snapshot->fallback_reason = bridge_snapshot.fallback_reason;
        out_snapshot->fallback_count = bridge_snapshot.fallback_count;
        if (!bridge_snapshot.state_open && !bridge_snapshot.producer_open &&
            bridge_snapshot.fallback_reason == GUEST_RENDER_FALLBACK_NONE &&
            bridge_snapshot.modes.effective_render_mode ==
                GUEST_RENDER_RENDER_ORIGINAL &&
            state.requested_render_mode != GUEST_RENDER_RENDER_ORIGINAL &&
            guest_render_bridge_last_completed(&completed_bridge, &completed) ==
                GUEST_RENDER_OK &&
            completed_bridge.fallback_reason == GUEST_RENDER_FALLBACK_NONE &&
            completed_bridge.modes.requested_render_mode ==
                state.requested_render_mode) {
            out_snapshot->modes = completed_bridge.modes;
        } else if (!bridge_snapshot.state_open &&
                   !bridge_snapshot.producer_open &&
                   bridge_snapshot.fallback_reason ==
                       GUEST_RENDER_FALLBACK_NONE &&
                   completed_authority) {
            out_snapshot->modes.requested_render_mode =
                state.requested_render_mode;
            out_snapshot->modes.effective_render_mode =
                state.requested_render_mode;
        }
    }
    if (guest_render_transaction_snapshot(&transaction_snapshot) ==
        GUEST_RENDER_TRANSACTION_OK) {
        out_snapshot->transaction_count =
            transaction_snapshot.published_transaction_count;
        out_snapshot->substitution_count =
            transaction_snapshot.published_substitution_count;
    }
}

void psx_xg_render_auth_reset(void) {
    const XgRenderInvalidationEvent event = {
        .kind = XG_RENDER_INVALIDATION_RESET,
    };

    guest_render_native_stream_set_enabled(false);
    end_gte_attribution_producer();
    if (state.auth != NULL) (void)xg_render_auth_scene_reset(state.auth);
    state = (XgRenderAuthRuntimeState){
        .armed = true,
        .scene_generation = 1u,
        .interpolation_scene_generation = 1u,
        .requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
    };
    g_psx_xg_render_auth_cold_enabled = false;
    gpu_native_interpolation_scene_boundary(interpolation_scene_generation());
    (void)xg_render_runtime_composition_configure(composition_services());
    xg_render_runtime_composition_configure_invalidation();
    xg_render_runtime_composition_handle_invalidation(&event);
    xg_render_runtime_composition_reset();
    completed_proof = (PsxXgRenderAuthCompletedProofReceipt){ 0 };
    completed_proof_scene_generation = 0u;
}

void psx_xg_render_auth_runtime_snapshot(
        PsxXgRenderAuthRuntimeSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = (PsxXgRenderAuthRuntimeSnapshot){
        .interpolation_scene_generation = interpolation_scene_generation(),
        .authenticated_artifact_generation =
            state.authenticated_artifact_generation,
        .authenticated_artifact_active = current_artifact_is_authorized(),
    };
}

const char *psx_xg_render_auth_rejection_source_name(uint32_t source) {
    switch (source) {
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE: return "none";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK: return "runtime_hook";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK: return "variant_hook";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH:
        return "loader_mismatch";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NATIVE_BAD_ENTRY:
        return "native_bad_entry";
    case PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION:
        return "code_page_mutation";
    }
    return "none";
}

const char *psx_xg_render_auth_hook_name(uint32_t hook) {
    switch (hook) {
    case PSX_XG_RENDER_AUTH_HOOK_PRODUCER_ENTRY: return "producer_entry";
    case PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION:
        return "internal_observation";
    case PSX_XG_RENDER_AUTH_HOOK_CONTINUATION: return "continuation";
    case PSX_XG_RENDER_AUTH_HOOK_PRODUCER_EXIT: return "producer_exit";
    case PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR: return "foreign_interior";
    case PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE: return "source_pre";
    case PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT: return "source_commit";
    }
    return "none";
}

void psx_xg_render_auth_set_terrain_temporal_coverage(bool enabled) {
    xg_render_runtime_composition_set_terrain_temporal_coverage(enabled);
}

void psx_xg_render_auth_set_exec_phase_exchange(
        PsxXgRenderExecPhaseExchange exchange) {
    xg_render_runtime_composition_set_exec_phase_exchange(exchange);
}

void psx_xg_render_auth_before_gpu_submission(void) {
    xg_render_runtime_composition_before_gpu_submission();
}

void psx_xg_render_auth_note_gpu_semantic_current(
        const GpuRenderSemantic *semantic) {
    xg_render_runtime_composition_note_gpu_semantic_current(semantic);
}

void psx_xg_render_auth_complete_gpu_source_frame(void) {
    xg_render_runtime_composition_complete_gpu_source_frame();
}

bool psx_xg_render_auth_prepare_ui_ot(uint32_t start_addr) {
    return xg_render_runtime_composition_prepare_ui_ot(start_addr);
}

void psx_xg_render_auth_ui_ot_snapshot(
        PsxXgRenderUiOtSnapshot *out_snapshot) {
    xg_render_runtime_composition_ui_ot_snapshot(out_snapshot);
}

void psx_xg_render_auth_source_snapshot(
        PsxXgRenderSourceSnapshot *out_snapshot) {
    xg_render_runtime_composition_source_snapshot(out_snapshot);
}

void psx_xg_render_auth_source_collector_snapshot(
        FieldCharacterShadowSummary *out_summary) {
    xg_render_runtime_composition_source_collector_snapshot(out_summary);
}

void psx_xg_render_auth_source_reset(void) {
    xg_render_runtime_composition_source_reset();
}

void psx_xg_render_auth_ft4_geometry_enable(bool enabled) {
    xg_render_runtime_composition_ft4_geometry_enable(enabled);
}

void psx_xg_render_auth_capture_clear_tile(CPUState *cpu) {
    xg_render_runtime_composition_capture_clear_tile(cpu);
}

void psx_xg_render_auth_capture_logo_sprite(
        uint32_t command_address, uint8_t color) {
    xg_render_runtime_composition_capture_logo_sprite(command_address, color);
}

void psx_xg_render_auth_capture_tile_write(
        CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
        uint8_t color) {
    xg_render_runtime_composition_capture_tile_write(
        cpu, command_address, writer_pc, color);
}

bool psx_xg_render_auth_resident_ft4_observe(
        CPUState *cpu, uint32_t stage, uint32_t pc,
        uint32_t instruction_word) {
    return xg_render_runtime_composition_resident_ft4_observe(
        cpu, stage, pc, instruction_word);
}

bool psx_xg_render_auth_ft4_geometry_pop(
        PsxXgRenderFt4Geometry *out_geometry) {
    return xg_render_runtime_composition_ft4_geometry_pop(out_geometry);
}

void psx_xg_render_auth_ft4_geometry_snapshot(
        PsxXgRenderFt4GeometrySnapshot *out_snapshot) {
    xg_render_runtime_composition_ft4_geometry_snapshot(out_snapshot);
}

void psx_xg_render_auth_zoom_template_contract_snapshot(
        PsxXgRenderZoomTemplateContractSnapshot *out_snapshot) {
    xg_render_runtime_composition_zoom_template_contract_snapshot(out_snapshot);
}

void psx_xg_render_auth_overlay_ft4_snapshot(
        PsxXgRenderOverlayFt4Snapshot *out_snapshot) {
    xg_render_runtime_composition_overlay_ft4_snapshot(out_snapshot);
}

void psx_xg_render_auth_producer_family_enable(bool enabled) {
    xg_render_runtime_composition_enable_producer_family(enabled);
}

void psx_xg_render_auth_producer_family_snapshot(
        PsxXgRenderProducerFamilySnapshot *out_snapshot) {
    xg_render_runtime_composition_producer_family_snapshot(out_snapshot);
}

void psx_xg_render_auth_projected_lifecycle_snapshot(
        PsxXgRenderProjectedLifecycleSnapshot *out_snapshot) {
    xg_render_runtime_composition_projected_lifecycle_snapshot(out_snapshot);
}

void psx_xg_render_auth_model_ft4_shadow_snapshot(
        PsxXgRenderModelFt4ShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_model_ft4_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_model_ft3_shadow_snapshot(
        PsxXgRenderModelFt3ShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_model_ft3_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_sprite_ft4_shadow_snapshot(
        PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_sprite_ft4_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_field_polyline_snapshot(
        PsxXgRenderFieldPolylineSnapshot *out_snapshot) {
    xg_render_runtime_composition_field_polyline_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_horizon_shadow_snapshot(
        PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_horizon_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_effects_shadow_snapshot(
        PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_effects_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_terrain_water_shadow_snapshot(
        PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_terrain_water_shadow_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_entity_shadows_shadow_snapshot(
        PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_entity_shadows_shadow_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_decorations_shadow_snapshot(
        PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_decorations_shadow_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_clouds_shadow_snapshot(
        PsxXgRenderWorldCloudsShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_clouds_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_minimap_shadow_snapshot(
        PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_minimap_shadow_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_models_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_models_native_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_actor_sprites_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_actor_sprites_native_snapshot(
        out_snapshot);
}

void psx_xg_render_auth_world_sky_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_sky_native_snapshot(out_snapshot);
}

void psx_xg_render_auth_world_execution_snapshot(
        PsxXgRenderWorldExecutionSnapshot *out_snapshot) {
    xg_render_runtime_composition_world_execution_snapshot(out_snapshot);
}
