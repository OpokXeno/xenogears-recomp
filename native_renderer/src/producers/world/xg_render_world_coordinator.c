#include "xg_render_world_coordinator.h"

#include "gpu.h"
#include "xg_render_model_sprite_pipeline.h"
#include "xg_render_world_model_repository.h"
#include "xg_render_world_models_pipeline.h"
#include "xg_render_world_pending_services.h"
#include "xg_render_world_simple_producers.h"
#include "xg_render_world_sky_producer.h"
#include "xg_world_actor_sprites_source_capture.h"
#include "xg_world_clouds_shadow.h"
#include "xg_world_decorations_shadow.h"
#include "xg_world_entity_shadows.h"
#include "xg_world_terrain_water_source_capture.h"

#include <stddef.h>

typedef bool (*XgRenderWorldNativeCutover)(CPUState *cpu);

static const XgRenderWorldCoordinatorPolicy *active_policy;
static bool world_native_cutover_in_progress;
static bool world_native_cutover_failed;
static int (*exec_phase_exchange)(int phase);

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static void activate_policy(const XgRenderWorldCoordinatorPolicy *policy) {
    active_policy = policy;
}

static void abort_submission(void) {
    if (active_policy != NULL && active_policy->abort_submission != NULL)
        active_policy->abort_submission();
}

static uint32_t coordinated_dispatch_blocker(void) {
    if (world_native_cutover_failed) return 1u;
    if (active_policy == NULL || active_policy->readiness_blocker == NULL)
        return 2u;
    return active_policy->readiness_blocker();
}

static bool cutover_ready(void) {
    return coordinated_dispatch_blocker() == 0u;
}

static bool authorize_direct_dispatch(void) {
    return active_policy != NULL &&
        active_policy->authorize_direct_dispatch != NULL &&
        active_policy->authorize_direct_dispatch();
}

static bool authentication_generation(uint64_t *out_generation) {
    return active_policy != NULL &&
        active_policy->authentication_generation != NULL &&
        active_policy->authentication_generation(out_generation);
}

static uint64_t interpolation_scene_generation(void) {
    return active_policy != NULL &&
        active_policy->interpolation_scene_generation != NULL
        ? active_policy->interpolation_scene_generation() : 0u;
}

static int32_t screen_x_cull_margin(void) {
    return active_policy != NULL && active_policy->screen_x_cull_margin != NULL
        ? active_policy->screen_x_cull_margin() : 0;
}

static const XgNativeView *native_view(void) {
    return active_policy != NULL && active_policy->native_view != NULL
        ? active_policy->native_view() : NULL;
}

static bool authorize_guest_range(uint32_t address, uint32_t size,
                                  uint32_t alignment, bool allow_scratchpad) {
    return active_policy != NULL &&
        active_policy->authorize_guest_range != NULL &&
        active_policy->authorize_guest_range(
            address, size, alignment, allow_scratchpad);
}

static bool stack_address_is_valid(uint32_t address) {
    return active_policy != NULL &&
        active_policy->stack_address_is_valid != NULL &&
        active_policy->stack_address_is_valid(address);
}

static bool authorize_guest_word(uint32_t address) {
    return active_policy != NULL &&
        active_policy->authorize_guest_word != NULL &&
        active_policy->authorize_guest_word(address);
}

static bool begin_submission(void) {
    return active_policy != NULL && active_policy->begin_submission != NULL &&
        active_policy->begin_submission();
}

static bool stage_native(
        const XgRenderIrNativePrimitive *primitive, uint32_t packet_address,
        uint32_t source_primitive_index, uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id) {
    return active_policy != NULL && active_policy->stage_native != NULL &&
        active_policy->stage_native(
            primitive, packet_address, source_primitive_index,
            interpolation_producer_id, interpolation_primitive_id);
}

static bool stage_temporal(
        const XgRenderIrNativePrimitive *primitive,
        uint32_t interpolation_producer_id,
        uint32_t interpolation_primitive_id,
        const GpuRenderTemporalCullPolicy *policy) {
    return active_policy != NULL && active_policy->stage_temporal != NULL &&
        active_policy->stage_temporal(
            primitive, interpolation_producer_id,
            interpolation_primitive_id, policy);
}

