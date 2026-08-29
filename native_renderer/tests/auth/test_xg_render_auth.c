#include "guest_render_bridge.h"
#include "xg_render_auth.h"
#include "xg_render_ir.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

typedef enum FixtureMutation {
    FIXTURE_MUTATE_BASE_CRC,
    FIXTURE_MUTATE_RANGE_CRC,
    FIXTURE_MUTATE_PAGE_GENERATION,
    FIXTURE_MUTATE_WINDOW_DIGEST,
    FIXTURE_MUTATE_WINDOW_START,
    FIXTURE_MUTATE_WINDOW_SIZE,
    FIXTURE_MUTATE_CALLER,
    FIXTURE_MUTATE_CALLEE,
    FIXTURE_MUTATE_RETURN,
    FIXTURE_MUTATE_JAL_OPCODE,
    FIXTURE_MUTATE_JAL_TARGET,
    FIXTURE_MUTATE_DELAY_SLOT_COUNT,
    FIXTURE_MUTATE_DELAY_SLOT_CONTROL,
    FIXTURE_MUTATE_DELAY_SLOT_COMPLETE,
    FIXTURE_MUTATE_STATIC_IDENTITY,
    FIXTURE_MUTATE_RUNTIME_IDENTITY,
    FIXTURE_MUTATE_FIELD_IDENTITY,
    FIXTURE_MUTATE_MANIFEST_DIGEST,
    FIXTURE_MUTATE_CODEGEN_DIGEST,
} FixtureMutation;

static XgRenderAuthDigest make_digest(uint8_t domain) {
    XgRenderAuthDigest digest = { { 0 } };

    digest.bytes[0] = domain;
    digest.bytes[7] = (uint8_t)(domain + 1u);
    digest.bytes[19] = (uint8_t)(domain + 2u);
    digest.bytes[31] = (uint8_t)(domain + 3u);
    return digest;
}

static XgRenderAuthIdentity make_identity(uint32_t namespace_id, uint8_t domain) {
    XgRenderAuthIdentity identity = { 0 };

    identity.namespace_id = namespace_id;
    identity.full_sha256 = make_digest(domain);
    return identity;
}

static XgRenderAuthValidation make_validation(void) {
    XgRenderAuthValidation validation = { 0 };

    validation.base_crc32 = UINT32_C(0x10203040);
    validation.range_crc32 = UINT32_C(0x50607080);
    validation.code_page_generation = 9u;
    validation.instruction_window_digest = make_digest(0x21u);
    validation.instruction_window_start = UINT32_C(0x00001000);
    validation.instruction_window_size = 16u;
    validation.caller_site = UINT32_C(0x00001004);
    validation.callee_entry = UINT32_C(0x00002000);
    validation.return_site = UINT32_C(0x0000100c);
    validation.required_jal_opcode = 3u;
    validation.jal_target = UINT32_C(0x00002000);
    validation.required_delay_slot_instructions = 1u;
    validation.delay_slot_complete = true;
    validation.delay_slot_is_control_transfer = false;
    validation.manifest_digest = make_digest(0x31u);
    return validation;
}

static XgRenderAuthCacheIdentity make_cache_identity(void) {
    XgRenderAuthCacheIdentity cache_identity = { 0 };

    cache_identity.codegen_digest = make_digest(0x41u);
    cache_identity.manifest_digest = make_digest(0x31u);
    return cache_identity;
}

static XgRenderAuthProfile make_profile(void) {
    XgRenderAuthProfile profile = { 0 };

    profile.producer_record_id = XG_RENDER_AUTH_PRODUCER_RECORD_ID;
    profile.site_record_id = XG_RENDER_AUTH_SITE_RECORD_ID;
    profile.producer_entry = UINT32_C(0x00007634);
    profile.static_game_identity = make_identity(UINT32_C(0x10203040), 0x11u);
    profile.field_image_identity = make_identity(UINT32_C(0x50607080), 0x12u);
    profile.validation = make_validation();
    profile.cache_identity = make_cache_identity();
    return profile;
}

