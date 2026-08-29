#include "xg_render_runtime_composition.h"

#include "guest_render_bridge.h"
#include "guest_render_native_stream.h"
#include "guest_render_transaction.h"
#include "gpu.h"
#include "xg_field_compass.h"
#include "xg_field_particles.h"
#include "xg_field_projected.h"
#include "xg_field_render_services.h"
#include "xg_field_zoom.h"
#include "xg_host_3d.h"
#include "xg_native_view.h"
#include "xg_render_f4_sources.h"
#include "xg_render_cutover_dispatch.h"
#include "xg_render_field_character_pipeline.h"
#include "xg_render_field_polyline.h"
#include "xg_render_field_sprite.h"
#include "xg_render_invalidation_dispatch.h"
#include "xg_render_invalidation_modules.h"
#include "xg_render_local_producer_auth.h"
#include "xg_render_model_repository.h"
#include "xg_render_model_sprite_pipeline.h"
#include "xg_render_mutation_classifier.h"
#include "xg_render_overlay_ft4.h"
#include "xg_render_overlay_cutovers_generated.h"
#include "xg_render_producer_lifecycle.h"
#include "xg_render_residual.h"
#include "xg_render_resident_line_f2.h"
#include "xg_render_resolver_registry.h"
#include "xg_render_resource_watch.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_render_shared_packet_resolver.h"
#include "xg_render_static_auth_metadata.h"
#include "xg_render_submission.h"
#include "xg_render_ui_ot.h"
#include "xg_render_world_coordinator.h"
#include "xg_render_world_execution.h"
#include "xg_render_world_models_pipeline.h"
#include "xg_render_world_pending_services.h"
#include "xg_render_world_simple_producers.h"
#include "xg_render_world_sky_producer.h"
#include "xg_world_clouds_shadow.h"
#include "xg_world_models_native.h"

#include <string.h>

static XgRenderRuntimeAuthSceneServices auth_scene;
static bool configured;
static bool local_auth_configured;
static uint64_t next_resource_generation = 1u;
static XgNativeView native_view;

#ifdef XG_RENDER_RUNTIME_COMPOSITION_TESTING
static bool test_registration_failure_enabled;
static uint32_t test_registration_failure_after;

void xg_render_runtime_composition_test_fail_registration_after(
        uint32_t successful_registrations) {
    test_registration_failure_enabled = true;
    test_registration_failure_after = successful_registrations;
}

void xg_render_runtime_composition_test_clear_registration_failure(void) {
    test_registration_failure_enabled = false;
}
#endif

static void query_state(XgRenderRuntimeAuthSceneState *out_state) {
    if (out_state == NULL) return;
    *out_state = (XgRenderRuntimeAuthSceneState){0};
    if (configured && auth_scene.query_state != NULL)
        auth_scene.query_state(out_state);
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool guest_data_range_is_valid(uint32_t address, uint32_t size,
                                      uint32_t alignment,
                                      bool allow_scratchpad) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint64_t physical = address & UINT32_C(0x1fffffff);
    const bool valid_segment = segment == 0u ||
        segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);
    const bool in_ram = physical + size <= UINT64_C(0x00200000);
    const bool in_scratchpad = allow_scratchpad &&
        physical >= UINT32_C(0x1f800000) &&
        physical + size <= UINT64_C(0x1f800400);

    return size != 0u && alignment != 0u && valid_segment &&
        (address & (alignment - 1u)) == 0u &&
        (uint64_t)address + size - 1u <= UINT32_MAX &&
        (in_ram || in_scratchpad);
}

static uint64_t interpolation_scene_generation(void) {
    XgRenderRuntimeAuthSceneState state;

    query_state(&state);
    return state.interpolation_generation != 0u
        ? state.interpolation_generation : 1u;
}

static int32_t screen_x_cull_margin(void) {
    const int32_t native_margin = xg_host_3d_native_view_margin();
    const int32_t temporal_margin = psx_ws_x_margin();

    return native_margin > temporal_margin ? native_margin : temporal_margin;
}

static void watch_resource(uint32_t address, uint32_t size) {
    xg_render_resource_watch_add(address, size);
}

static void watch_model_ft3_descriptor(uint32_t address, uint32_t size) {
    xg_render_resource_watch_add_model_ft3_descriptor(address, size);
}

static uint64_t local_generation_for_pc(uint32_t pc) {
    XgRenderRuntimeAuthSceneState state;

    query_state(&state);
    return xg_render_local_producer_auth_generation_for_pc(
        pc, state.scene_generation);
}

static bool local_authority_matches(uint32_t pc, uint64_t generation) {
    XgRenderRuntimeAuthSceneState state;

    query_state(&state);
    return xg_render_local_producer_auth_matches(
        pc, generation, state.scene_generation);
}

static uint64_t artifact_generation_for_pc(uint32_t pc) {
    XgRenderRuntimeAuthSceneState state;

    query_state(&state);
    if (auth_scene.artifact_authorizes_pc != NULL &&
        auth_scene.artifact_authorizes_pc(pc))
        return state.artifact_generation;
    return local_generation_for_pc(pc);
}

static bool resident_lifecycle_pc(uint32_t pc) {
    static const uint32_t resident_pcs[] = {
        UINT32_C(0x8002675c),
        UINT32_C(0x800273c4),
        UINT32_C(0x80045ed0),
    };

    if (xg_render_model_sprite_pipeline_resident_lifecycle_pc(pc)) return true;
    for (size_t index = 0u;
         index < sizeof(resident_pcs) / sizeof(resident_pcs[0]); ++index)
        if (physical_address_equals(pc, resident_pcs[index])) return true;
    return false;
}

static bool lifecycle_begin(uint32_t producer_pc,
                            XgRenderProducerLifecycle *out_lifecycle) {
    XgRenderRuntimeAuthSceneState state;
    const bool resident = resident_lifecycle_pc(producer_pc);
    uint64_t local_generation;
    uint64_t artifact_generation;

    query_state(&state);
    local_generation = resident || xg_render_runtime_variant_no_gates_enabled()
        ? 0u : local_generation_for_pc(producer_pc);
    artifact_generation = resident ||
            xg_render_runtime_variant_no_gates_enabled()
        ? 0u
        : (local_generation != 0u ? local_generation
                                  : artifact_generation_for_pc(producer_pc));
    if (out_lifecycle == NULL || next_resource_generation == 0u ||
        (!resident && !xg_render_runtime_variant_no_gates_enabled() &&
         artifact_generation == 0u))
        return false;
    *out_lifecycle = (XgRenderProducerLifecycle){
        .artifact_generation = artifact_generation,
        .resource_generation = next_resource_generation++,
        .scene_generation = state.scene_generation,
        .producer_pc = xg_render_runtime_guest_address(producer_pc),
        .scene_resource = resident ? 0u :
            (xg_render_runtime_variant_no_gates_enabled() ? 2u :
             (local_generation != 0u ? 3u : 1u)),
    };
    return true;
}

static bool lifecycle_matches(const XgRenderProducerLifecycle *lifecycle) {
    XgRenderRuntimeAuthSceneState state;

    query_state(&state);
    if (lifecycle == NULL || lifecycle->resource_generation == 0u)
        return false;
    if (lifecycle->scene_resource == 0u)
        return lifecycle->artifact_generation == 0u &&
            resident_lifecycle_pc(lifecycle->producer_pc);
    if (lifecycle->scene_resource == 2u)
        return lifecycle->artifact_generation == 0u &&
            lifecycle->scene_generation == state.scene_generation &&
            xg_render_runtime_variant_no_gates_enabled();
    if (lifecycle->scene_resource == 3u)
        return lifecycle->artifact_generation != 0u &&
            lifecycle->scene_generation == state.scene_generation &&
            local_authority_matches(lifecycle->producer_pc,
                                    lifecycle->artifact_generation);
    return lifecycle->scene_resource == 1u &&
        lifecycle->artifact_generation != 0u &&
        lifecycle->artifact_generation == state.artifact_generation &&
        lifecycle->scene_generation == state.scene_generation &&
        auth_scene.artifact_is_authorized != NULL &&
        auth_scene.artifact_is_authorized();
}

static bool replay_container_matches_command(
        const GuestRenderNativeStreamMissContext *context) {
    return context != NULL &&
        (context->source_kind !=
             GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST ||
         (context->command_id >= 4u &&
          physical_address_equals((uint32_t)context->container_id,
                                  (uint32_t)context->command_id - 4u)));
}

static bool lifecycle_matches_replay(
        const XgRenderProducerLifecycle *lifecycle,
        const GuestRenderNativeStreamMissContext *context) {
    return context != NULL && context->command_id != 0u &&
        (context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_MMIO ||
         context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK ||
         context->source_kind ==
             GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST ||
         context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BURST) &&
        lifecycle_matches(lifecycle);
}

