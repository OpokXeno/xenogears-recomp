#ifndef XG_RENDER_FRAGMENT_RUNTIME_H
#define XG_RENDER_FRAGMENT_RUNTIME_H

#include "xg_render_semantic_compositor.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderFragmentRuntimeResult {
    XG_RENDER_FRAGMENT_RUNTIME_OK = 0,
    XG_RENDER_FRAGMENT_RUNTIME_INVALID_ARGUMENT,
    XG_RENDER_FRAGMENT_RUNTIME_MODULE_MISMATCH,
    XG_RENDER_FRAGMENT_RUNTIME_SCENE_GENERATION_MISMATCH,
    XG_RENDER_FRAGMENT_RUNTIME_MIXED_PRIMARY_OWNER,
    XG_RENDER_FRAGMENT_RUNTIME_PRODUCER_REJECTED,
    XG_RENDER_FRAGMENT_RUNTIME_TERMINAL_FAILURE,
} XgRenderFragmentRuntimeResult;

typedef struct XgRenderFragmentRuntimeDiagnostics {
    uint64_t ingress_attempts;
    uint64_t accepted_fragments;
    uint64_t module_mismatches;
    uint64_t scene_generation_mismatches;
    uint64_t mixed_primary_owner_rejections;
    uint64_t producer_rejections;
    uint64_t finalize_attempts;
    uint64_t finalized_frames;
    uint64_t empty_boundaries;
    uint64_t incomplete_boundaries;
    uint64_t terminal_failures;
    uint64_t production_field_world_updates;
    uint64_t production_resident_menu_updates;
    uint64_t production_battle_updates;
    uint64_t production_movie_updates;
    XgSemanticModuleKind primary_owner;
    uint32_t scene_generation;
    int32_t last_producer_result;
    XgRenderSourceFrameResult last_frame_result;
    bool frame_owned;
    bool field_movie_companion;
    bool battle_fx_companion;
    bool terminal_failure;
    XgRenderSemanticBoundaryKind last_boundary_kind;
} XgRenderFragmentRuntimeDiagnostics;

typedef struct XgRenderFragmentRuntimeTransaction XgRenderFragmentRuntimeTransaction;

/* Guest-owner transactions must not overlap. Rollback consumes the snapshot
 * without restoring either subsystem after reset/invalidation or a boundary. */
XgRenderFragmentRuntimeResult xg_render_fragment_runtime_transaction_begin(
    XgRenderFragmentRuntimeTransaction **out_transaction);
void xg_render_fragment_runtime_transaction_commit(
    XgRenderFragmentRuntimeTransaction *transaction);
void xg_render_fragment_runtime_transaction_rollback(
    XgRenderFragmentRuntimeTransaction *transaction);

/* Production adapter ingress. It assigns authenticated runtime fragments to
 * Field/World, Resident/Menu, Battle/Battling, or Movie
 * before any direct record is appended to the source frame. */
XgRenderFragmentRuntimeResult xg_render_fragment_runtime_ingress_production_adapter(
    const XgRenderSourceFrameDescription *frame);
/* Claims the direct production lane and atomically copies one authenticated
 * fragment into the runtime-owned SourceFrame. A rejected append makes the
 * current source boundary terminal rather than allowing a partial frame. */
XgRenderFragmentRuntimeResult xg_render_fragment_runtime_ingress_production_fragment(
    const XgRenderSourceFrameDescription *frame,
    const XgRenderSourceFrameFragment *fragment);
/* Classified production events that are already authenticated may update a
 * distinct retained owner without being collapsed into the primary module. */
XgRenderFragmentRuntimeResult
xg_render_fragment_runtime_ingress_authenticated_fragment(
    const XgRenderSourceFrameDescription *frame,
    const XgRenderSemanticOwnerKey *owner,
    const XgRenderSemanticOwnerAuthority *authority,
    const XgRenderSourceFrameFragment *fragment);
void xg_render_fragment_runtime_reject_source_frame(void);

/* Idempotent until boundary_end. It is the sole production owner of source
 * frame completion; producers only append fragments. */
XgRenderSourceFrameResult xg_render_fragment_runtime_finalize_source_frame(void);
void xg_render_fragment_runtime_set_boundary_description(
    const XgRenderSourceFrameDescription *description);
void xg_render_fragment_runtime_finish_source_frame(bool published);
void xg_render_fragment_runtime_boundary_end(void);
void xg_render_fragment_runtime_invalidate_building(void);
void xg_render_fragment_runtime_invalidate(void);
void xg_render_fragment_runtime_reset_diagnostics(void);
void xg_render_fragment_runtime_diagnostics(
    XgRenderFragmentRuntimeDiagnostics *out_diagnostics);

#ifdef __cplusplus
}
#endif

#endif