static XgRenderAuthExecution make_execution(XgRenderAuthTier tier,
                                             XgRenderAuthHook hook) {
    XgRenderAuthExecution execution = { 0 };

    execution.tier = tier;
    execution.hook = hook;
    execution.observed_producer_entry = UINT32_C(0x00007634);
    execution.static_game_identity = make_identity(UINT32_C(0x10203040), 0x11u);
    execution.runtime_game_identity = make_identity(UINT32_C(0x10203040), 0x11u);
    execution.field_image_identity = make_identity(UINT32_C(0x50607080), 0x12u);
    execution.validation = make_validation();
    execution.cache_identity = make_cache_identity();
    return execution;
}

static int digest_equal(const XgRenderAuthDigest *left,
                        const XgRenderAuthDigest *right) {
    return memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static int identity_equal(const XgRenderAuthIdentity *left,
                          const XgRenderAuthIdentity *right) {
    return left->namespace_id == right->namespace_id &&
           digest_equal(&left->full_sha256, &right->full_sha256);
}

static GuestRenderProducerProvenance native_provenance(void) {
    GuestRenderProducerProvenance provenance = { 0 };

    provenance.tier = GUEST_RENDER_PRODUCER_NATIVE;
    return provenance;
}

static int begin_bridge_state(GuestRenderVisualStateId *out_state_id) {
    const GuestRenderSceneConfig config = {
        GUEST_RENDER_RENDER_NATIVE,
    };
    GuestRenderVisualStateId state_id = { 0 };

    CHECK(guest_render_bridge_begin_scene(&config) == GUEST_RENDER_OK);
    CHECK(guest_render_bridge_begin_state(&state_id) == GUEST_RENDER_OK);
    *out_state_id = state_id;
    return 1;
}

static int begin_scene(XgRenderAuth *auth,
                       const XgRenderAuthProfile *profile,
                       GuestRenderVisualStateId *out_state_id) {
    GuestRenderVisualStateId state_id = { 0 };

    CHECK(begin_bridge_state(&state_id));
    CHECK(xg_render_auth_scene_begin(auth, state_id, profile) == XG_RENDER_AUTH_OK);
    *out_state_id = state_id;
    return 1;
}

static int observe(XgRenderAuth *auth,
                   const XgRenderAuthExecution *execution,
                   GuestRenderRenderMode expected_render_mode,
                   int expected_native_use) {
    XgRenderAuthDecision decision = { 0 };

    CHECK(xg_render_auth_observe_hook(auth, execution, &decision) == XG_RENDER_AUTH_OK);
    CHECK(decision.effective_render_mode == expected_render_mode);
    CHECK(decision.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(decision.native_use_permitted == expected_native_use);
    return 1;
}

static int observe_tier_hook(XgRenderAuth *auth,
                             XgRenderAuthTier tier,
                             XgRenderAuthHook hook,
                             GuestRenderRenderMode expected_render_mode,
                             int expected_native_use) {
    XgRenderAuthExecution execution = make_execution(tier, hook);

    return observe(auth, &execution, expected_render_mode, expected_native_use);
}

static int assert_ir_phase(XgRenderIrPhase expected_phase, int expected_usable) {
    XgRenderIr *ir = NULL;
    XgRenderIrSnapshot snapshot = { 0 };

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == expected_phase);
    CHECK(snapshot.usable == expected_usable);
    return 1;
}

static int assert_ir_native_count(size_t expected_native_count) {
    XgRenderIr *ir = NULL;
    XgRenderIrSnapshot snapshot = { 0 };

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.native_count == expected_native_count);
    return 1;
}

static int assert_scene_abort(XgRenderAuth *auth,
                              XgRenderAuthRejectReason reason,
                              size_t expected_producer_begins) {
    XgRenderAuthSnapshot snapshot = { 0 };
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };

    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.reject_reason == reason);
    CHECK(snapshot.scene_aborted);
    CHECK(snapshot.producer_begin_count == expected_producer_begins);
    CHECK(snapshot.native_item_count == 0u);
    CHECK(!snapshot.ir_usable);
    CHECK(!snapshot.native_use_permitted);
    CHECK(guest_render_bridge_snapshot(&bridge_snapshot) == GUEST_RENDER_OK);
    CHECK(!bridge_snapshot.state_open);
    CHECK(!bridge_snapshot.producer_open);
    CHECK(bridge_snapshot.slot_count == 0u);
    CHECK(bridge_snapshot.binding_count == 0u);
    CHECK(bridge_snapshot.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(assert_ir_phase(XG_RENDER_IR_EMPTY, 0));
    return 1;
}