static const XgRenderProducerLifecycleServices *lifecycle_services(void) {
    static const XgRenderProducerLifecycleServices services = {
        .begin = lifecycle_begin,
        .matches = lifecycle_matches,
        .matches_replay = lifecycle_matches_replay,
        .replay_container_matches_command = replay_container_matches_command,
        .guest_data_range_is_valid = guest_data_range_is_valid,
    };
    return &services;
}

static void register_model_ft3_source(uint32_t source_id) {
    xg_render_resolver_registry_hint_put(source_id, XG_NATIVE_RESOLVE_MODEL_FT3);
}

static void register_model_ft4_source(uint32_t source_id) {
    xg_render_resolver_registry_hint_put(source_id, XG_NATIVE_RESOLVE_MODEL_FT4);
}

static const XgRenderModelRepositoryServices *model_repository_services(void) {
    static XgRenderModelRepositoryServices services;

    services = (XgRenderModelRepositoryServices){
        .lifecycle = lifecycle_services(),
        .submission = {
            .register_ft3_replay_source = register_model_ft3_source,
            .register_ft4_replay_source = register_model_ft4_source,
            .interpolation_scene = interpolation_scene_generation,
            .record_interpolation_anchors =
                xg_render_submission_record_interpolation_anchors,
        },
        .resources = {
            .watch = watch_resource,
            .watch_ft3_descriptor = watch_model_ft3_descriptor,
            .ft3_descriptor_overlaps =
                xg_render_resource_watch_model_ft3_descriptor_overlaps,
            .reset_ft3_descriptors =
                xg_render_resource_watch_reset_model_ft3_descriptors,
        },
    };
    return &services;
}

static const XgRenderModelSpritePipelineServices *model_sprite_services(void) {
    static XgRenderModelSpritePipelineServices services;

    services = (XgRenderModelSpritePipelineServices){
        .lifecycle = lifecycle_services(),
        .repository = model_repository_services(),
        .screen_x_cull_margin = screen_x_cull_margin,
        .pre_scene_available = xg_render_submission_pre_scene_available,
        .stage_pre_scene = xg_render_submission_pre_scene_stage,
        .stage_standalone =
            xg_render_submission_stage_standalone_primitive_with_detail,
        .watch_resource = watch_resource,
    };
    return &services;
}

static void register_polyline_command(uint32_t command_id) {
    xg_render_resolver_registry_hint_put(
        command_id, XG_NATIVE_RESOLVE_FIELD_POLYLINE);
}

static const XgRenderFieldPolylineServices *polyline_services(void) {
    static const XgRenderFieldPolylineServices services = {
        .guest_data_range_is_valid = guest_data_range_is_valid,
        .stage_semantic =
            xg_render_submission_stage_standalone_semantic_identified,
        .register_replay_command = register_polyline_command,
        .interpolation_scene = interpolation_scene_generation,
        .replay_container_matches_command = replay_container_matches_command,
    };
    return &services;
}

static const XgRenderResidentLineF2Services *resident_line_services(void) {
    static const XgRenderResidentLineF2Services services = {
        .word_address_is_valid = xg_render_runtime_word_address_is_valid,
        .guest_data_range_is_valid = guest_data_range_is_valid,
        .stage_semantic =
            xg_render_submission_stage_standalone_semantic_identified,
        .abort_submission = xg_render_submission_standalone_abort,
    };
    return &services;
}

static const XgRenderF4SourceServices *f4_services(void) {
    static XgRenderF4SourceServices services;

    services = (XgRenderF4SourceServices){
        .lifecycle = lifecycle_services(),
        .guest_memory = {
            .data_range_is_valid = guest_data_range_is_valid,
            .word_address_is_valid = xg_render_runtime_word_address_is_valid,
            .stack_address_is_valid = xg_render_runtime_stack_address_is_valid,
            .vector_address_is_valid = xg_render_runtime_vector_address_is_valid,
            .capture_projection = xg_render_runtime_capture_shadow_projection,
        },
        .interpolation = {
            .scene_generation = interpolation_scene_generation,
        },
        .resources = { .watch = watch_resource },
    };
    return &services;
}

static void register_field_sprite_command(uint32_t command_id) {
    xg_render_resolver_registry_hint_put(
        command_id, XG_NATIVE_RESOLVE_FIELD_SPRITE);
}

static const XgRenderOverlayFt4Services *overlay_services(void);
static const XgRenderFieldSpriteServices *field_sprite_services(void);

static void invalidate_field_sprite_template(uint32_t packet_address) {
    xg_render_field_sprite_invalidate_template(
        packet_address, field_sprite_services());
}

static const XgRenderOverlayFt4Services *overlay_services(void) {
    static XgRenderOverlayFt4Services services;

    services = (XgRenderOverlayFt4Services){
        .lifecycle = lifecycle_services(),
        .local_producer_generation = local_generation_for_pc,
        .guest_data_range_is_valid = guest_data_range_is_valid,
        .stage_primitive =
            xg_render_submission_stage_standalone_primitive_identified,
        .invalidate_field_sprite_template = invalidate_field_sprite_template,
        .watch_resource = watch_resource,
    };
    return &services;
}

static bool publish_field_sprite_overlay(
        const XgRenderFieldSpriteOverlayPublication *publication,
        uint32_t *failure_detail) {
    return xg_render_overlay_ft4_publish_field_sprite(
        publication, overlay_services(), failure_detail);
}

static const XgRenderFieldSpriteServices *field_sprite_services(void) {
    static XgRenderFieldSpriteServices services;

    services = (XgRenderFieldSpriteServices){
        .lifecycle = lifecycle_services(),
        .stage_primitive =
            xg_render_submission_stage_standalone_primitive_with_detail,
        .publish_overlay = publish_field_sprite_overlay,
        .register_replay_command = register_field_sprite_command,
        .watch_resource = watch_resource,
        .interpolation_scene = interpolation_scene_generation,
    };
    return &services;
}

static bool source_context_matches(XgRenderAuthTier tier,
                                   uint32_t *out_context_bits) {
    XgRenderRuntimeAuthSceneState state;
    bool identity_bound = false;
    bool identity_gate_passed = false;
    bool proof_context;
    bool candidate_context;
    bool pending_context;
    bool static_valid;
    bool identity_valid;

    query_state(&state);
    candidate_context = tier == XG_RENDER_AUTH_TIER_COLD_INTERPRETER ||
        (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE &&
         ((state.active && state.candidate_matched &&
           state.candidate_dispatched) ||
          (auth_scene.artifact_is_authorized != NULL &&
           auth_scene.artifact_is_authorized())));
    proof_context = auth_scene.completed_proof_matches_tier != NULL &&
        auth_scene.completed_proof_matches_tier(tier);
    pending_context = state.active && state.pending_sequence &&
        state.pending_tier == tier && candidate_context;
    static_valid = xg_render_static_auth_metadata_is_valid();
    identity_valid = xg_render_static_auth_bind_identity(
        &identity_bound, &identity_gate_passed);
    if (out_context_bits != NULL)
        *out_context_bits =
            (state.armed ? 1u : 0u) |
            (state.pending_sequence ? 2u : 0u) |
            (state.pending_capture_ready ? 4u : 0u) |
            (state.pending_tier == tier ? 8u : 0u) |
            (candidate_context ? 16u : 0u) |
            (state.completed ? 32u : 0u) |
            (proof_context ? 64u : 0u) |
            (static_valid ? 128u : 0u) |
            (identity_valid ? 256u : 0u);
    return state.armed &&
        (xg_render_runtime_variant_no_gates_enabled() ||
         pending_context || proof_context) &&
        static_valid && identity_valid;
}

static bool capture_auth_context(XgFieldCharacterSourceCaptureRequest *request) {
    XgRenderRuntimeAuthSceneState state;
    XgRenderAuthSnapshot snapshot = {0};

    if (request == NULL || auth_scene.auth_snapshot == NULL ||
        !auth_scene.auth_snapshot(&snapshot))
        return false;
    query_state(&state);
    request->scene_generation = state.scene_generation;
    request->visual_state.scene_epoch =
        snapshot.logical_identity.state_id.scene_epoch;
    request->visual_state.state_sequence =
        snapshot.logical_identity.state_id.state_sequence;
    request->producer_record_id =
        snapshot.logical_identity.producer_record_id;
    request->producer_entry = state.pending_producer_entry;
    return true;
}

static uint64_t field_character_scene_generation(void) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return state.scene_generation;
}

static void reject_auth(void) {
    if (auth_scene.reject_auth != NULL) auth_scene.reject_auth(0u);
}

static const XgRenderFieldCharacterPipelineServices *field_character_services(void) {
    static const XgRenderFieldCharacterPipelineServices services = {
        .source_context_matches = source_context_matches,
        .source_site_lookup = xg_render_runtime_variant_source_site_lookup,
        .capture_auth_context = capture_auth_context,
        .scene_generation = field_character_scene_generation,
        .interpolation_scene_generation = interpolation_scene_generation,
        .stage_candidate = xg_render_submission_stage_field_character,
        .reject_auth = reject_auth,
    };
    return &services;
}