static bool coordinator_in_progress(void) {
    return world_native_cutover_in_progress;
}

static bool coordinator_begin(void) {
    if (world_native_cutover_in_progress) return false;
    world_native_cutover_in_progress = true;
    return true;
}

static void coordinator_end(void) {
    world_native_cutover_in_progress = false;
}

static void coordinator_fail(void) {
    world_native_cutover_failed = true;
}

static bool coordinator_failed(void) {
    return world_native_cutover_failed;
}

static void model_coordinator_fail(void) {
    coordinator_fail();
    abort_submission();
}

static const XgRenderWorldModelRepositoryServices repository_services = {
    .authentication_generation = authentication_generation,
    .authorize_guest_range = authorize_guest_range,
    .stack_address_is_valid = stack_address_is_valid,
    .coordinator_in_progress = coordinator_in_progress,
    .coordinator_fail = model_coordinator_fail,
};

static const XgRenderWorldSimpleServices simple_services = {
    .coordinated_dispatch_blocker = coordinated_dispatch_blocker,
    .authorize_direct_dispatch = authorize_direct_dispatch,
    .authentication_generation = authentication_generation,
    .interpolation_scene_generation = interpolation_scene_generation,
    .screen_x_cull_margin = screen_x_cull_margin,
    .native_view = native_view,
    .authorize_guest_range = authorize_guest_range,
    .stage_native = stage_native,
    .stage_temporal = stage_temporal,
    .abort_submission = abort_submission,
};

static const XgRenderWorldSkyServices sky_services = {
    .authorize_native_dispatch = authorize_direct_dispatch,
    .authorize_guest_word = authorize_guest_word,
    .stage_native = stage_native,
    .stage_temporal = stage_temporal,
    .abort_submission = abort_submission,
};

static const XgRenderWorldPendingServices pending_services = {
    .cutover_ready = cutover_ready,
    .authentication_generation = authentication_generation,
    .authorize_guest_range = authorize_guest_range,
    .screen_x_cull_margin = screen_x_cull_margin,
    .begin_submission = begin_submission,
    .stage_native = stage_native,
    .stage_temporal = stage_temporal,
    .abort_submission = abort_submission,
    .coordinator_in_progress = coordinator_in_progress,
    .coordinator_begin = coordinator_begin,
    .coordinator_end = coordinator_end,
    .coordinator_fail = coordinator_fail,
    .coordinator_failed = coordinator_failed,
};

static const XgRenderWorldModelsPipelineServices models_services = {
    .repository = &repository_services,
    .cutover_ready = cutover_ready,
    .authentication_generation = authentication_generation,
    .authorize_guest_range = authorize_guest_range,
    .stack_address_is_valid = stack_address_is_valid,
    .interpolation_scene_generation = interpolation_scene_generation,
    .screen_x_cull_margin = screen_x_cull_margin,
    .begin_submission = begin_submission,
    .stage_native = stage_native,
    .stage_temporal = stage_temporal,
    .abort_submission = abort_submission,
};

static bool actor_commit(CPUState *cpu) {
    return xg_render_world_actor_commit(cpu, &pending_services);
}

static bool models_commit(CPUState *cpu) {
    return xg_render_world_models_commit(cpu, &models_services);
}

static bool terrain_water_cutover(CPUState *cpu) {
    return xg_render_world_terrain_water_cutover(cpu, &simple_services);
}

static bool entity_shadows_cutover(CPUState *cpu) {
    return xg_render_world_entity_shadows_cutover(cpu, &simple_services);
}

static bool decorations_cutover(CPUState *cpu) {
    return xg_render_world_decorations_cutover(cpu, &simple_services);
}

static bool minimap_cutover(CPUState *cpu) {
    return xg_render_world_minimap_cutover(cpu, &simple_services);
}

static bool horizon_cutover(CPUState *cpu) {
    return xg_render_world_horizon_cutover(cpu, &simple_services);
}

static bool effects_cutover(CPUState *cpu) {
    return xg_render_world_effects_cutover(cpu, &simple_services);
}

static bool sky_cutover(CPUState *cpu) {
    return xg_render_world_sky_cutover(cpu, &sky_services);
}