static int run_valid_tier(XgRenderAuth *auth,
                          XgRenderAuthTier tier,
                          const XgRenderAuthProfile *profile,
                          XgRenderAuthSnapshot *out_snapshot) {
    GuestRenderVisualStateId state_id = { 0 };
    GuestRenderBridgeSnapshot bridge_snapshot = { 0 };
    XgRenderAuthSnapshot snapshot = { 0 };

    CHECK(begin_scene(auth, profile, &state_id));
    CHECK(observe_tier_hook(auth, tier, XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(guest_render_bridge_snapshot(&bridge_snapshot) == GUEST_RENDER_OK);
    CHECK(bridge_snapshot.state_open);
    CHECK(bridge_snapshot.producer_open);
    CHECK(bridge_snapshot.slot_count == 1u);
    CHECK(assert_ir_phase(XG_RENDER_IR_BUILDING, 0));
    CHECK(observe_tier_hook(auth, tier, XG_RENDER_AUTH_HOOK_CAPTURE_SITE,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_bind_packet(auth, UINT32_C(0x80000100), 1u) ==
          XG_RENDER_AUTH_OK);
    CHECK(guest_render_bridge_snapshot(&bridge_snapshot) == GUEST_RENDER_OK);
    CHECK(bridge_snapshot.producer_open);
    CHECK(bridge_snapshot.binding_count == 1u);
    CHECK(assert_ir_native_count(0u));
    CHECK(observe_tier_hook(auth, tier, XG_RENDER_AUTH_HOOK_RETURN,
                            GUEST_RENDER_RENDER_NATIVE, 1));
    CHECK(guest_render_bridge_snapshot(&bridge_snapshot) == GUEST_RENDER_OK);
    CHECK(!bridge_snapshot.state_open);
    CHECK(!bridge_snapshot.producer_open);
    CHECK(bridge_snapshot.slot_count == 1u);
    CHECK(bridge_snapshot.binding_count == 1u);
    CHECK(assert_ir_phase(XG_RENDER_IR_FINALIZED, 1));
    CHECK(assert_ir_native_count(0u));
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(snapshot.native_item_count == 1u);
    CHECK(snapshot.ir_usable);
    CHECK(snapshot.native_use_permitted);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(snapshot.hook_sequence[0] == XG_RENDER_AUTH_HOOK_ENTRY);
    CHECK(snapshot.hook_sequence[1] == XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
    CHECK(snapshot.hook_sequence[2] == XG_RENDER_AUTH_HOOK_RETURN);
    CHECK(guest_render_bridge_id_equal(snapshot.logical_identity.state_id, state_id));
    *out_snapshot = snapshot;
    return 1;
}

static int test_no_bindings_finalize_as_empty_native_transaction(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    XgRenderAuthSnapshot snapshot = { 0 };
    GuestRenderVisualStateId state_id = { 0 };

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_bind_packet(auth, UINT32_C(0x80000100), 11u) ==
          XG_RENDER_AUTH_INVALID_TRANSITION);
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_CAPTURE_SITE,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(assert_ir_native_count(0u));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_RETURN,
                            GUEST_RENDER_RENDER_NATIVE, 0));
    CHECK(assert_ir_phase(XG_RENDER_IR_FINALIZED, 1));
    CHECK(assert_ir_native_count(0u));
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(snapshot.native_item_count == 0u);
    CHECK(!snapshot.native_use_permitted);
    return 1;
}

static int test_multiple_packet_bindings_complete_exact_count(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    XgRenderAuthSnapshot snapshot = { 0 };
    GuestRenderBridgeSnapshot bridge = { 0 };
    GuestRenderVisualStateId state_id = { 0 };

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_CAPTURE_SITE,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_bind_packet(auth, UINT32_C(0x80000100), 11u) ==
          XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_bind_packet(auth, UINT32_C(0xa0000120), 12u) ==
          XG_RENDER_AUTH_OK);
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_RETURN,
                            GUEST_RENDER_RENDER_NATIVE, 1));
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.native_item_count == 2u);
    CHECK(snapshot.native_use_permitted);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(!bridge.state_open && !bridge.producer_open);
    CHECK(bridge.binding_count == 2u);
    return 1;
}

