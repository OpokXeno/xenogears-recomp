#include "xg_render_fragment_runtime.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum XgRenderFragmentFrameLane {
    XG_RENDER_FRAGMENT_FRAME_LANE_NONE = 0,
    XG_RENDER_FRAGMENT_FRAME_LANE_DIRECT,
    XG_RENDER_FRAGMENT_FRAME_LANE_TYPED,
} XgRenderFragmentFrameLane;

static XgRenderFragmentRuntimeDiagnostics diagnostics;
static XgRenderFragmentFrameLane frame_lane;
static bool finalized;
static bool boundary_description_valid;
static bool boundary_finished;
static XgRenderSourceFrameDescription boundary_description;
static atomic_flag guard = ATOMIC_FLAG_INIT;
static uint64_t lifecycle_generation;

struct XgRenderFragmentRuntimeTransaction {
    XgRenderSemanticCompositorTransaction *compositor;
    uint64_t lifecycle_generation;
    XgRenderFragmentRuntimeDiagnostics diagnostics;
    XgRenderSourceFrameDescription boundary_description;
    XgRenderFragmentFrameLane frame_lane;
    bool finalized;
    bool boundary_description_valid;
    bool boundary_finished;
};

static void lock_runtime(void) {
    while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) {}
}

static void unlock_runtime(void) {
    atomic_flag_clear_explicit(&guard, memory_order_release);
}

static void clear_frame_state(void) {
    ++lifecycle_generation;
    diagnostics.primary_owner = XG_SEMANTIC_MODULE_RESIDENT;
    diagnostics.scene_generation = 0u;
    diagnostics.frame_owned = false;
    diagnostics.field_movie_companion = false;
    diagnostics.battle_fx_companion = false;
    diagnostics.terminal_failure = false;
    frame_lane = XG_RENDER_FRAGMENT_FRAME_LANE_NONE;
    finalized = false;
    boundary_description_valid = false;
    boundary_finished = false;
    memset(&boundary_description, 0, sizeof(boundary_description));
}

XgRenderFragmentRuntimeResult xg_render_fragment_runtime_transaction_begin(
        XgRenderFragmentRuntimeTransaction **out_transaction) {
    XgRenderFragmentRuntimeTransaction *transaction;

    if (out_transaction == NULL) return XG_RENDER_FRAGMENT_RUNTIME_INVALID_ARGUMENT;
    *out_transaction = NULL;
    transaction = (XgRenderFragmentRuntimeTransaction *)calloc(
        1u, sizeof(*transaction));
    if (transaction == NULL) return XG_RENDER_FRAGMENT_RUNTIME_PRODUCER_REJECTED;
    lock_runtime();
    transaction->lifecycle_generation = lifecycle_generation;
    transaction->diagnostics = diagnostics;
    transaction->boundary_description = boundary_description;
    transaction->frame_lane = frame_lane;
    transaction->finalized = finalized;
    transaction->boundary_description_valid = boundary_description_valid;
    transaction->boundary_finished = boundary_finished;
    if (xg_render_semantic_compositor_transaction_begin(
            &transaction->compositor) != XG_RENDER_SEMANTIC_COMPOSITOR_OK) {
        unlock_runtime();
        free(transaction);
        return XG_RENDER_FRAGMENT_RUNTIME_PRODUCER_REJECTED;
    }
    unlock_runtime();
    *out_transaction = transaction;
    return XG_RENDER_FRAGMENT_RUNTIME_OK;
}

void xg_render_fragment_runtime_transaction_commit(
        XgRenderFragmentRuntimeTransaction *transaction) {
    if (transaction == NULL) return;
    xg_render_semantic_compositor_transaction_commit(transaction->compositor);
    free(transaction);
}

void xg_render_fragment_runtime_transaction_rollback(
        XgRenderFragmentRuntimeTransaction *transaction) {
    if (transaction == NULL) return;
    lock_runtime();
    /* Match begin/ingress lock order; never restore only half of the pair. */
    if (transaction->lifecycle_generation != lifecycle_generation) {
        xg_render_semantic_compositor_transaction_commit(transaction->compositor);
    } else if (xg_render_semantic_compositor_transaction_rollback(
                   transaction->compositor)) {
        diagnostics = transaction->diagnostics;
        boundary_description = transaction->boundary_description;
        frame_lane = transaction->frame_lane;
        finalized = transaction->finalized;
        boundary_description_valid = transaction->boundary_description_valid;
        boundary_finished = transaction->boundary_finished;
    }
    unlock_runtime();
    free(transaction);
}