static XgRenderWorldCoordinatorResult run_native_cutover(
        CPUState *cpu, XgRenderWorldNativeCutover cutover) {
    bool accepted;
    int previous_exec_phase = 0;

    if (cutover == NULL || world_native_cutover_in_progress) {
        coordinator_fail();
        abort_submission();
        return XG_RENDER_WORLD_COORDINATOR_FAILURE;
    }
    /* A failed sibling aborts its own submission without disabling unrelated
     * world families for the rest of the scene. */
    if (world_native_cutover_failed) world_native_cutover_failed = false;
    world_native_cutover_in_progress = true;
    if (exec_phase_exchange != NULL)
        previous_exec_phase = exec_phase_exchange(2);
    accepted = cutover(cpu);
    if (exec_phase_exchange != NULL)
        (void)exec_phase_exchange(previous_exec_phase);
    world_native_cutover_in_progress = false;
    return accepted ? XG_RENDER_WORLD_COORDINATOR_BYPASS
                    : XG_RENDER_WORLD_COORDINATOR_FAILURE;
}

static XgRenderWorldCoordinatorResult prepare_actor(CPUState *cpu) {
    bool prepared;

    if (coordinator_failed() || coordinator_in_progress() ||
        !coordinator_begin()) {
        coordinator_fail();
        abort_submission();
        xg_render_world_actor_clear_pending();
        return XG_RENDER_WORLD_COORDINATOR_FAILURE;
    }
    prepared = xg_render_world_actor_prepare(cpu, &pending_services);
    coordinator_end();
    if (!prepared) {
        coordinator_fail();
        xg_render_world_actor_clear_pending();
        return XG_RENDER_WORLD_COORDINATOR_FAILURE;
    }
    return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
}

static XgRenderWorldCoordinatorResult prepare_models(CPUState *cpu) {
    bool prepared;

    if (coordinator_failed() || coordinator_in_progress() ||
        !coordinator_begin()) {
        coordinator_fail();
        abort_submission();
        xg_render_world_models_clear_pending();
        return XG_RENDER_WORLD_COORDINATOR_FAILURE;
    }
    prepared = xg_render_world_models_prepare(cpu, &models_services);
    coordinator_end();
    if (!prepared) {
        coordinator_fail();
        xg_render_world_models_clear_pending();
        return XG_RENDER_WORLD_COORDINATOR_FAILURE;
    }
    return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
}

