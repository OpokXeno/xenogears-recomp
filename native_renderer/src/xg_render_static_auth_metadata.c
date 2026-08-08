#include "xg_render_static_auth_metadata.h"

#include "game_identity.h"
#include "xg_render_manifest_generated.h"

#include <string.h>

static const XgRenderManifestRecord *record_for(uint32_t record_id) {
    uint32_t index;

    for (index = 0u; index < xg_render_manifest_record_count; ++index) {
        if (xg_render_manifest_records[index].record_id == record_id)
            return &xg_render_manifest_records[index];
    }
    return NULL;
}

bool xg_render_static_auth_metadata_is_valid(void) {
    const XgRenderManifestValidation *validation = &xg_render_manifest_validation;
    const XgRenderManifestRecord *producer = record_for(validation->producer_record_id);
    const XgRenderManifestRecord *site = record_for(validation->site_record_id);

    return producer != NULL && site != NULL &&
           producer->address == validation->producer_entry &&
           site->address == validation->caller_site &&
           site->target_address == validation->static_callee &&
           validation->return_site == validation->caller_site + 8u &&
           validation->required_jal_opcode == 3u &&
           validation->jal_target == validation->static_callee &&
           validation->required_delay_slot_instructions == 1u &&
           validation->required_delay_slot_non_control_transfer == 1u;
}

bool xg_render_static_auth_bind_identity(bool *out_bound, bool *out_gate_passed) {
    PsxGameIdentity expected = { { 0 }, { 0 } };
    const PsxGameIdentity *runtime = psx_game_identity_runtime();
    bool bound;

    memcpy(expected.game_sha256, xg_render_game_identity,
           sizeof(expected.game_sha256));
    memcpy(expected.manifest_sha256, xg_render_manifest_identity,
           sizeof(expected.manifest_sha256));
    bound = runtime != NULL && psx_game_identity_equal(runtime, &expected) &&
            psx_game_identity_bind_static(&expected);
    *out_bound = bound;
    *out_gate_passed = bound && psx_game_identity_gate(&expected);
    return *out_gate_passed;
}

XgRenderAuthProfile xg_render_static_auth_profile_from_metadata(void) {
    const XgRenderManifestValidation *validation = &xg_render_manifest_validation;
    const XgRenderManifestRecord *producer = record_for(validation->producer_record_id);
    XgRenderAuthProfile profile = { 0 };

    profile.producer_record_id = validation->producer_record_id;
    profile.site_record_id = validation->site_record_id;
    profile.producer_entry = validation->producer_entry;
    profile.static_game_identity.namespace_id = xg_render_namespace_crc32;
    memcpy(profile.static_game_identity.full_sha256.bytes, xg_render_game_identity,
           sizeof(profile.static_game_identity.full_sha256.bytes));
    profile.field_image_identity.namespace_id = xg_render_namespace_crc32;
    memcpy(profile.field_image_identity.full_sha256.bytes, producer->image_identity,
           sizeof(profile.field_image_identity.full_sha256.bytes));
    profile.validation = (XgRenderAuthValidation){
        validation->field_base_crc32, validation->field_range_crc32, 1u,
        { { 0 } }, validation->instruction_window_start,
        validation->instruction_window_size, validation->caller_site,
        validation->static_callee, validation->return_site,
        validation->required_jal_opcode, validation->jal_target,
        validation->required_delay_slot_instructions, true,
        validation->required_delay_slot_non_control_transfer == 0u, { { 0 } },
    };
    memcpy(profile.validation.instruction_window_digest.bytes,
           validation->instruction_window_identity,
           sizeof(profile.validation.instruction_window_digest.bytes));
    memcpy(profile.validation.manifest_digest.bytes, xg_render_manifest_identity,
           sizeof(profile.validation.manifest_digest.bytes));
    profile.cache_identity.codegen_digest = profile.static_game_identity.full_sha256;
    profile.cache_identity.manifest_digest = profile.validation.manifest_digest;
    return profile;
}

XgRenderAuthExecution xg_render_static_auth_execution_for(XgRenderAuthHook hook) {
    XgRenderAuthProfile profile = xg_render_static_auth_profile_from_metadata();
    XgRenderAuthExecution execution = { 0 };

    execution.tier = XG_RENDER_AUTH_TIER_STATIC;
    execution.hook = hook;
    execution.observed_producer_entry = profile.producer_entry;
    execution.static_game_identity = profile.static_game_identity;
    execution.runtime_game_identity = profile.static_game_identity;
    execution.field_image_identity = profile.field_image_identity;
    execution.validation = profile.validation;
    execution.cache_identity = profile.cache_identity;
    return execution;
}