static XgRenderFragmentRuntimeResult reject_preflight(
        XgRenderFragmentRuntimeResult result) {
    diagnostics.terminal_failure = true;
    diagnostics.terminal_failures++;
    return result;
}

static XgRenderFragmentRuntimeResult preflight_production_adapter(
        const XgRenderSourceFrameDescription *frame, bool *out_claimed) {
    XgRenderFragmentRuntimeResult result = XG_RENDER_FRAGMENT_RUNTIME_OK;

    if (out_claimed != NULL) *out_claimed = false;
    diagnostics.ingress_attempts++;
    if (frame == NULL || frame->scene.module > XG_SEMANTIC_MODULE_MOVIE ||
        frame->scene_generation == 0u) {
        result = reject_preflight(XG_RENDER_FRAGMENT_RUNTIME_INVALID_ARGUMENT);
    } else if (diagnostics.terminal_failure) {
        result = XG_RENDER_FRAGMENT_RUNTIME_TERMINAL_FAILURE;
    } else if (frame_lane == XG_RENDER_FRAGMENT_FRAME_LANE_TYPED) {
        diagnostics.mixed_primary_owner_rejections++;
        result = reject_preflight(
            XG_RENDER_FRAGMENT_RUNTIME_MIXED_PRIMARY_OWNER);
    } else if (!diagnostics.frame_owned) {
        frame_lane = XG_RENDER_FRAGMENT_FRAME_LANE_DIRECT;
        diagnostics.frame_owned = true;
        diagnostics.primary_owner = frame->scene.module;
        diagnostics.scene_generation = frame->scene_generation;
        boundary_description = *frame;
        boundary_description_valid = true;
        if (out_claimed != NULL) *out_claimed = true;
    } else if (diagnostics.primary_owner != frame->scene.module) {
        diagnostics.module_mismatches++;
        result = reject_preflight(XG_RENDER_FRAGMENT_RUNTIME_MODULE_MISMATCH);
    } else if (diagnostics.scene_generation != frame->scene_generation) {
        diagnostics.scene_generation_mismatches++;
        result = reject_preflight(
            XG_RENDER_FRAGMENT_RUNTIME_SCENE_GENERATION_MISMATCH);
    }
    return result;
}

XgRenderFragmentRuntimeResult xg_render_fragment_runtime_ingress_production_adapter(
        const XgRenderSourceFrameDescription *frame) {
    XgRenderFragmentRuntimeResult result;
    bool claimed = false;

    lock_runtime();
    result = preflight_production_adapter(frame, &claimed);
    if (result == XG_RENDER_FRAGMENT_RUNTIME_OK && claimed)
        diagnostics.accepted_fragments++;
    unlock_runtime();
    return result;
}

static XgRenderFragmentRuntimeResult producer_result(int result, int success) {
    diagnostics.last_producer_result = result;
    if (result == success) {
        diagnostics.accepted_fragments++;
        return XG_RENDER_FRAGMENT_RUNTIME_OK;
    }
    diagnostics.producer_rejections++;
    diagnostics.terminal_failures++;
    diagnostics.terminal_failure = true;
    return XG_RENDER_FRAGMENT_RUNTIME_PRODUCER_REJECTED;
}

static XgRenderSemanticOwnerDomain production_owner_domain(
        XgSemanticModuleKind module) {
    switch (module) {
    case XG_SEMANTIC_MODULE_FIELD:
    case XG_SEMANTIC_MODULE_WORLD:
        return XG_RENDER_SEMANTIC_OWNER_FIELD_WORLD;
    case XG_SEMANTIC_MODULE_RESIDENT:
    case XG_SEMANTIC_MODULE_MENU:
        return XG_RENDER_SEMANTIC_OWNER_RESIDENT_MENU;
    case XG_SEMANTIC_MODULE_BATTLE:
    case XG_SEMANTIC_MODULE_BATTLING:
        return XG_RENDER_SEMANTIC_OWNER_BATTLE_PRIMARY;
    case XG_SEMANTIC_MODULE_MOVIE:
        return XG_RENDER_SEMANTIC_OWNER_MOVIE;
    }
    return XG_RENDER_SEMANTIC_OWNER_DISPLAY;
}