XgRenderWorldCoordinatorResult xg_render_world_coordinator_observe_route(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        GuestRenderRenderMode render_mode, uint64_t scene_generation,
        const XgRenderModelSpritePipelineServices *model_sprite,
        const XgRenderWorldCoordinatorPolicy *policy) {
    const bool native = render_mode == GUEST_RENDER_RENDER_NATIVE;
    const bool shadow = render_mode == GUEST_RENDER_RENDER_SHADOW;

    activate_policy(policy);
    if (physical_address_equals(pc, UINT32_C(0x8002c8cc)) &&
        instruction_word == UINT32_C(0x27bdffd0)) {
        xg_render_world_model_repository_initializer_begin(
            cpu, render_mode, &repository_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002cb4c)) &&
        instruction_word == UINT32_C(0x03e00008)) {
        xg_render_world_model_repository_initializer_finish(
            cpu, &repository_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x8003f968)) &&
        instruction_word == UINT32_C(0x1080000a)) {
        xg_render_world_model_repository_begin_packet_copy(
            cpu, render_mode, &repository_services);
        xg_render_model_sprite_pipeline_begin_packet_copy(cpu, render_mode);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x8003f994)) &&
        instruction_word == UINT32_C(0x03e00008)) {
        xg_render_world_model_repository_finish_packet_copy(
            cpu, render_mode, &repository_services);
        xg_render_model_sprite_pipeline_finish_packet_copy(
            cpu, render_mode, model_sprite);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x8002caa4)) &&
        instruction_word == UINT32_C(0x3c028006)) {
        xg_render_world_model_repository_observe_initializer_success(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if ((physical_address_equals(pc, UINT32_C(0x8004a1ac)) &&
         instruction_word == UINT32_C(0xe8b60000)) ||
        (physical_address_equals(pc, UINT32_C(0x8004a1e8)) &&
         instruction_word == UINT32_C(0xe9360000)) ||
        (physical_address_equals(pc, UINT32_C(0x8004a274)) &&
         instruction_word == UINT32_C(0xe8d60000)) ||
        (physical_address_equals(pc, UINT32_C(0x8004a2b8)) &&
         instruction_word == UINT32_C(0xe9560000))) {
        xg_render_world_model_repository_observe_color_writes(
            cpu, pc, &repository_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, XG_WORLD_ACTOR_SPRITES_WORLD_ENTRY) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        xg_render_world_actor_context_begin(cpu, &pending_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x80085f38)) &&
        instruction_word == UINT32_C(0x8fbf0020)) {
        xg_render_world_actor_context_finish(cpu, &pending_services);
        return coordinator_failed() ? XG_RENDER_WORLD_COORDINATOR_FAILURE
                                    : XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (native && xg_render_world_actor_context_active() &&
        physical_address_equals(pc, XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM) &&
        instruction_word == UINT32_C(0x02002021))
        return prepare_actor(cpu);
    if (native && xg_render_world_actor_context_active() &&
        xg_render_world_actor_pending_valid() &&
        physical_address_equals(pc, UINT32_C(0x8001e2e0)) &&
        instruction_word == UINT32_C(0x8fbf0018)) {
        const XgRenderWorldCoordinatorResult result =
            run_native_cutover(cpu, actor_commit);
        return result == XG_RENDER_WORLD_COORDINATOR_FAILURE
            ? result : XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }

    if (physical_address_equals(pc, XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        xg_render_world_terrain_water_note_entry(cpu, native);
        if (native) return run_native_cutover(cpu, terrain_water_cutover);
    }
    if (native &&
        physical_address_equals(pc, XG_WORLD_ENTITY_SHADOWS_ENTRY_PC) &&
        instruction_word == UINT32_C(0x3c02800a))
        return run_native_cutover(cpu, entity_shadows_cutover);
    if (native &&
        physical_address_equals(pc, XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC) &&
        instruction_word == UINT32_C(0x27bdffd8))
        return run_native_cutover(cpu, decorations_cutover);
    if (native &&
        physical_address_equals(pc, XG_WORLD_MODELS_PRODUCER_ENTRY) &&
        instruction_word == UINT32_C(0x24020800))
        return prepare_models(cpu);
    if (native && physical_address_equals(pc, UINT32_C(0x80084cd0)) &&
        instruction_word == UINT32_C(0x8fbf0038)) {
        const XgRenderWorldCoordinatorResult result =
            run_native_cutover(cpu, models_commit);
        return result == XG_RENDER_WORLD_COORDINATOR_FAILURE
            ? result : XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (native && physical_address_equals(pc, UINT32_C(0x80086798)) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        xg_render_world_clouds_prepare(cpu, &pending_services);
        return coordinator_failed() ? XG_RENDER_WORLD_COORDINATOR_FAILURE
                                    : XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (native && physical_address_equals(pc, UINT32_C(0x800876dc)) &&
        instruction_word == UINT32_C(0x8fbf004c)) {
        xg_render_world_clouds_commit(cpu, &pending_services);
        return coordinator_failed() ? XG_RENDER_WORLD_COORDINATOR_FAILURE
                                    : XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (native && physical_address_equals(pc, UINT32_C(0x8007412c)) &&
        instruction_word == UINT32_C(0x266400b8))
        return run_native_cutover(cpu, minimap_cutover);
    if (native && physical_address_equals(pc, UINT32_C(0x80073b04)) &&
        instruction_word == UINT32_C(0x27bdffc0))
        return run_native_cutover(cpu, horizon_cutover);
    if (native && physical_address_equals(pc, UINT32_C(0x80089c78)) &&
        instruction_word == UINT32_C(0x27bdffb0))
        return run_native_cutover(cpu, effects_cutover);
    if (native && physical_address_equals(pc, UINT32_C(0x800737ec)) &&
        instruction_word == UINT32_C(0x27bdffb8))
        return run_native_cutover(cpu, sky_cutover);
    if (shadow && physical_address_equals(pc, UINT32_C(0x8009932c)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        xg_render_world_terrain_water_shadow_begin(cpu, &simple_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x800996d4)) &&
        instruction_word == UINT32_C(0x8fbf0034)) {
        xg_render_world_terrain_water_shadow_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x800747dc)) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        xg_render_world_entity_shadows_shadow_begin(cpu, &simple_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x80074e24)) &&
        instruction_word == UINT32_C(0x8fbf0064)) {
        xg_render_world_entity_shadows_shadow_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x80074c84)) &&
        instruction_word == UINT32_C(0x8ecc0000)) {
        xg_render_world_entity_shadows_shadow_observe_transform(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x8008615c)) &&
        instruction_word == UINT32_C(0x27bdffd8)) {
        xg_render_world_decorations_shadow_outer_begin(cpu, &simple_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x80099bfc)) &&
        instruction_word == UINT32_C(0x27bdfff8)) {
        xg_render_world_decorations_shadow_helper_begin(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x80099e78)) &&
        instruction_word == UINT32_C(0x8fb10008)) {
        xg_render_world_decorations_shadow_helper_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x800863bc)) &&
        instruction_word == UINT32_C(0x8fbf0024)) {
        xg_render_world_decorations_shadow_outer_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x80086798)) &&
        instruction_word == UINT32_C(0x3c02800a)) {
        GpuDrawState draw = {0};
        gpu_get_draw_state(&draw);
        (void)xg_world_clouds_shadow_begin(
            cpu, scene_generation + 1u, &draw, screen_x_cull_margin());
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x80086e3c)) &&
        instruction_word == UINT32_C(0x8e2202d4)) {
        xg_world_clouds_shadow_observe_anchor(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x800876dc)) &&
        instruction_word == UINT32_C(0x8fbf004c)) {
        (void)xg_world_clouds_shadow_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x800740b8)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        xg_render_world_minimap_shadow_begin(cpu, &simple_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (shadow && physical_address_equals(pc, UINT32_C(0x80074564)) &&
        instruction_word == UINT32_C(0x8fbf0030)) {
        xg_render_world_minimap_shadow_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x80073b04)) &&
        instruction_word == UINT32_C(0x27bdffc0)) {
        xg_render_world_horizon_shadow_begin(cpu, shadow, &simple_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x80089c78)) &&
        instruction_word == UINT32_C(0x27bdffb0)) {
        xg_render_world_effects_shadow_begin(cpu, shadow, &simple_services);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x8008a294)) &&
        instruction_word == UINT32_C(0x8fbf004c)) {
        xg_render_world_effects_shadow_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    if (physical_address_equals(pc, UINT32_C(0x80073e0c)) &&
        instruction_word == UINT32_C(0x8fbf003c)) {
        xg_render_world_horizon_shadow_finish(cpu);
        return XG_RENDER_WORLD_COORDINATOR_OBSERVED;
    }
    return XG_RENDER_WORLD_COORDINATOR_NONE;
}