static int test_static_cold_warm_share_identity_and_real_lifecycle(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    XgRenderAuthSnapshot static_snapshot = { 0 };
    XgRenderAuthSnapshot cold_snapshot = { 0 };
    XgRenderAuthSnapshot warm_snapshot = { 0 };

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(run_valid_tier(auth, XG_RENDER_AUTH_TIER_STATIC, &profile,
                         &static_snapshot));
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(run_valid_tier(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, &profile,
                         &cold_snapshot));
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(run_valid_tier(auth, XG_RENDER_AUTH_TIER_WARM_NATIVE, &profile,
                         &warm_snapshot));
    CHECK(identity_equal(&static_snapshot.field_image_identity,
                         &cold_snapshot.field_image_identity));
    CHECK(identity_equal(&cold_snapshot.field_image_identity,
                         &warm_snapshot.field_image_identity));
    CHECK(memcmp(static_snapshot.hook_sequence, cold_snapshot.hook_sequence,
                 sizeof(static_snapshot.hook_sequence)) == 0);
    CHECK(memcmp(cold_snapshot.hook_sequence, warm_snapshot.hook_sequence,
                 sizeof(cold_snapshot.hook_sequence)) == 0);
    return 1;
}

static void mutate_execution(XgRenderAuthExecution *execution,
                             FixtureMutation mutation) {
    switch (mutation) {
    case FIXTURE_MUTATE_BASE_CRC:
        execution->validation.base_crc32++;
        break;
    case FIXTURE_MUTATE_RANGE_CRC:
        execution->validation.range_crc32++;
        break;
    case FIXTURE_MUTATE_PAGE_GENERATION:
        execution->validation.code_page_generation++;
        break;
    case FIXTURE_MUTATE_WINDOW_DIGEST:
        execution->validation.instruction_window_digest.bytes[0]++;
        break;
    case FIXTURE_MUTATE_WINDOW_START:
        execution->validation.instruction_window_start += 4u;
        break;
    case FIXTURE_MUTATE_WINDOW_SIZE:
        execution->validation.instruction_window_size -= 4u;
        break;
    case FIXTURE_MUTATE_CALLER:
        execution->validation.caller_site += 4u;
        break;
    case FIXTURE_MUTATE_CALLEE:
        execution->validation.callee_entry += 4u;
        break;
    case FIXTURE_MUTATE_RETURN:
        execution->validation.return_site += 4u;
        break;
    case FIXTURE_MUTATE_JAL_OPCODE:
        execution->validation.required_jal_opcode = 2u;
        break;
    case FIXTURE_MUTATE_JAL_TARGET:
        execution->validation.jal_target += 4u;
        break;
    case FIXTURE_MUTATE_DELAY_SLOT_COUNT:
        execution->validation.required_delay_slot_instructions = 2u;
        break;
    case FIXTURE_MUTATE_DELAY_SLOT_CONTROL:
        execution->validation.delay_slot_is_control_transfer = true;
        break;
    case FIXTURE_MUTATE_DELAY_SLOT_COMPLETE:
        execution->validation.delay_slot_complete = false;
        break;
    case FIXTURE_MUTATE_STATIC_IDENTITY:
        execution->static_game_identity.full_sha256.bytes[31]++;
        break;
    case FIXTURE_MUTATE_RUNTIME_IDENTITY:
        execution->runtime_game_identity.full_sha256.bytes[31]++;
        break;
    case FIXTURE_MUTATE_FIELD_IDENTITY:
        execution->field_image_identity.full_sha256.bytes[31]++;
        break;
    case FIXTURE_MUTATE_MANIFEST_DIGEST:
        execution->validation.manifest_digest.bytes[31]++;
        break;
    case FIXTURE_MUTATE_CODEGEN_DIGEST:
        execution->cache_identity.codegen_digest.bytes[31]++;
        break;
    }
}

static XgRenderAuthRejectReason reason_for_mutation(FixtureMutation mutation) {
    switch (mutation) {
    case FIXTURE_MUTATE_STATIC_IDENTITY:
    case FIXTURE_MUTATE_RUNTIME_IDENTITY:
    case FIXTURE_MUTATE_FIELD_IDENTITY:
        return XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH;
    case FIXTURE_MUTATE_CODEGEN_DIGEST:
        return XG_RENDER_AUTH_REJECT_CACHE_IDENTITY_MISMATCH;
    default:
        return XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH;
    }
}