static void reject_family(uint32_t blocker) {
    xg_render_field_character_reject(blocker, field_character_services());
}

static bool submission_auth_available(void) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return state.active && !state.completed;
}

static bool submission_auth_snapshot(GpuRenderTransactionId *out_visual_id) {
    XgRenderAuthSnapshot snapshot = {0};

    if (out_visual_id == NULL || auth_scene.auth_snapshot == NULL ||
        !auth_scene.auth_snapshot(&snapshot))
        return false;
    *out_visual_id = (GpuRenderTransactionId){
        snapshot.logical_identity.state_id.scene_epoch,
        snapshot.logical_identity.state_id.state_sequence,
    };
    return true;
}

static bool submission_scene_config(GuestRenderSceneConfig *out_config) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    if (out_config == NULL || state.render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    *out_config = (GuestRenderSceneConfig){
        state.render_mode,
    };
    return true;
}

static const XgRenderSubmissionServices *submission_services(void) {
    static XgRenderSubmissionServices services;

    services = (XgRenderSubmissionServices){
        .active_auth_available = submission_auth_available,
        .active_auth_snapshot = submission_auth_snapshot,
        .active_auth_append = auth_scene.append_authenticated_ir,
        .standalone_scene_config = submission_scene_config,
        .presentation_gate = auth_scene.presentation_gate,
        .interpolation_generation = interpolation_scene_generation,
    };
    return &services;
}

static bool local_overlay_preflight(
        const XgRenderLocalProducerAuthContext *context) {
    return context != NULL && xg_render_overlay_ft4_local_producer_preflight(
        context->cpu, context->render_mode, (uint8_t)context->handler_data,
        overlay_services());
}

static bool local_overlay_begin(
        const XgRenderLocalProducerAuthContext *context) {
    return context != NULL && xg_render_overlay_ft4_local_producer_begin(
        context->cpu, context->render_mode, context->generation,
        (uint8_t)context->handler_data, overlay_services());
}

static bool local_overlay_writer(
        const XgRenderLocalProducerAuthContext *context,
        uint32_t writer_index) {
    return context != NULL && xg_render_overlay_ft4_local_producer_writer(
        context->cpu, writer_index, (uint8_t)context->handler_data);
}

static bool local_overlay_commit(
        const XgRenderLocalProducerAuthContext *context) {
    return context != NULL && xg_render_overlay_ft4_local_producer_commit(
        context->cpu, context->generation, context->pc,
        (uint8_t)context->handler_data, overlay_services());
}

static bool local_zoom_preflight(
        const XgRenderLocalProducerAuthContext *context) {
    return context != NULL && context->cpu != NULL &&
        context->render_mode == GUEST_RENDER_RENDER_NATIVE &&
        xg_render_runtime_stack_address_is_valid(context->cpu->gpr[29]);
}

static bool local_zoom_begin(
        const XgRenderLocalProducerAuthContext *context) {
    return context != NULL && xg_field_zoom_local_producer_begin(
        context->cpu, context->render_mode, context->generation);
}

static bool local_zoom_writer(
        const XgRenderLocalProducerAuthContext *context,
        uint32_t writer_index) {
    return context != NULL && xg_field_zoom_local_producer_writer(
        context->cpu, writer_index);
}

static bool local_zoom_commit(
        const XgRenderLocalProducerAuthContext *context) {
    static const XgFieldZoomLocalProducerServices services = {
        .watch_resource = watch_resource,
    };
    return context != NULL && xg_field_zoom_local_producer_commit(
        context->cpu, context->generation, &services);
}

static const XgRenderLocalProducerAuthHandler local_auth_handlers[] = {
    [XG_RENDER_LOCAL_PRODUCER_FT4_2C] = {
        local_overlay_preflight, local_overlay_begin, local_overlay_writer,
        local_overlay_commit, xg_render_overlay_ft4_local_producer_cancel,
    },
    [XG_RENDER_LOCAL_PRODUCER_FT4_2E] = {
        local_overlay_preflight, local_overlay_begin, local_overlay_writer,
        local_overlay_commit, xg_render_overlay_ft4_local_producer_cancel,
    },
    [XG_RENDER_LOCAL_PRODUCER_ZOOM] = {
        local_zoom_preflight, local_zoom_begin, local_zoom_writer,
        local_zoom_commit, xg_field_zoom_reset_pending,
    },
};

static void unconfigure_local_auth(void) {
    if (!local_auth_configured) return;
    for (uint32_t kind = XG_RENDER_LOCAL_PRODUCER_FT4_2C;
         kind < XG_RENDER_LOCAL_PRODUCER_COUNT; ++kind)
        (void)xg_render_local_producer_auth_unregister(
            (XgRenderLocalProducerKind)kind, &local_auth_handlers[kind]);
    local_auth_configured = false;
}

static bool configure_local_auth(uint32_t *successful_registrations) {
    uint32_t kind;

    if (local_auth_configured) return true;
    for (kind = XG_RENDER_LOCAL_PRODUCER_FT4_2C;
         kind < XG_RENDER_LOCAL_PRODUCER_COUNT; ++kind) {
#ifdef XG_RENDER_RUNTIME_COMPOSITION_TESTING
        if (test_registration_failure_enabled &&
            *successful_registrations == test_registration_failure_after)
            goto rollback;
#endif
        if (!xg_render_local_producer_auth_register(
                (XgRenderLocalProducerKind)kind,
                &local_auth_handlers[kind]))
            goto rollback;
        ++*successful_registrations;
    }
    local_auth_configured = true;
    return true;

rollback:
    while (kind > XG_RENDER_LOCAL_PRODUCER_FT4_2C) {
        --kind;
        (void)xg_render_local_producer_auth_unregister(
            (XgRenderLocalProducerKind)kind, &local_auth_handlers[kind]);
    }
    return false;
}

static bool observe_local_producer(CPUState *cpu, uint32_t pc,
                                   uint32_t instruction_word) {
    XgRenderRuntimeVariantCutover cutover;
    XgRenderRuntimeAuthSceneState state;

    uint32_t successful_registrations = 0u;
    if (!configure_local_auth(&successful_registrations) ||
        !xg_render_runtime_variant_native_cutover_contract_lookup(
            pc, instruction_word, &cutover))
        return false;
    query_state(&state);
    switch ((XgRenderRuntimeVariantCutoverHandler)cutover.handler) {
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_BEGIN:
        (void)xg_render_local_producer_auth_begin(
            cpu, &cutover, state.scene_generation, state.render_mode);
        return true;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_WRITER:
        (void)xg_render_local_producer_auth_writer(
            cpu, &cutover, state.scene_generation, state.render_mode);
        return true;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_COMMIT:
        (void)xg_render_local_producer_auth_commit(
            cpu, &cutover, state.scene_generation, state.render_mode);
        return true;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_BEGIN:
        if (auth_scene.artifact_authorizes_pc != NULL &&
            auth_scene.artifact_authorizes_pc(pc)) {
            xg_render_local_producer_auth_clear_kind(
                XG_RENDER_LOCAL_PRODUCER_ZOOM);
            return false;
        }
        (void)xg_render_local_producer_auth_begin(
            cpu, &cutover, state.scene_generation, state.render_mode);
        return true;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_WRITER:
        if (!xg_render_local_producer_auth_pending()) return false;
        (void)xg_render_local_producer_auth_writer(
            cpu, &cutover, state.scene_generation, state.render_mode);
        return true;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_COMMIT:
        if (!xg_render_local_producer_auth_pending()) return false;
        (void)xg_render_local_producer_auth_commit(
            cpu, &cutover, state.scene_generation, state.render_mode);
        return true;
    default:
        return false;
    }
}

static bool authorize_dispatch_pc(uint32_t pc, uint32_t instruction_word) {
    const uint32_t physical = pc & UINT32_C(0x1fffffff);
    XgRenderMutationClassification mutation;
    const uint32_t resident_world_mask =
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_SKY) |
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON) |
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS);

    static const struct {
        uint32_t pc;
        uint32_t instruction_word;
    } resident_sites[] = {
        { UINT32_C(0x8007fbe0), UINT32_C(0x00a04021) },
        { UINT32_C(0x80073b64), UINT32_C(0x3c03800d) },
        { UINT32_C(0x8007412c), UINT32_C(0x266400b8) },
        { UINT32_C(0x8009932c), UINT32_C(0x27bdffc8) },
        { UINT32_C(0x80084cd0), UINT32_C(0x8fbf0038) },
        { UINT32_C(0x80085f38), UINT32_C(0x8fbf0020) },
    };
    bool resident_site = false;

    xg_render_mutation_classify(pc, 4u, NULL, &mutation);

    for (size_t index = 0u;
         index < sizeof(resident_sites) / sizeof(resident_sites[0]); ++index)
        if (physical_address_equals(pc, resident_sites[index].pc) &&
            instruction_word == resident_sites[index].instruction_word) {
            resident_site = true;
            break;
        }
    return physical < UINT32_C(0x0005a000) ||
        (mutation.code_write_mask & resident_world_mask) != 0u ||
        xg_render_world_execution_site_authorized(pc, instruction_word) ||
        resident_site ||
        local_generation_for_pc(pc) != 0u ||
        (auth_scene.artifact_authorizes_pc != NULL &&
         auth_scene.artifact_authorizes_pc(pc));
}