static void count_production_update(XgSemanticModuleKind module) {
    switch (module) {
    case XG_SEMANTIC_MODULE_FIELD:
    case XG_SEMANTIC_MODULE_WORLD:
        diagnostics.production_field_world_updates++;
        break;
    case XG_SEMANTIC_MODULE_RESIDENT:
    case XG_SEMANTIC_MODULE_MENU:
        diagnostics.production_resident_menu_updates++;
        break;
    case XG_SEMANTIC_MODULE_BATTLE:
    case XG_SEMANTIC_MODULE_BATTLING:
        diagnostics.production_battle_updates++;
        break;
    case XG_SEMANTIC_MODULE_MOVIE:
        diagnostics.production_movie_updates++;
        break;
    }
}

static XgRenderFragmentRuntimeResult ingress_production_owner_fragment(
        const XgRenderSourceFrameDescription *frame,
        const XgRenderSourceFrameFragment *fragment) {
    const XgRenderSemanticOwnerKey owner = {
        .module = frame->scene.module,
        .domain = production_owner_domain(frame->scene.module),
        .primary_root = frame->scene.primary_overlay_identity,
        .secondary_root = frame->scene.authored_submode,
    };
    const XgRenderSemanticOwnerAuthority authority = {
        .scene_generation = frame->scene_generation,
        .owner_generation = frame->scene_generation,
        .receipt = frame->scene.executable_identity,
        .proof = frame->scene.primary_overlay_identity,
    };
    const XgRenderSemanticCompositorResult compositor_result =
        xg_render_semantic_compositor_upsert(
            &owner, &authority, frame, fragment);

    count_production_update(frame->scene.module);
    return producer_result((int)compositor_result,
                           (int)XG_RENDER_SEMANTIC_COMPOSITOR_OK);
}

XgRenderFragmentRuntimeResult xg_render_fragment_runtime_ingress_production_fragment(
        const XgRenderSourceFrameDescription *frame,
        const XgRenderSourceFrameFragment *fragment) {
    XgRenderFragmentRuntimeResult result;

    lock_runtime();
    result = preflight_production_adapter(frame, NULL);
    if (result == XG_RENDER_FRAGMENT_RUNTIME_OK)
        result = fragment != NULL
            ? ingress_production_owner_fragment(frame, fragment)
            : reject_preflight(XG_RENDER_FRAGMENT_RUNTIME_INVALID_ARGUMENT);
    unlock_runtime();
    return result;
}

XgRenderFragmentRuntimeResult
xg_render_fragment_runtime_ingress_authenticated_fragment(
        const XgRenderSourceFrameDescription *frame,
        const XgRenderSemanticOwnerKey *owner,
        const XgRenderSemanticOwnerAuthority *authority,
        const XgRenderSourceFrameFragment *fragment) {
    XgRenderFragmentRuntimeResult result = XG_RENDER_FRAGMENT_RUNTIME_OK;
    XgRenderSemanticCompositorResult compositor_result;

    lock_runtime();
    diagnostics.ingress_attempts++;
    if (frame == NULL || owner == NULL || authority == NULL || fragment == NULL ||
        frame->scene_generation == 0u ||
        frame->scene.module != owner->module ||
        frame->scene_generation != authority->scene_generation) {
        result = reject_preflight(XG_RENDER_FRAGMENT_RUNTIME_INVALID_ARGUMENT);
    } else if (diagnostics.terminal_failure) {
        result = XG_RENDER_FRAGMENT_RUNTIME_TERMINAL_FAILURE;
    } else if (diagnostics.frame_owned &&
               (diagnostics.primary_owner != frame->scene.module ||
                diagnostics.scene_generation != frame->scene_generation)) {
        diagnostics.mixed_primary_owner_rejections++;
        result = reject_preflight(
            XG_RENDER_FRAGMENT_RUNTIME_MIXED_PRIMARY_OWNER);
    } else {
        if (!diagnostics.frame_owned) {
            diagnostics.frame_owned = true;
            diagnostics.primary_owner = frame->scene.module;
            diagnostics.scene_generation = frame->scene_generation;
            frame_lane = XG_RENDER_FRAGMENT_FRAME_LANE_TYPED;
            boundary_description = *frame;
            boundary_description_valid = true;
        }
        compositor_result = xg_render_semantic_compositor_upsert(
            owner, authority, frame, fragment);
        result = producer_result((int)compositor_result,
                                 (int)XG_RENDER_SEMANTIC_COMPOSITOR_OK);
        if (result == XG_RENDER_FRAGMENT_RUNTIME_OK) {
            if (owner->domain == XG_RENDER_SEMANTIC_OWNER_MOVIE)
                diagnostics.production_movie_updates++;
            else if (owner->domain == XG_RENDER_SEMANTIC_OWNER_BATTLE_FX)
                diagnostics.production_battle_updates++;
        }
    }
    unlock_runtime();
    return result;
}

