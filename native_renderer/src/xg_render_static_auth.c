#include "xg_render_static_auth.h"

#include "xg_render_auth.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_static_auth_metadata.h"

#include <string.h>

typedef struct XgRenderStaticAuthState {
    XgRenderStaticAuthTraceEvent trace[XG_RENDER_STATIC_AUTH_TRACE_CAPACITY];
    uint64_t next_sequence;
    size_t trace_write_index;
    size_t trace_count;
    XgRenderStaticAuthStatus status;
    GuestRenderRenderMode effective_render_mode;
    bool identity_bound;
    bool identity_gate_passed;
    bool trace_proven;
    bool auth_scene_selected;
} XgRenderStaticAuthState;

static XgRenderStaticAuthState state = {
    .next_sequence = 1u,
    .status = XG_RENDER_STATIC_AUTH_UNINITIALIZED,
    .effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL,
};

static bool is_control_transfer(uint32_t instruction) {
    const uint32_t opcode = instruction >> 26u;
    const uint32_t function = instruction & 0x3fu;

    return opcode == 1u || opcode == 2u || opcode == 3u ||
           (opcode >= 4u && opcode <= 7u) ||
           (opcode >= 0x14u && opcode <= 0x17u) ||
           (opcode == 0u && (function == 8u || function == 9u));
}

static void record_hook(XgRenderStaticAuthHook hook,
                        uint32_t producer_entry,
                        uint32_t caller_site,
                        uint32_t callee_entry,
                        uint32_t return_site,
                        bool accepted) {
    XgRenderStaticAuthTraceEvent *event = &state.trace[state.trace_write_index];

    *event = (XgRenderStaticAuthTraceEvent){
        state.next_sequence, hook, producer_entry, caller_site, callee_entry,
        return_site, accepted,
    };
    state.trace_write_index = (state.trace_write_index + 1u) %
                              XG_RENDER_STATIC_AUTH_TRACE_CAPACITY;
    if (state.trace_count < XG_RENDER_STATIC_AUTH_TRACE_CAPACITY)
        ++state.trace_count;
    if (state.next_sequence != UINT64_MAX)
        ++state.next_sequence;
}

static const XgRenderStaticAuthTraceEvent *trace_at(size_t index) {
    const size_t first = (state.trace_write_index + XG_RENDER_STATIC_AUTH_TRACE_CAPACITY -
                          state.trace_count) % XG_RENDER_STATIC_AUTH_TRACE_CAPACITY;
    return &state.trace[(first + index) % XG_RENDER_STATIC_AUTH_TRACE_CAPACITY];
}

static bool has_exact_trace_proof(void) {
    const XgRenderManifestValidation *validation = &xg_render_manifest_validation;
    const XgRenderStaticAuthTraceEvent *entry;
    const XgRenderStaticAuthTraceEvent *capture;
    const XgRenderStaticAuthTraceEvent *returned;

    if (state.trace_count < 3u) return false;
    entry = trace_at(state.trace_count - 3u);
    capture = trace_at(state.trace_count - 2u);
    returned = trace_at(state.trace_count - 1u);
    return entry->accepted && capture->accepted && returned->accepted &&
           entry->hook == XG_RENDER_STATIC_AUTH_HOOK_ENTRY &&
           capture->hook == XG_RENDER_STATIC_AUTH_HOOK_CAPTURE &&
           returned->hook == XG_RENDER_STATIC_AUTH_HOOK_RETURN &&
           entry->producer_entry == validation->producer_entry &&
           capture->caller_site == validation->caller_site &&
           capture->callee_entry == validation->static_callee &&
           capture->return_site == validation->return_site &&
           returned->return_site == validation->return_site;
}

static void select_scene_boundary(void) {
    const GuestRenderSceneConfig original = {
        GUEST_RENDER_TIMING_ORIGINAL, GUEST_RENDER_RENDER_ORIGINAL,
    };
    const GuestRenderSceneConfig native = {
        GUEST_RENDER_TIMING_NATIVE_59_94, GUEST_RENDER_RENDER_NATIVE,
    };
    const GuestRenderSceneConfig *config = &original;
    XgRenderAuth *auth = NULL;
    GuestRenderVisualStateId state_id = { 0 };
    XgRenderAuthProfile profile;

    state.auth_scene_selected = false;
    state.effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    if (!xg_render_static_auth_metadata_is_valid() ||
        !xg_render_static_auth_bind_identity(&state.identity_bound,
                                             &state.identity_gate_passed))
        state.status = XG_RENDER_STATIC_AUTH_IDENTITY_MISMATCH;
    else if (!has_exact_trace_proof())
        state.status = XG_RENDER_STATIC_AUTH_TRACE_UNPROVEN;
    else
        config = &native;
    (void)xg_render_auth_process_owner(&auth);
    if (auth != NULL) (void)xg_render_auth_scene_reset(auth);
    if (guest_render_bridge_begin_scene(config) != GUEST_RENDER_OK) {
        state.status = XG_RENDER_STATIC_AUTH_AUTH_REJECTED;
        return;
    }
    if (config == &original) return;
    profile = xg_render_static_auth_profile_from_metadata();
    if (guest_render_bridge_begin_state(&state_id) != GUEST_RENDER_OK ||
        xg_render_auth_scene_begin(auth, state_id, &profile) != XG_RENDER_AUTH_OK) {
        guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
        state.status = XG_RENDER_STATIC_AUTH_AUTH_REJECTED;
        return;
    }
    state.auth_scene_selected = true;
    state.status = XG_RENDER_STATIC_AUTH_SELECTED;
}