static int test_validation_rejects_after_entry_producer_begin(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    FixtureMutation mutation;

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    for (mutation = FIXTURE_MUTATE_BASE_CRC;
         mutation <= FIXTURE_MUTATE_CODEGEN_DIGEST;
         mutation++) {
        XgRenderAuthExecution execution =
            make_execution(XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                           XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
        XgRenderAuthDecision decision = { 0 };
        GuestRenderVisualStateId state_id = { 0 };

        CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
        CHECK(begin_scene(auth, &profile, &state_id));
        CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                XG_RENDER_AUTH_HOOK_ENTRY,
                                GUEST_RENDER_RENDER_ORIGINAL, 0));
        mutate_execution(&execution, mutation);
        CHECK(xg_render_auth_observe_hook(auth, &execution, &decision) ==
              XG_RENDER_AUTH_REJECTED);
        CHECK(decision.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
        CHECK(decision.reject_reason == reason_for_mutation(mutation));
        CHECK(assert_scene_abort(auth, reason_for_mutation(mutation), 1u));
    }
    return 1;
}

static int test_zero_or_collision_provenance_rejects(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    XgRenderAuthExecution execution =
        make_execution(XG_RENDER_AUTH_TIER_STATIC, XG_RENDER_AUTH_HOOK_ENTRY);
    GuestRenderVisualStateId state_id = { 0 };
    XgRenderAuthDecision decision = { 0 };

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    execution.observed_producer_entry = 0u;
    CHECK(xg_render_auth_observe_hook(auth, &execution, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    execution = make_execution(XG_RENDER_AUTH_TIER_STATIC, XG_RENDER_AUTH_HOOK_ENTRY);
    memset(execution.field_image_identity.full_sha256.bytes, 0,
           sizeof(execution.field_image_identity.full_sha256.bytes));
    CHECK(xg_render_auth_observe_hook(auth, &execution, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH, 0u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    execution = make_execution(XG_RENDER_AUTH_TIER_STATIC, XG_RENDER_AUTH_HOOK_ENTRY);
    execution.field_image_identity = execution.static_game_identity;
    CHECK(xg_render_auth_observe_hook(auth, &execution, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_IDENTITY_MISMATCH, 0u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_bridge_state(&state_id));
    profile.producer_entry = 0u;
    CHECK(xg_render_auth_scene_begin(auth, state_id, &profile) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));
    return 1;
}

static int test_zero_profile_validation_fields_reject(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    GuestRenderVisualStateId state_id = { 0 };

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_bridge_state(&state_id));
    profile.validation.instruction_window_start = 0u;
    CHECK(xg_render_auth_scene_begin(auth, state_id, &profile) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));

    profile = make_profile();
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_bridge_state(&state_id));
    profile.validation.instruction_window_size = 0u;
    CHECK(xg_render_auth_scene_begin(auth, state_id, &profile) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));

    profile = make_profile();
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_bridge_state(&state_id));
    profile.validation.required_jal_opcode = 0u;
    CHECK(xg_render_auth_scene_begin(auth, state_id, &profile) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));

    profile = make_profile();
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_bridge_state(&state_id));
    profile.validation.jal_target = 0u;
    CHECK(xg_render_auth_scene_begin(auth, state_id, &profile) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));

    profile = make_profile();
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_bridge_state(&state_id));
    profile.validation.required_delay_slot_instructions = 0u;
    CHECK(xg_render_auth_scene_begin(auth, state_id, &profile) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));
    return 1;
}