static bool world_authentication_generation(uint64_t *out_generation) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    if (out_generation == NULL || state.scene_generation == UINT64_MAX)
        return false;
    *out_generation = state.scene_generation + 1u;
    return *out_generation != 0u;
}

static uint32_t world_readiness_blocker(void) {
    XgRenderRuntimeAuthSceneState state;
    GuestRenderBridgeSnapshot bridge = {0};
    query_state(&state);
    if (state.render_mode != GUEST_RENDER_RENDER_NATIVE) return 2u;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK) return 3u;
    return bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE
        ? 0u : 4u;
}

static bool world_authorize_direct_dispatch(void) {
    XgRenderRuntimeAuthSceneState state;
    GuestRenderBridgeSnapshot bridge = {0};
    query_state(&state);
    return state.render_mode == GUEST_RENDER_RENDER_NATIVE &&
        guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK &&
        (!state.active || bridge.modes.effective_render_mode ==
            GUEST_RENDER_RENDER_NATIVE) &&
        (state.active || state.armed);
}

static const XgNativeView *current_native_view(void) { return &native_view; }

static bool finalize_temporal_with_model_anchors(void) {
    return xg_render_model_repository_record_active_producer_anchors(
               model_repository_services()) &&
        xg_render_submission_finalize_temporal();
}

static const XgRenderWorldCoordinatorPolicy *world_policy(void) {
    static const XgRenderWorldCoordinatorPolicy policy = {
        .readiness_blocker = world_readiness_blocker,
        .authorize_direct_dispatch = world_authorize_direct_dispatch,
        .authentication_generation = world_authentication_generation,
        .interpolation_scene_generation = interpolation_scene_generation,
        .screen_x_cull_margin = screen_x_cull_margin,
        .native_view = current_native_view,
        .authorize_guest_range = guest_data_range_is_valid,
        .stack_address_is_valid = xg_render_runtime_stack_address_is_valid,
        .authorize_guest_word = xg_render_runtime_word_address_is_valid,
        .begin_submission = xg_render_submission_standalone_begin,
        .stage_native =
            xg_render_submission_stage_standalone_primitive_identified,
        .stage_temporal =
            xg_render_submission_stage_temporal_primitive_identified,
        .abort_submission = xg_render_submission_standalone_abort,
        .finalize_submission = xg_render_submission_standalone_finalize,
        .finalize_temporal = finalize_temporal_with_model_anchors,
    };
    return &policy;
}

static bool compass_publish(
        const XgRenderModelFt4SourceRecord *record,
        const XgRenderModelSourcePublication *publication) {
    return xg_render_model_repository_store_ft4_source(
        record, publication, model_repository_services());
}

static const XgFieldCompassPipelineServices *compass_services(void) {
    static const XgFieldCompassPipelineServices services = {
        .pre_scene_available = xg_field_compass_pre_scene_available,
        .begin_lifecycle = lifecycle_begin,
        .stage = xg_render_submission_pre_scene_stage,
        .publish = compass_publish,
    };
    return &services;
}

static bool compass_cutover(CPUState *cpu, uint32_t pc,
                            bool screen_aligned) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    if (state.render_mode != GUEST_RENDER_RENDER_NATIVE) return false;
    if (!xg_render_runtime_variant_no_gates_enabled() &&
        (auth_scene.artifact_is_authorized == NULL ||
         !auth_scene.artifact_is_authorized()))
        return false;
    if (auth_scene.artifact_authorizes_pc == NULL ||
        !auth_scene.artifact_authorizes_pc(pc)) {
        xg_field_compass_fail(8u);
        return false;
    }
    return xg_field_compass_cutover(
        cpu, pc, screen_aligned, compass_services());
}

static const XgFieldParticlePipelineServices *particle_services(void) {
    static const XgFieldParticlePipelineServices services = {
        .stage_active = xg_render_submission_stage_active_primitive,
        .stage_temporal =
            xg_render_submission_stage_temporal_primitive_identified,
        .reject = reject_family,
    };
    return &services;
}

static bool particle_cutover(CPUState *cpu, uint32_t pc) {
    XgRenderRuntimeAuthSceneState state;
    GuestRenderBridgeSnapshot bridge = {0};
    query_state(&state);
    if (state.render_mode != GUEST_RENDER_RENDER_NATIVE) return false;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK) goto fail;
    if (bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (auth_scene.artifact_authorizes_pc == NULL ||
        !auth_scene.artifact_authorizes_pc(pc))
        goto fail;
    return xg_field_particles_cutover(cpu, pc, particle_services());
fail:
    reject_family(41u);
    return false;
}

static bool projected_capture_template(
        const XgRenderFieldSpriteTemplateInput *input) {
    return xg_render_field_sprite_capture_template(
        input, field_sprite_services());
}

static bool proof_active(void) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return state.active && !state.completed;
}

static void projected_reject(uint32_t blocker) {
    if (proof_active()) reject_family(blocker);
}

static const XgFieldProjectedPipelineServices *projected_services(void) {
    static const XgFieldProjectedPipelineServices services = {
        .proof_active = proof_active,
        .stage_active = xg_render_submission_stage_active_primitive,
        .stage_standalone =
            xg_render_submission_stage_standalone_primitive_identified,
        .pre_scene_available = xg_render_submission_pre_scene_available,
        .stage_pre_scene = xg_render_submission_pre_scene_stage,
        .stage_temporal =
            xg_render_submission_stage_temporal_primitive_identified,
        .begin_lifecycle = lifecycle_begin,
        .has_template = xg_render_field_sprite_has_template,
        .available_template_capacity =
            xg_render_field_sprite_available_template_capacity,
        .capture_template = projected_capture_template,
        .native_view = current_native_view,
        .reject_policy = projected_reject,
    };
    return &services;
}

static bool projected_cutover(CPUState *cpu, uint32_t pc) {
    XgRenderRuntimeAuthSceneState state;
    GuestRenderBridgeSnapshot bridge = {0};
    query_state(&state);
    xg_field_projected_note_cutover_attempt();
    if (state.render_mode != GUEST_RENDER_RENDER_NATIVE) return false;
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK)
        return xg_field_projected_reject(68u, projected_services());
    if (bridge.modes.effective_render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    return xg_field_projected_cutover(cpu, pc, projected_services());
}

static bool zoom_authorize_replay(
        const XgRenderZoomSource *source,
        const GuestRenderNativeStreamMissContext *context) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return source != NULL && context != NULL &&
        (((auth_scene.artifact_is_authorized != NULL &&
           auth_scene.artifact_is_authorized()) &&
          source->artifact_generation == state.artifact_generation) ||
         local_authority_matches(
             XG_RENDER_ZOOM_TEMPLATE_STORE_PC, source->artifact_generation)) &&
        replay_container_matches_command(context) &&
        context->visual_id.scene_epoch == state.scene_generation + 1u &&
        (context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK ||
         context->source_kind ==
             GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST ||
         context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BURST);
}

static const XgFieldZoomPipelineServices *zoom_services(void) {
    static const XgFieldZoomPipelineServices services = {
        .proof_active = proof_active,
        .pre_scene_available = xg_render_submission_pre_scene_available,
        .stage_active = xg_render_submission_stage_active_primitive,
        .stage_pre_scene = xg_render_submission_pre_scene_stage,
        .authorize_replay = zoom_authorize_replay,
        .interpolation_scene = interpolation_scene_generation,
        .reject_policy = reject_family,
    };
    return &services;
}

static bool zoom_cutover(CPUState *cpu, uint32_t continuation) {
    XgRenderRuntimeAuthSceneState state;
    GuestRenderBridgeSnapshot bridge = {0};
    query_state(&state);
    if (state.render_mode != GUEST_RENDER_RENDER_NATIVE) return false;
    xg_field_zoom_note_cutover_attempt();
    if (guest_render_bridge_snapshot(&bridge) != GUEST_RENDER_OK)
        return xg_field_zoom_reject(57u, zoom_services());
    if ((state.active && bridge.modes.effective_render_mode !=
             GUEST_RENDER_RENDER_NATIVE) ||
        (!state.active && !state.armed)) {
        xg_field_zoom_reset_source_and_invocation();
        return false;
    }
    return xg_field_zoom_cutover(cpu, continuation, zoom_services());
}