static void observe_selected(XgRenderAuthHook hook) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthDecision decision = { 0 };
    XgRenderAuthExecution execution = xg_render_static_auth_execution_for(hook);

    if (!state.auth_scene_selected) return;
    (void)xg_render_auth_process_owner(&auth);
    if (auth == NULL || xg_render_auth_observe_hook(auth, &execution, &decision) !=
                            XG_RENDER_AUTH_OK) {
        state.auth_scene_selected = false;
        state.effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
        state.status = XG_RENDER_STATIC_AUTH_AUTH_REJECTED;
        return;
    }
    state.effective_render_mode = decision.effective_render_mode;
}

static void observe_selected_trace(void) {
    observe_selected(XG_RENDER_AUTH_HOOK_ENTRY);
    observe_selected(XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
    observe_selected(XG_RENDER_AUTH_HOOK_RETURN);
}

static void discard_selected_scene(void) {
    XgRenderAuth *auth = NULL;

    if (!state.auth_scene_selected) return;
    (void)xg_render_auth_process_owner(&auth);
    if (auth != NULL) (void)xg_render_auth_scene_reset(auth);
    state.auth_scene_selected = false;
    state.effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
}

void psx_xg_render_static_auth_entry(uint32_t producer_entry) {
    const bool accepted = xg_render_static_auth_metadata_is_valid() &&
                          xg_render_static_auth_bind_identity(&state.identity_bound,
                                                               &state.identity_gate_passed) &&
                          producer_entry == xg_render_manifest_validation.producer_entry;

    select_scene_boundary();
    record_hook(XG_RENDER_STATIC_AUTH_HOOK_ENTRY, producer_entry, 0u, 0u, 0u,
                accepted);
}

void psx_xg_render_static_auth_capture(uint32_t caller_site,
                                       uint32_t callee_entry,
                                       uint32_t return_site,
                                       uint32_t instruction_word,
                                       uint32_t delay_slot_word) {
    const XgRenderManifestValidation *validation = &xg_render_manifest_validation;
    const bool site_matches = xg_render_static_auth_metadata_is_valid() &&
        caller_site == validation->caller_site && callee_entry == validation->static_callee &&
        return_site == validation->return_site &&
        (instruction_word >> 26u) == validation->required_jal_opcode &&
        ((caller_site & 0xf0000000u) | ((instruction_word & 0x03ffffffu) << 2u)) ==
            validation->jal_target;
    const bool accepted = state.identity_gate_passed && site_matches &&
                          !is_control_transfer(delay_slot_word);

    record_hook(XG_RENDER_STATIC_AUTH_HOOK_CAPTURE, 0u, caller_site, callee_entry,
                return_site, accepted);
    if (!site_matches) {
        discard_selected_scene();
        state.status = XG_RENDER_STATIC_AUTH_SITE_MISMATCH;
    } else if (!accepted) {
        discard_selected_scene();
        state.status = XG_RENDER_STATIC_AUTH_DELAY_SLOT_MISMATCH;
    }
}

void psx_xg_render_static_auth_return(uint32_t return_site,
                                      uint32_t return_address) {
    const bool accepted = state.identity_gate_passed &&
                          xg_render_static_auth_metadata_is_valid() &&
                          return_site == xg_render_manifest_validation.return_site &&
                          (return_address & 0x1fffffffu) ==
                              (return_site & 0x1fffffffu) &&
                          state.trace_count >= 2u && trace_at(state.trace_count - 1u)->accepted;

    record_hook(XG_RENDER_STATIC_AUTH_HOOK_RETURN, 0u, 0u, 0u, return_site,
                accepted);
    state.trace_proven = has_exact_trace_proof();
    if (!accepted || !state.trace_proven) {
        discard_selected_scene();
        return;
    }
    if (state.auth_scene_selected) observe_selected_trace();
}

XgRenderStaticAuthResult xg_render_static_auth_snapshot(
    XgRenderStaticAuthSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return XG_RENDER_STATIC_AUTH_INVALID_ARGUMENT;
    *out_snapshot = (XgRenderStaticAuthSnapshot){
        state.status, state.effective_render_mode, state.identity_bound,
        state.identity_gate_passed, state.trace_proven, state.auth_scene_selected,
        state.trace_count,
    };
    return XG_RENDER_STATIC_AUTH_OK;
}

XgRenderStaticAuthResult xg_render_static_auth_trace_snapshot(
    XgRenderStaticAuthTraceSnapshot *out_snapshot) {
    size_t index;

    if (out_snapshot == NULL) return XG_RENDER_STATIC_AUTH_INVALID_ARGUMENT;
    out_snapshot->next_sequence = state.next_sequence;
    out_snapshot->count = state.trace_count;
    for (index = 0u; index < state.trace_count; ++index)
        out_snapshot->events[index] = *trace_at(index);
    return XG_RENDER_STATIC_AUTH_OK;
}

const char *xg_render_static_auth_status_name(XgRenderStaticAuthStatus status) {
    switch (status) {
    case XG_RENDER_STATIC_AUTH_UNINITIALIZED: return "uninitialized";
    case XG_RENDER_STATIC_AUTH_IDENTITY_MISMATCH: return "identity_mismatch";
    case XG_RENDER_STATIC_AUTH_TRACE_UNPROVEN: return "trace_unproven";
    case XG_RENDER_STATIC_AUTH_SITE_MISMATCH: return "site_mismatch";
    case XG_RENDER_STATIC_AUTH_DELAY_SLOT_MISMATCH: return "delay_slot_mismatch";
    case XG_RENDER_STATIC_AUTH_AUTH_REJECTED: return "auth_rejected";
    case XG_RENDER_STATIC_AUTH_SELECTED: return "selected";
    }
    return "unknown";
}
