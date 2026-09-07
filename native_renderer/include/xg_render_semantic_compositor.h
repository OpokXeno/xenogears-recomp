#ifndef XG_RENDER_SEMANTIC_COMPOSITOR_H
#define XG_RENDER_SEMANTIC_COMPOSITOR_H

#include "xg_render_source_frame.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XG_RENDER_SEMANTIC_OWNER_CAPACITY 64u

typedef enum XgRenderSemanticOwnerDomain {
    XG_RENDER_SEMANTIC_OWNER_DISPLAY = 0,
    XG_RENDER_SEMANTIC_OWNER_FIELD_WORLD,
    XG_RENDER_SEMANTIC_OWNER_RESIDENT_MENU,
    XG_RENDER_SEMANTIC_OWNER_BATTLE_PRIMARY,
    XG_RENDER_SEMANTIC_OWNER_BATTLE_FX,
    XG_RENDER_SEMANTIC_OWNER_MOVIE,
    XG_RENDER_SEMANTIC_OWNER_DOMAIN_COUNT,
} XgRenderSemanticOwnerDomain;

typedef struct XgRenderSemanticOwnerKey {
    XgSemanticModuleKind module;
    XgRenderSemanticOwnerDomain domain;
    uint64_t primary_root;
    uint64_t secondary_root;
} XgRenderSemanticOwnerKey;

typedef struct XgRenderSemanticOwnerAuthority {
    uint32_t scene_generation;
    uint64_t owner_generation;
    uint64_t receipt;
    uint64_t proof;
} XgRenderSemanticOwnerAuthority;

typedef enum XgRenderSemanticCompositorResult {
    XG_RENDER_SEMANTIC_COMPOSITOR_OK = 0,
    XG_RENDER_SEMANTIC_COMPOSITOR_INVALID_ARGUMENT,
    XG_RENDER_SEMANTIC_COMPOSITOR_SCENE_MISMATCH,
    XG_RENDER_SEMANTIC_COMPOSITOR_AUTHORITY_REJECTED,
    XG_RENDER_SEMANTIC_COMPOSITOR_RESOURCE_REJECTED,
    XG_RENDER_SEMANTIC_COMPOSITOR_CAPACITY_EXCEEDED,
    XG_RENDER_SEMANTIC_COMPOSITOR_TOPOLOGY_REJECTED,
    XG_RENDER_SEMANTIC_COMPOSITOR_BLOCKED,
} XgRenderSemanticCompositorResult;

typedef enum XgRenderSemanticBoundaryKind {
    XG_RENDER_SEMANTIC_BOUNDARY_HOLD = 0,
    XG_RENDER_SEMANTIC_BOUNDARY_ENDPOINT,
    XG_RENDER_SEMANTIC_BOUNDARY_DISPLAY_ROOT,
    XG_RENDER_SEMANTIC_BOUNDARY_REJECTED,
} XgRenderSemanticBoundaryKind;

typedef struct XgRenderSemanticCompositorDiagnostics {
    uint64_t update_attempts;
    uint64_t accepted_updates;
    uint64_t removals;
    uint64_t endpoint_boundaries;
    uint64_t display_root_boundaries;
    uint64_t hold_boundaries;
    uint64_t rejected_boundaries;
    uint64_t committed_boundaries;
    uint64_t discarded_boundaries;
    uint64_t completed_field_submissions;
    uint64_t superseded_field_submissions;
    uint64_t display_matched_field_submissions;
    uint64_t semantic_resets;
    uint64_t building_invalidations;
    uint64_t mutation_rebind_attempts;
    uint64_t mutation_rebind_matches;
    uint64_t mutation_rebind_misses;
    uint64_t mutation_rebound_draws;
    uint64_t mutation_materialized_draws;
    uint64_t mutation_previous_resource_id;
    uint64_t mutation_previous_generation;
    uint64_t mutation_replacement_resource_id;
    uint64_t mutation_replacement_generation;
    uint32_t retained_owner_count;
    uint32_t retained_pass_count;
    uint32_t retained_draw_count;
    uint32_t retained_resource_count;
    uint32_t retained_edge_count;
    uint32_t retained_ui_node_count;
    uint32_t completed_field_candidate_count;
    XgRenderSemanticBoundaryKind last_boundary;
    XgRenderSemanticCompositorResult last_result;
    bool pending;
    bool prepared;
    bool retained;
} XgRenderSemanticCompositorDiagnostics;

typedef struct XgRenderSemanticCompositorTransaction
    XgRenderSemanticCompositorTransaction;

/* Guest-owner transaction (fragment_runtime/movie scanout); transactions must not
 * overlap. Reset, invalidation and boundary transitions invalidate rollback. */
XgRenderSemanticCompositorResult
xg_render_semantic_compositor_transaction_begin(
    XgRenderSemanticCompositorTransaction **out_transaction);
void xg_render_semantic_compositor_transaction_commit(
    XgRenderSemanticCompositorTransaction *transaction);
/* Consumes the snapshot even when stale; true only if pending was restored. */
bool xg_render_semantic_compositor_transaction_rollback(
    XgRenderSemanticCompositorTransaction *transaction);

/* UPSERT replaces this owner's retained records on its first update in the
 * current VBlank and appends on subsequent updates in that same transaction. */
XgRenderSemanticCompositorResult xg_render_semantic_compositor_upsert(
    const XgRenderSemanticOwnerKey *owner,
    const XgRenderSemanticOwnerAuthority *authority,
    const XgRenderSourceFrameDescription *description,
    const XgRenderSourceFrameFragment *fragment);
XgRenderSemanticCompositorResult
xg_render_semantic_compositor_rebind_mutated_resource(
    const XgSemanticResourceRef *previous,
    const XgSemanticResourceRef *replacement);
XgRenderSemanticCompositorResult xg_render_semantic_compositor_remove(
    const XgRenderSemanticOwnerKey *owner,
    const XgRenderSemanticOwnerAuthority *authority,
    const XgRenderSourceFrameDescription *description);

/* Seals Field updates only after the corresponding DMA2 linked list has
 * completed. The draw region is the GPU state after that list executed. */
void xg_render_semantic_compositor_complete_field_submission(
    uint32_t start_address, uint32_t transferred_words,
    uint16_t draw_left, uint16_t draw_top,
    uint16_t draw_right, uint16_t draw_bottom);
bool xg_render_semantic_compositor_field_boundary_target_required(
    const XgRenderSourceFrameDescription *current_description);
bool xg_render_semantic_compositor_set_field_boundary_target(
    const XgRenderSourceFrameDescription *current_description,
    const XgSemanticResourceRef *target);

/* Materializes exactly one complete SourceFrame or classifies a true hold.
 * finish_boundary installs the candidate only after queue publication. */
XgRenderSourceFrameResult xg_render_semantic_compositor_prepare_boundary(
    const XgRenderSourceFrameDescription *current_description,
    XgRenderSemanticBoundaryKind *out_kind);
void xg_render_semantic_compositor_finish_boundary(bool published);
void xg_render_semantic_compositor_invalidate_building(void);
void xg_render_semantic_compositor_reset(void);
void xg_render_semantic_compositor_diagnostics(
    XgRenderSemanticCompositorDiagnostics *out_diagnostics);

#ifdef __cplusplus
}
#endif

#endif