static bool model_ft3_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    XgRenderRuntimeAuthSceneState state;
    XgRenderModelReplayResult result;
    query_state(&state);
    result = xg_render_model_repository_resolve_ft3(
        context, state.render_mode, out_semantic, model_repository_services());
    xg_render_model_sprite_pipeline_record_ft3_replay(result, context);
    return result == XG_RENDER_MODEL_REPLAY_RESOLVED;
}

static bool model_ft4_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    XgRenderRuntimeAuthSceneState state;
    const bool sprite_opcode = context != NULL && (context->opcode & 1u) == 0u;
    XgRenderModelReplayResult result;
    query_state(&state);
    result = xg_render_model_repository_resolve_ft4(
        context, state.render_mode, out_semantic, model_repository_services());
    xg_render_model_sprite_pipeline_record_ft4_replay(result, sprite_opcode);
    return result == XG_RENDER_MODEL_REPLAY_RESOLVED;
}

static bool zoom_resolve(const GuestRenderNativeStreamMissContext *context,
                         GpuRenderSemantic *out_semantic) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return xg_field_zoom_resolve(
        context, out_semantic, state.render_mode, zoom_services());
}

static bool field_sprite_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return xg_render_field_sprite_resolve(
        context, out_semantic, state.render_mode, field_sprite_services());
}

static bool polyline_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return xg_render_field_polyline_resolve(
        context, out_semantic, state.render_mode, polyline_services());
}

static bool residual_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return xg_render_residual_resolve(
        context, out_semantic, state.render_mode, lifecycle_services());
}

static bool f4_resolve(const GuestRenderNativeStreamMissContext *context,
                       GpuRenderSemantic *out_semantic) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return xg_render_f4_sources_resolve(
        context, out_semantic, state.render_mode, f4_services());
}

static uint64_t resolver_scene_generation(void) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return state.scene_generation;
}

static GuestRenderRenderMode resolver_render_mode(void) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return state.render_mode;
}

static uint64_t resolver_resource_generation(void) {
    return next_resource_generation;
}

static const XgRenderSharedPacketResolverServices *shared_packet_services(void) {
    static XgRenderSharedPacketResolverServices services;

    services = (XgRenderSharedPacketResolverServices){
        .guest.read_word = auth_scene.read_guest_word,
        .environment = xg_render_shared_packet_gpu_environment_services(),
        .mode = {
            .render_mode = resolver_render_mode,
            .packet_bindings_enabled =
                xg_render_runtime_variant_no_gates_enabled,
        },
        .identity = {
            .visual_scene_generation = resolver_scene_generation,
            .interpolation_scene_generation = interpolation_scene_generation,
        },
    };
    return &services;
}

static const XgRenderResolverRegistryServices *resolver_services(void) {
    static XgRenderResolverRegistryServices services = {
        .model_ft4 = model_ft4_resolve,
        .model_ft3 = model_ft3_resolve,
        .zoom = zoom_resolve,
        .field_sprite = field_sprite_resolve,
        .field_polyline = polyline_resolve,
        .residual = residual_resolve,
        .f4 = f4_resolve,
        .scene_generation = resolver_scene_generation,
        .resource_generation = resolver_resource_generation,
        .f4_source_count = xg_render_f4_sources_count,
    };
    services.shared_packet = shared_packet_services();
    return &services;
}

static bool record_resolved_semantic_anchors(
        uint64_t command_id, const GpuRenderSemantic *semantic) {
    return xg_render_submission_record_interpolation_anchors(semantic) &&
        xg_render_model_repository_record_resolved_producer_anchors(
            command_id, semantic, model_repository_services());
}

static XgRenderInvalidationServices invalidation_services(void) {
    return (XgRenderInvalidationServices){
        .field_sprite = field_sprite_services(),
        .model_repository = model_repository_services(),
        .model_sprite_pipeline = model_sprite_services(),
    };
}

static void configure_native_stream(
        const XgRenderRuntimeAuthSceneState *state) {
    guest_render_native_stream_set_miss_resolver(
        state->render_mode == GUEST_RENDER_RENDER_NATIVE
            ? xg_render_resolver_registry_resolve : NULL);
    guest_render_native_stream_set_resolved_semantic_observer(
        state->render_mode == GUEST_RENDER_RENDER_NATIVE
            ? record_resolved_semantic_anchors : NULL);
    guest_render_native_stream_set_shared_packet_bindings(
        xg_render_runtime_variant_no_gates_enabled());
}

bool xg_render_runtime_composition_configure(
        const XgRenderRuntimeAuthSceneServices *services) {
    XgRenderRuntimeAuthSceneState state;
    const XgRenderResolverRegistryServices *registry = NULL;
    uint32_t successful_registrations = 0u;
    bool registry_registered = false;
    bool invalidation_touched = false;

    if (services == NULL || services->query_state == NULL ||
        services->auth_snapshot == NULL ||
        services->auth_ir_item_get == NULL ||
        services->append_authenticated_ir == NULL ||
        services->artifact_authorizes_pc == NULL ||
        services->artifact_is_authorized == NULL ||
        services->completed_proof_matches_tier == NULL ||
        services->reject_auth == NULL || services->presentation_gate == NULL ||
        services->frame_count == NULL || services->read_guest_word == NULL)
        return false;
    if (configured) {
        const bool services_match =
            auth_scene.query_state == services->query_state &&
            auth_scene.auth_snapshot == services->auth_snapshot &&
            auth_scene.auth_ir_item_get == services->auth_ir_item_get &&
            auth_scene.append_authenticated_ir ==
                services->append_authenticated_ir &&
            auth_scene.artifact_authorizes_pc ==
                services->artifact_authorizes_pc &&
            auth_scene.artifact_is_authorized ==
                services->artifact_is_authorized &&
            auth_scene.completed_proof_matches_tier ==
                services->completed_proof_matches_tier &&
            auth_scene.reject_auth == services->reject_auth &&
            auth_scene.presentation_gate == services->presentation_gate &&
            auth_scene.frame_count == services->frame_count &&
            auth_scene.read_guest_word == services->read_guest_word;
        if (!services_match) return false;
        services->query_state(&state);
        configure_native_stream(&state);
        return true;
    }

    auth_scene = *services;
    if (!configure_local_auth(&successful_registrations)) goto rollback;
#ifdef XG_RENDER_RUNTIME_COMPOSITION_TESTING
    if (test_registration_failure_enabled &&
        successful_registrations == test_registration_failure_after)
        goto rollback;
#endif
    registry = resolver_services();
    if (!xg_render_resolver_registry_register(registry)) goto rollback;
    registry_registered = true;
    ++successful_registrations;
    invalidation_touched = true;
    if (!xg_render_invalidation_modules_configure()) goto rollback;

    services->query_state(&state);
    xg_render_submission_configure(submission_services());
    configure_native_stream(&state);
    configured = true;
    return true;

rollback:
    if (invalidation_touched) {
        xg_render_invalidation_clear_modules();
        xg_render_mutation_classifier_clear_sources();
    }
    if (registry_registered)
        (void)xg_render_resolver_registry_unregister(registry);
    unconfigure_local_auth();
    auth_scene = (XgRenderRuntimeAuthSceneServices){0};
    return false;
}

static XgRenderCutoverDispatchResult cutover_terminal(bool bypass) {
    return bypass ? XG_RENDER_CUTOVER_DISPATCH_BYPASS
                  : XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
}

static XgRenderCutoverDispatchResult observe_cutover_preamble(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route,
        const XgRenderRuntimeAuthSceneState *state) {
    XgRenderFieldPolylineObservation polyline;

    xg_render_world_execution_observe(
        state->render_mode != GUEST_RENDER_RENDER_ORIGINAL,
        route->pc, route->instruction_word);
    if (observe_local_producer(cpu, route->pc, route->instruction_word))
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    polyline = xg_render_field_polyline_observe(
        cpu, route->pc, route->instruction_word, state->render_mode,
        polyline_services());
    if (polyline != XG_RENDER_FIELD_POLYLINE_OBSERVATION_NONE) {
        if (polyline == XG_RENDER_FIELD_POLYLINE_OBSERVATION_FINISH)
            (void)xg_render_f4_sources_capture_field_f4(
                cpu, state->render_mode, f4_services());
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    }
    return authorize_dispatch_pc(route->pc, route->instruction_word)
        ? XG_RENDER_CUTOVER_DISPATCH_CONTINUE
        : XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
}

static XgRenderCutoverDispatchResult observe_f4_route(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route,
        GuestRenderRenderMode render_mode) {
    switch ((XgRenderCutoverAction)route->action) {
    case XG_CUTOVER_F4_FIXED_2A:
        (void)xg_render_f4_sources_capture_fixed_2a(
            cpu, render_mode, f4_services());
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_CUTOVER_F4_BATTLE_FADER:
        (void)xg_render_f4_sources_capture_battle_fader(
            cpu, render_mode, f4_services());
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_CUTOVER_F4_PROJECTED_2A:
        (void)xg_render_f4_sources_capture_projected_2a(
            cpu, render_mode, f4_services());
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_CUTOVER_F4_OBSERVE_2A_OT:
        xg_render_f4_sources_observe_2a_ot(
            cpu, render_mode, f4_services());
        return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    default:
        return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    }
}