void xg_render_fragment_runtime_reject_source_frame(void) {
    lock_runtime();
    (void)reject_preflight(XG_RENDER_FRAGMENT_RUNTIME_TERMINAL_FAILURE);
    unlock_runtime();
}

XgRenderSourceFrameResult xg_render_fragment_runtime_finalize_source_frame(void) {
    XgRenderSourceFrameResult result;
    XgRenderSemanticBoundaryKind boundary_kind;
    lock_runtime();
    diagnostics.finalize_attempts++;
    if (finalized) {
        result = diagnostics.last_frame_result;
        unlock_runtime();
        return result;
    }
    ++lifecycle_generation;
    if (diagnostics.terminal_failure) {
        xg_render_source_frame_reset();
        result = XG_RENDER_SOURCE_FRAME_REJECTED;
    } else {
        result = xg_render_semantic_compositor_prepare_boundary(
            boundary_description_valid ? &boundary_description : NULL,
            &boundary_kind);
        diagnostics.last_boundary_kind = boundary_kind;
    }
    finalized = true;
    diagnostics.last_frame_result = result;
    if (result == XG_RENDER_SOURCE_FRAME_OK) diagnostics.finalized_frames++;
    else if (result == XG_RENDER_SOURCE_FRAME_EMPTY) diagnostics.empty_boundaries++;
    else {
        diagnostics.incomplete_boundaries++;
        diagnostics.terminal_failure = true;
        diagnostics.terminal_failures++;
        xg_render_source_frame_reset();
    }
    unlock_runtime();
    return result;
}

void xg_render_fragment_runtime_set_boundary_description(
        const XgRenderSourceFrameDescription *description) {
    lock_runtime();
    if (!finalized && !boundary_description_valid && description != NULL &&
        description->scene_generation != 0u) {
        boundary_description = *description;
        boundary_description_valid = true;
    }
    unlock_runtime();
}

void xg_render_fragment_runtime_finish_source_frame(bool published) {
    lock_runtime();
    if (!boundary_finished) {
        ++lifecycle_generation;
        xg_render_semantic_compositor_finish_boundary(published);
        boundary_finished = true;
    }
    unlock_runtime();
}

void xg_render_fragment_runtime_boundary_end(void) {
    lock_runtime();
    if (!boundary_finished)
        xg_render_semantic_compositor_finish_boundary(false);
    clear_frame_state();
    unlock_runtime();
}

void xg_render_fragment_runtime_invalidate_building(void) {
    lock_runtime();
    clear_frame_state();
    xg_render_semantic_compositor_invalidate_building();
    unlock_runtime();
}

void xg_render_fragment_runtime_invalidate(void) {
    lock_runtime();
    clear_frame_state();
    xg_render_semantic_compositor_reset();
    unlock_runtime();
}

void xg_render_fragment_runtime_reset_diagnostics(void) {
    lock_runtime();
    ++lifecycle_generation;
    memset(&diagnostics, 0, sizeof(diagnostics));
    diagnostics.last_frame_result = XG_RENDER_SOURCE_FRAME_EMPTY;
    frame_lane = XG_RENDER_FRAGMENT_FRAME_LANE_NONE;
    finalized = false;
    boundary_description_valid = false;
    boundary_finished = false;
    unlock_runtime();
}

void xg_render_fragment_runtime_diagnostics(
        XgRenderFragmentRuntimeDiagnostics *out_diagnostics) {
    if (out_diagnostics == NULL) return;
    lock_runtime();
    *out_diagnostics = diagnostics;
    unlock_runtime();
}