void xg_render_world_coordinator_set_exec_phase_exchange(
        int (*exchange)(int phase)) {
    exec_phase_exchange = exchange;
}

void xg_render_world_coordinator_before_gpu_submission(
        const XgRenderWorldCoordinatorPolicy *policy) {
    activate_policy(policy);
    if (world_native_cutover_failed) {
        abort_submission();
        return;
    }
    if (policy == NULL || policy->finalize_submission == NULL ||
        !policy->finalize_submission())
        world_native_cutover_failed = true;
}

void xg_render_world_coordinator_complete_gpu_source_frame(
        const XgRenderWorldCoordinatorPolicy *policy) {
    activate_policy(policy);
    if (world_native_cutover_failed) {
        abort_submission();
        return;
    }
    if (policy == NULL || policy->finalize_temporal == NULL ||
        !policy->finalize_temporal()) {
        world_native_cutover_failed = true;
        abort_submission();
    }
}

void xg_render_world_coordinator_disable(
        const XgRenderWorldCoordinatorPolicy *policy) {
    activate_policy(policy);
    abort_submission();
    world_native_cutover_in_progress = false;
    world_native_cutover_failed = false;
}

void xg_render_world_coordinator_scene_boundary(bool generation_advanced) {
    world_native_cutover_in_progress = false;
    world_native_cutover_failed = !generation_advanced;
}

void xg_render_world_coordinator_reset(void) {
    world_native_cutover_in_progress = false;
    world_native_cutover_failed = false;
}