static XgRenderCutoverDispatchResult observe_residual_route(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route,
        GuestRenderRenderMode render_mode) {
    XgRenderResidualCaptureRequest request = { .cpu = cpu };

    switch ((XgRenderCutoverAction)route->action) {
    case XG_CUTOVER_RESIDUAL_CLEAR_TILE:
        request.kind = XG_RENDER_RESIDUAL_CAPTURE_CLEAR_TILE;
        break;
    case XG_CUTOVER_RESIDUAL_FULLSCREEN_TILE:
        request.kind = XG_RENDER_RESIDUAL_CAPTURE_FULLSCREEN_TILE;
        break;
    case XG_CUTOVER_RESIDUAL_FADE_TILES:
        request.kind = XG_RENDER_RESIDUAL_CAPTURE_FADE_TILES;
        break;
    default:
        return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    }
    xg_render_residual_capture(&request, render_mode, lifecycle_services());
    return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
}

static XgRenderCutoverDispatchResult observe_overlay_route(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route,
        GuestRenderRenderMode render_mode) {
    const XgRenderOverlayFt4Observation observation =
        xg_render_overlay_ft4_observe(
            cpu, route->pc, route->instruction_word,
            render_mode, overlay_services());

    if (observation == XG_RENDER_OVERLAY_FT4_OBSERVATION_NONE)
        return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    if (observation ==
            XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_STATIC_GOURAUD) {
        const XgRenderResidualCaptureRequest request = {
            .kind = XG_RENDER_RESIDUAL_CAPTURE_STATIC_GOURAUD,
            .cpu = cpu,
        };
        xg_render_residual_capture(
            &request, render_mode, lifecycle_services());
        return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    }
    if (observation ==
            XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_PROJECTED_GOURAUD ||
        observation ==
            XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_PROJECTED_GOURAUD_2E) {
        const XgRenderResidualCaptureRequest request = {
            .kind = XG_RENDER_RESIDUAL_CAPTURE_PROJECTED_GOURAUD,
            .cpu = cpu,
        };
        xg_render_residual_capture(
            &request, render_mode, lifecycle_services());
        xg_render_overlay_ft4_finish_observation(
            observation, cpu, render_mode, overlay_services());
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    }
    return observation == XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED
        ? XG_RENDER_CUTOVER_DISPATCH_OBSERVED
        : XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
}