static int test_real_bridge_and_ir_failures_abort(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    GuestRenderVisualStateId state_id = { 0 };
    GuestRenderProducerHandle handle = { 0 };
    GuestRenderProducerProvenance provenance = native_provenance();
    XgRenderAuthExecution capture =
        make_execution(XG_RENDER_AUTH_TIER_STATIC,
                       XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
    XgRenderAuthExecution returned =
        make_execution(XG_RENDER_AUTH_TIER_STATIC, XG_RENDER_AUTH_HOOK_RETURN);
    XgRenderAuthDecision decision = { 0 };
    XgRenderIr *ir = NULL;

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(guest_render_bridge_producer_begin(state_id, &provenance, &handle) ==
          GUEST_RENDER_OK);
    CHECK(xg_render_auth_observe_hook(
              auth, &(XgRenderAuthExecution){
                  .tier = XG_RENDER_AUTH_TIER_STATIC,
                  .hook = XG_RENDER_AUTH_HOOK_ENTRY,
                  .observed_producer_entry = UINT32_C(0x00007634),
                  .static_game_identity = make_identity(UINT32_C(0x10203040), 0x11u),
                  .runtime_game_identity = make_identity(UINT32_C(0x10203040), 0x11u),
                  .field_image_identity = make_identity(UINT32_C(0x50607080), 0x12u),
                  .validation = make_validation(),
                  .cache_identity = make_cache_identity(),
              }, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE, 0u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(xg_render_ir_begin(ir, state_id, profile.validation.code_page_generation) ==
          XG_RENDER_IR_OK);
    CHECK(xg_render_auth_observe_hook(
              auth, &(XgRenderAuthExecution){
                  .tier = XG_RENDER_AUTH_TIER_STATIC,
                  .hook = XG_RENDER_AUTH_HOOK_ENTRY,
                  .observed_producer_entry = UINT32_C(0x00007634),
                  .static_game_identity = make_identity(UINT32_C(0x10203040), 0x11u),
                  .runtime_game_identity = make_identity(UINT32_C(0x10203040), 0x11u),
                  .field_image_identity = make_identity(UINT32_C(0x50607080), 0x12u),
                  .validation = make_validation(),
                  .cache_identity = make_cache_identity(),
              }, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE, 0u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    CHECK(observe(auth, &capture, GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_observe_hook(auth, &returned, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE, 1u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_STATIC,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(observe(auth, &capture, GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_observe_hook(auth, &returned, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_TRANSACTION_FAILURE, 1u));
    return 1;
}

static int test_mutation_and_foreign_interior_abort_every_active_phase(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthProfile profile = make_profile();
    XgRenderAuthExecution foreign =
        make_execution(XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                       XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR);
    XgRenderAuthDecision decision = { 0 };
    GuestRenderVisualStateId state_id = { 0 };
    XgRenderAuthSnapshot snapshot = { 0 };

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(xg_render_auth_note_code_page_mutation(auth, 9u, 10u) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 0u));
    CHECK(xg_render_auth_observe_hook(auth, &foreign, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION);
    CHECK(xg_render_auth_scene_begin(auth, state_id, &profile) ==
          XG_RENDER_AUTH_INVALID_TRANSITION);

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_note_code_page_mutation(auth, 9u, 10u) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 1u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                            XG_RENDER_AUTH_HOOK_CAPTURE_SITE,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_note_code_page_mutation(auth, 9u, 10u) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 1u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(run_valid_tier(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, &profile,
                         &snapshot));
    CHECK(xg_render_auth_note_code_page_mutation(auth, 9u, 10u) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 1u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(xg_render_auth_observe_hook(auth, &foreign, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR, 0u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_observe_hook(auth, &foreign, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR, 1u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(begin_scene(auth, &profile, &state_id));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                            XG_RENDER_AUTH_HOOK_ENTRY,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(observe_tier_hook(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                            XG_RENDER_AUTH_HOOK_CAPTURE_SITE,
                            GUEST_RENDER_RENDER_ORIGINAL, 0));
    CHECK(xg_render_auth_observe_hook(auth, &foreign, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR, 1u));

    CHECK(xg_render_auth_scene_reset(auth) == XG_RENDER_AUTH_OK);
    CHECK(run_valid_tier(auth, XG_RENDER_AUTH_TIER_COLD_INTERPRETER, &profile,
                         &snapshot));
    CHECK(xg_render_auth_observe_hook(auth, &foreign, &decision) ==
          XG_RENDER_AUTH_REJECTED);
    CHECK(assert_scene_abort(auth, XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR, 1u));
    return 1;
}

int main(void) {
    if (!test_no_bindings_finalize_as_empty_native_transaction()) return 1;
    if (!test_multiple_packet_bindings_complete_exact_count()) return 1;
    if (!test_static_cold_warm_share_identity_and_real_lifecycle()) return 1;
    if (!test_validation_rejects_after_entry_producer_begin()) return 1;
    if (!test_zero_or_collision_provenance_rejects()) return 1;
    if (!test_zero_profile_validation_fields_reject()) return 1;
    if (!test_real_bridge_and_ir_failures_abort()) return 1;
    if (!test_mutation_and_foreign_interior_abort_every_active_phase()) return 1;
    return 0;
}