static XgRenderCutoverDispatchResult observe_model_pre_field_route(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route,
        GuestRenderRenderMode render_mode) {
    switch ((XgRenderCutoverAction)route->action) {
    case XG_CUTOVER_MODEL_FT4_TEMPLATE:
        xg_render_model_sprite_pipeline_observe_ft4_template(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_FT3_TEMPLATE:
        xg_render_model_sprite_pipeline_observe_ft3_template(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_FT3_LINK_CAPTURE:
        xg_render_model_sprite_pipeline_capture_ft3_link(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_FT3_LINK_FINISH:
        xg_render_model_sprite_pipeline_finish_ft3_link(
            cpu, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_FT4_GUEST_AVERAGE:
        xg_render_model_sprite_pipeline_observe_ft4_guest_pass(cpu, true);
        break;
    case XG_CUTOVER_MODEL_FT3_GUEST:
        xg_render_model_sprite_pipeline_observe_ft3_guest_pass(cpu);
        break;
    case XG_CUTOVER_MODEL_FT4_GUEST_FARTHEST:
        xg_render_model_sprite_pipeline_observe_ft4_guest_pass(cpu, false);
        break;
    case XG_CUTOVER_SPRITE_WRAPPER_BEGIN:
        xg_render_model_sprite_pipeline_sprite_begin(cpu, true, render_mode);
        break;
    default:
        return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    }
    return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
}

static XgRenderCutoverDispatchResult observe_model_post_field_route(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route,
        GuestRenderRenderMode render_mode) {
    switch ((XgRenderCutoverAction)route->action) {
    case XG_CUTOVER_SPRITE_BEGIN:
        xg_render_model_sprite_pipeline_sprite_begin(cpu, false, render_mode);
        break;
    case XG_CUTOVER_SPRITE_GEOMETRY:
        xg_render_model_sprite_pipeline_sprite_geometry_seam(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_SPRITE_MATERIAL:
        xg_render_model_sprite_pipeline_sprite_material_seam(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_SPRITE_WRAPPER_END:
        xg_render_model_sprite_pipeline_sprite_end(
            false, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_SPRITE_END:
        xg_render_model_sprite_pipeline_sprite_end(
            true, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_BEGIN:
        xg_render_model_sprite_pipeline_model_begin(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_FT4_SEAM:
        xg_render_model_sprite_pipeline_model_ft4_seam(
            cpu, route->pc, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_FT3_SEAM:
        xg_render_model_sprite_pipeline_model_ft3_seam(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_FINISH:
        xg_render_model_sprite_pipeline_model_finish(
            cpu, render_mode, model_sprite_services());
        break;
    case XG_CUTOVER_MODEL_END:
        xg_render_model_sprite_pipeline_model_end();
        break;
    default:
        return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    }
    return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
}

static XgRenderCutoverDispatchResult observe_variant_route(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route,
        const XgRenderRuntimeAuthSceneState *state) {
    const uint64_t artifact_generation = artifact_generation_for_pc(route->pc);

    switch ((XgRenderRuntimeVariantCutoverHandler)route->action) {
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_BEGIN:
        xg_field_zoom_observe_initializer_begin(
            cpu, state->render_mode, artifact_generation);
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_COMMIT:
        xg_field_zoom_observe_initializer_commit(cpu, artifact_generation);
        if (xg_field_zoom_source_valid())
            xg_field_zoom_register_resource_watches(watch_resource);
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_RGB_BEGIN:
        xg_field_zoom_observe_rgb_begin(
            cpu, state->render_mode, artifact_generation);
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_RGB_COMMIT:
        xg_field_zoom_observe_rgb_commit(cpu, artifact_generation);
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_ENTRY:
        xg_field_zoom_observe_entry(
            cpu, state->render_mode, artifact_generation);
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_NATIVE:
        return cutover_terminal(zoom_cutover(cpu, route->continuation));
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_PARTICLE_INITIALIZER:
        (void)xg_field_particles_observe_initializer(
            cpu, state->render_mode,
            auth_scene.artifact_authorizes_pc(route->pc), route->pc);
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_PARTICLE_NATIVE:
        return cutover_terminal(particle_cutover(cpu, route->pc));
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_COMPASS_WORLD:
        return cutover_terminal(compass_cutover(cpu, route->pc, false));
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_COMPASS_SCREEN:
        return cutover_terminal(compass_cutover(cpu, route->pc, true));
    case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ACTOR:
        return cutover_terminal(
            xg_render_field_character_native_actor_cutover(
                cpu, route->continuation, state->pending_tier,
                field_character_services()));
    default:
        return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    }
}

static XgRenderCutoverDispatchResult observe_cutover_route(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route) {
    XgRenderRuntimeAuthSceneState state;

    if (route == NULL) return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    query_state(&state);
    switch (route->module) {
    case XG_RENDER_CUTOVER_MODULE_PREAMBLE:
        return observe_cutover_preamble(cpu, route, &state);
    case XG_RENDER_CUTOVER_MODULE_F4:
        return observe_f4_route(cpu, route, state.render_mode);
    case XG_RENDER_CUTOVER_MODULE_OVERLAY_ADD_PRIM:
        return xg_render_overlay_ft4_observe_add_prim(
                   cpu, route->pc, route->instruction_word,
                   state.render_mode, overlay_services())
            ? XG_RENDER_CUTOVER_DISPATCH_OBSERVED
            : XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    case XG_RENDER_CUTOVER_MODULE_RESIDENT_LINE:
        return xg_render_resident_line_f2_observe(
                   cpu, route->pc, route->instruction_word,
                   state.render_mode, resident_line_services()) !=
                    XG_RENDER_RESIDENT_LINE_F2_OBSERVATION_NONE
            ? XG_RENDER_CUTOVER_DISPATCH_OBSERVED
            : XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    case XG_RENDER_CUTOVER_MODULE_RESIDUAL:
        return observe_residual_route(cpu, route, state.render_mode);
    case XG_RENDER_CUTOVER_MODULE_OVERLAY:
        return observe_overlay_route(cpu, route, state.render_mode);
    case XG_RENDER_CUTOVER_MODULE_WORLD: {
        const XgRenderWorldCoordinatorResult result =
            xg_render_world_coordinator_observe_route(
                cpu, route->pc, route->instruction_word, state.render_mode,
                state.scene_generation, model_sprite_services(), world_policy());
        if (result == XG_RENDER_WORLD_COORDINATOR_BYPASS)
            return XG_RENDER_CUTOVER_DISPATCH_BYPASS;
        return result == XG_RENDER_WORLD_COORDINATOR_NONE
            ? XG_RENDER_CUTOVER_DISPATCH_CONTINUE
            : XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
    }
    case XG_RENDER_CUTOVER_MODULE_MODEL_PRE_FIELD:
        return observe_model_pre_field_route(cpu, route, state.render_mode);
    case XG_RENDER_CUTOVER_MODULE_FIELD_SPRITE:
        return xg_render_field_sprite_observe(
                   cpu, route->pc, route->instruction_word, state.render_mode,
                   state.active, field_sprite_services())
            ? XG_RENDER_CUTOVER_DISPATCH_OBSERVED
            : XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    case XG_RENDER_CUTOVER_MODULE_OVERLAY_FIELD_MATERIAL:
        return xg_render_overlay_ft4_observe_field_material(
                   cpu, route->pc, route->instruction_word,
                   state.render_mode, overlay_services())
            ? XG_RENDER_CUTOVER_DISPATCH_OBSERVED
            : XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    case XG_RENDER_CUTOVER_MODULE_MODEL_POST_FIELD:
        return observe_model_post_field_route(cpu, route, state.render_mode);
    case XG_RENDER_CUTOVER_MODULE_PROJECTED:
        if (route->action == XG_CUTOVER_PROJECTED_INITIALIZER_BEGIN) {
            xg_field_projected_observe_initializer_begin(cpu, state.render_mode);
            return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
        }
        if (route->action == XG_CUTOVER_PROJECTED_INITIALIZER_COMMIT) {
            xg_field_projected_observe_initializer_commit(cpu);
            xg_field_projected_note_initializer_result(cpu);
            return XG_RENDER_CUTOVER_DISPATCH_OBSERVED;
        }
        return route->action == XG_CUTOVER_PROJECTED_NATIVE
            ? cutover_terminal(projected_cutover(cpu, route->pc))
            : XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
    case XG_RENDER_CUTOVER_MODULE_VARIANT:
        return observe_variant_route(cpu, route, &state);
    case XG_RENDER_CUTOVER_MODULE_FIELD_CHARACTER:
        return cutover_terminal(xg_render_field_character_native_bypass(
            cpu, route->pc, route->instruction_word,
            field_character_services()));
    }
    return XG_RENDER_CUTOVER_DISPATCH_CONTINUE;
}

XgRenderRuntimeCompositionResult xg_render_runtime_composition_observe_dispatch(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word) {
    const XgRenderCutoverDispatchContext context = {
        .observe_route = observe_cutover_route,
    };
    return xg_render_cutover_dispatch(cpu, pc, instruction_word, &context) ==
            XG_RENDER_CUTOVER_DISPATCH_BYPASS
        ? XG_RENDER_RUNTIME_COMPOSITION_BYPASS
        : XG_RENDER_RUNTIME_COMPOSITION_OBSERVED;
}

bool xg_render_runtime_composition_cutover_pc_relevant(uint32_t pc) {
    return xg_render_cutover_dispatch_pc_relevant(pc);
}

bool xg_render_runtime_composition_cutover_post_pc_relevant(uint32_t pc) {
    return xg_render_cutover_dispatch_post_pc_relevant(pc);
}

bool xg_render_runtime_composition_overlay_relevant(
        uint32_t pc, uint32_t instruction_word) {
    return xg_render_overlay_cutover_relevant(pc, instruction_word);
}

static bool authenticated_ir_describe(
        XgRenderAuthenticatedIrDescription *out_description) {
    XgRenderAuthSnapshot snapshot = {0};

    if (out_description == NULL || auth_scene.auth_snapshot == NULL ||
        !auth_scene.auth_snapshot(&snapshot))
        return false;
    *out_description = (XgRenderAuthenticatedIrDescription){
        .render_mode = snapshot.effective_render_mode,
        .item_count = snapshot.native_item_count,
    };
    return true;
}

bool xg_render_runtime_composition_flush_authenticated_ir(void) {
    const XgRenderAuthenticatedIrAccess access = {
        .describe = authenticated_ir_describe,
        .item_get = auth_scene.auth_ir_item_get,
    };
    return xg_render_submission_validate_authenticated_ir(&access);
}

bool xg_render_runtime_composition_observe_auth_hook(
        CPUState *cpu, XgRenderAuthTier tier, uint32_t hook, uint32_t pc,
        uint32_t instruction_word, uint32_t auxiliary) {
    XgRenderHookRouteDescriptor descriptor;

    if (xg_render_cutover_dispatch_hook_route_lookup(
            hook, pc, instruction_word, &descriptor) &&
        descriptor.kind == XG_RENDER_HOOK_ROUTE_UI_DRAW_OT &&
        configured && auth_scene.frame_count != NULL) {
        GpuRenderTransactionId visual_id = {0};

        if (submission_auth_snapshot(&visual_id) &&
            visual_id.state_sequence != UINT64_MAX) {
            ++visual_id.state_sequence;
            xg_render_ui_ot_note_draw_observation(
                (uint32_t)auth_scene.frame_count(), visual_id);
        }
    }
    if (hook == PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE) {
        (void)xg_render_field_character_observe_source(
            cpu, tier, PSX_XG_RENDER_SOURCE_STAGE_PRE, pc,
            instruction_word, auxiliary, field_character_services());
        return true;
    }
    if (hook == PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT) {
        (void)xg_render_field_character_observe_source(
            cpu, tier, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, pc,
            instruction_word, auxiliary, field_character_services());
        return true;
    }
    return xg_render_field_character_inner_call_matches(
        tier, hook, pc, instruction_word, field_character_services());
}

XgRenderHookRouteKind xg_render_runtime_composition_hook_route_kind(
        uint32_t hook, uint32_t pc, uint32_t instruction_word) {
    XgRenderHookRouteDescriptor descriptor;

    return xg_render_cutover_dispatch_hook_route_lookup(
               hook, pc, instruction_word, &descriptor)
        ? descriptor.kind : XG_RENDER_HOOK_ROUTE_NONE;
}

bool xg_render_runtime_composition_source_site_lookup(
        uint32_t pc, uint32_t instruction_word,
        PsxXgRenderSourceSiteMetadata *out_metadata) {
    return xg_render_runtime_variant_source_site_lookup(
        pc, instruction_word, out_metadata);
}

bool xg_render_runtime_composition_source_pc_relevant(uint32_t pc) {
    return !xg_render_field_character_source_blocked() &&
        xg_render_runtime_variant_source_pc_relevant(pc);
}

bool xg_render_runtime_composition_source_observe(
        CPUState *cpu, XgRenderAuthTier tier, PsxXgRenderSourceStage stage,
        uint32_t pc, uint32_t instruction_word, uint32_t auxiliary) {
    return xg_render_field_character_observe_source(
        cpu, tier, stage, pc, instruction_word, auxiliary,
        field_character_services());
}

bool xg_render_runtime_composition_resident_ft4_observe(
        CPUState *cpu, uint32_t stage, uint32_t pc,
        uint32_t instruction_word) {
    return xg_render_field_character_resident_ft4_observe(
        cpu, stage, pc, instruction_word, field_character_services());
}

void xg_render_runtime_composition_capture_model_ft3_link(CPUState *cpu) {
    XgRenderRuntimeAuthSceneState state;

    query_state(&state);
    xg_render_model_sprite_pipeline_capture_ft3_link(
        cpu, state.render_mode, model_sprite_services());
}

void xg_render_runtime_composition_capture_clear_tile(CPUState *cpu) {
    const XgRenderResidualCaptureRequest request = {
        .kind = XG_RENDER_RESIDUAL_CAPTURE_CLEAR_TILE,
        .cpu = cpu,
    };
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    xg_render_residual_capture(&request, state.render_mode, lifecycle_services());
}

void xg_render_runtime_composition_capture_logo_sprite(
        uint32_t command_address, uint8_t color) {
    const XgRenderResidualCaptureRequest request = {
        .kind = XG_RENDER_RESIDUAL_CAPTURE_LOGO_SPRITE,
        .command_address = command_address,
        .color = color,
    };
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    xg_render_residual_capture(&request, state.render_mode, lifecycle_services());
}

void xg_render_runtime_composition_capture_tile_write(
        CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
        uint8_t color) {
    const XgRenderResidualCaptureRequest request = {
        .kind = XG_RENDER_RESIDUAL_CAPTURE_TILE_WRITE,
        .cpu = cpu,
        .command_address = command_address,
        .writer_pc = writer_pc,
        .color = color,
    };
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    xg_render_residual_capture(&request, state.render_mode, lifecycle_services());
}

bool xg_render_runtime_composition_authority_authorizes_pc(uint32_t pc) {
    return local_generation_for_pc(pc) != 0u;
}

bool xg_render_runtime_composition_pending_authorizes_pc(uint32_t pc) {
    return xg_render_local_producer_auth_pending_authorizes_pc(pc);
}

void xg_render_runtime_composition_classify_code_write(
        uint32_t address, uint32_t size,
        const XgRenderMutationContext *context,
        XgRenderMutationClassification *out_classification) {
    XgRenderMutationContext resolved_context =
        context != NULL ? *context : (XgRenderMutationContext){0};

    resolved_context.resource_mutation |=
        xg_render_resource_watch_needs_invalidation(address, size);
    xg_render_mutation_classify(
        address, size, &resolved_context, out_classification);
}

bool xg_render_runtime_composition_resource_write_may_overlap(
        uint32_t address, uint32_t size) {
    return xg_render_resource_watch_overlaps(address, size);
}

bool xg_render_runtime_composition_resource_write_needs_invalidation(
        uint32_t address, uint32_t size) {
    return xg_render_resource_watch_needs_invalidation(address, size);
}

void xg_render_runtime_composition_handle_invalidation(
        const XgRenderInvalidationEvent *event) {
    const XgRenderInvalidationServices services = invalidation_services();
    xg_render_invalidation_dispatch(event, &services);
}

void xg_render_runtime_composition_configure_invalidation(void) {
    (void)xg_render_invalidation_modules_configure();
}

void xg_render_runtime_composition_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    xg_render_resource_watch_register(set_range);
    xg_render_mutation_register_watches(set_range);
}

bool xg_render_runtime_composition_configure_native_view(
        bool enabled, uint16_t aspect_num, uint16_t aspect_den,
        uint16_t canonical_width, uint16_t canonical_height) {
    return xg_native_view_configure(
        &native_view, enabled, aspect_num, aspect_den,
        canonical_width, canonical_height);
}

void xg_render_runtime_composition_before_gpu_submission(void) {
    xg_render_world_coordinator_before_gpu_submission(world_policy());
}

void xg_render_runtime_composition_complete_gpu_source_frame(void) {
    xg_render_world_coordinator_complete_gpu_source_frame(world_policy());
}

void xg_render_runtime_composition_disable(void) {
    xg_render_world_coordinator_disable(world_policy());
}

void xg_render_runtime_composition_prepare_authenticated_scene(void) {
    xg_render_submission_prepare_authenticated_scene();
}

bool xg_render_runtime_composition_flush_pre_scene(void) {
    if (xg_render_submission_pre_scene_flush()) return true;
    xg_render_field_character_note_pre_scene_blocker(
        xg_render_submission_pre_scene_blocker());
    return false;
}

bool xg_render_runtime_composition_producer_family_enabled(void) {
    return xg_render_field_character_producer_family_enabled();
}

void xg_render_runtime_composition_enable_producer_family(bool enabled) {
    xg_render_field_character_producer_family_enable(enabled);
    xg_render_submission_source_reset();
    xg_field_zoom_counters_reset();
}

void xg_render_runtime_composition_scene_boundary(bool generation_advanced) {
    xg_render_world_coordinator_scene_boundary(generation_advanced);
}

void xg_render_runtime_composition_reset(void) {
    next_resource_generation = 1u;
    xg_render_resolver_registry_reset();
    xg_render_world_coordinator_reset();
    (void)xg_native_view_configure(&native_view, false, 0u, 0u, 0u, 0u);
}

void xg_render_runtime_composition_set_terrain_temporal_coverage(bool enabled) {
    xg_render_world_simple_set_terrain_temporal_coverage(enabled);
}

void xg_render_runtime_composition_set_exec_phase_exchange(
        int (*exchange)(int phase)) {
    xg_render_world_coordinator_set_exec_phase_exchange(exchange);
}

void xg_render_runtime_composition_note_gpu_semantic_current(
        const GpuRenderSemantic *semantic) {
    (void)xg_render_submission_cover_temporal_current(semantic);
}

bool xg_render_runtime_composition_prepare_ui_ot(uint32_t start_addr) {
    XgRenderRuntimeAuthSceneState state;
    query_state(&state);
    return xg_render_ui_ot_prepare(
        start_addr, state.render_mode, (uint32_t)auth_scene.frame_count(),
        auth_scene.read_guest_word);
}

void xg_render_runtime_composition_ui_ot_snapshot(
        PsxXgRenderUiOtSnapshot *out_snapshot) {
    xg_render_ui_ot_snapshot(out_snapshot);
}

void xg_render_runtime_composition_source_snapshot(
        PsxXgRenderSourceSnapshot *out_snapshot) {
    xg_render_field_character_source_snapshot(out_snapshot);
}

void xg_render_runtime_composition_source_collector_snapshot(
        FieldCharacterShadowSummary *out_summary) {
    xg_render_field_character_source_collector_snapshot(out_summary);
}

void xg_render_runtime_composition_source_reset(void) {
    xg_render_field_character_source_reset();
    xg_render_submission_source_reset();
    xg_field_zoom_counters_reset();
}

void xg_render_runtime_composition_ft4_geometry_enable(bool enabled) {
    xg_render_field_character_ft4_geometry_enable(enabled);
}

bool xg_render_runtime_composition_ft4_geometry_pop(
        PsxXgRenderFt4Geometry *out_geometry) {
    return xg_render_field_character_ft4_geometry_pop(out_geometry);
}

void xg_render_runtime_composition_ft4_geometry_snapshot(
        PsxXgRenderFt4GeometrySnapshot *out_snapshot) {
    xg_render_field_character_ft4_geometry_snapshot(out_snapshot);
}

void xg_render_runtime_composition_zoom_template_contract_snapshot(
        PsxXgRenderZoomTemplateContractSnapshot *out_snapshot) {
    xg_field_zoom_template_contract_snapshot(out_snapshot);
}

void xg_render_runtime_composition_overlay_ft4_snapshot(
        PsxXgRenderOverlayFt4Snapshot *out_snapshot) {
    xg_render_overlay_ft4_snapshot(out_snapshot);
}

void xg_render_runtime_composition_producer_family_snapshot(
        PsxXgRenderProducerFamilySnapshot *out_snapshot) {
    xg_render_field_character_producer_family_snapshot(out_snapshot);
}

void xg_render_runtime_composition_projected_lifecycle_snapshot(
        PsxXgRenderProjectedLifecycleSnapshot *out_snapshot) {
    xg_field_projected_lifecycle_snapshot(out_snapshot);
}

void xg_render_runtime_composition_model_ft4_shadow_snapshot(
        PsxXgRenderModelFt4ShadowSnapshot *out_snapshot) {
    xg_render_model_sprite_pipeline_ft4_snapshot(out_snapshot);
}

void xg_render_runtime_composition_model_ft3_shadow_snapshot(
        PsxXgRenderModelFt3ShadowSnapshot *out_snapshot) {
    xg_render_model_sprite_pipeline_ft3_snapshot(out_snapshot);
}

void xg_render_runtime_composition_sprite_ft4_shadow_snapshot(
        PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot) {
    xg_render_model_sprite_pipeline_sprite_snapshot(out_snapshot);
}

void xg_render_runtime_composition_field_polyline_snapshot(
        PsxXgRenderFieldPolylineSnapshot *out_snapshot) {
    xg_render_field_polyline_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_horizon_shadow_snapshot(
        PsxXgRenderWorldHorizonShadowSnapshot *out_snapshot) {
    xg_render_world_horizon_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_effects_shadow_snapshot(
        PsxXgRenderWorldEffectsShadowSnapshot *out_snapshot) {
    xg_render_world_effects_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_terrain_water_shadow_snapshot(
        PsxXgRenderWorldTerrainWaterShadowSnapshot *out_snapshot) {
    xg_render_world_terrain_water_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_entity_shadows_shadow_snapshot(
        PsxXgRenderWorldEntityShadowsShadowSnapshot *out_snapshot) {
    xg_render_world_entity_shadows_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_decorations_shadow_snapshot(
        PsxXgRenderWorldDecorationsShadowSnapshot *out_snapshot) {
    xg_render_world_decorations_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_clouds_shadow_snapshot(
        PsxXgRenderWorldCloudsShadowSnapshot *out_snapshot) {
    xg_world_clouds_shadow_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_minimap_shadow_snapshot(
        PsxXgRenderWorldMinimapShadowSnapshot *out_snapshot) {
    xg_render_world_minimap_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_models_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_world_models_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_actor_sprites_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_world_actor_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_sky_native_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    xg_render_world_sky_snapshot(out_snapshot);
}

void xg_render_runtime_composition_world_execution_snapshot(
        PsxXgRenderWorldExecutionSnapshot *out_snapshot) {
    xg_render_world_execution_snapshot(out_snapshot);
}
