#ifndef XG_RENDER_SOURCE_COMMIT_H
#define XG_RENDER_SOURCE_COMMIT_H

#include "xg_render_scene_snapshot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_SOURCE_COMMIT_CAPACITY
#define XG_RENDER_SOURCE_COMMIT_CAPACITY 16u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderSourceBuilder {
    uint32_t slot;
    uint32_t generation;
} XgRenderSourceBuilder;

typedef struct XgRenderSourceCommitHandle {
    uint32_t slot;
    uint32_t generation;
} XgRenderSourceCommitHandle;

typedef enum XgRenderSourceCommitResult {
    XG_RENDER_SOURCE_COMMIT_OK = 0,
    XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT,
    XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION,
    XG_RENDER_SOURCE_COMMIT_CAPACITY_EXCEEDED,
    XG_RENDER_SOURCE_COMMIT_PROOF_REJECTED,
    XG_RENDER_SOURCE_COMMIT_INVALID_ORDER,
    XG_RENDER_SOURCE_COMMIT_STALE_EPOCH,
    XG_RENDER_SOURCE_COMMIT_FROZEN,
    XG_RENDER_SOURCE_COMMIT_OUT_OF_RANGE,
} XgRenderSourceCommitResult;

typedef enum XgRenderSourceCommitState {
    XG_RENDER_SOURCE_EMPTY = 0,
    XG_RENDER_SOURCE_BUILDING,
    XG_RENDER_SOURCE_SEALED,
    XG_RENDER_SOURCE_QUEUED,
    XG_RENDER_SOURCE_RETIRED,
    XG_RENDER_SOURCE_REJECTED,
    XG_RENDER_SOURCE_CANCELLED,
} XgRenderSourceCommitState;

typedef struct XgRenderSourceCommitHeader {
    XgPresentationIdentity identity;
    XgSemanticSceneIdentity scene;
    XgSemanticDisplayState display;
    uint64_t digest;
    uint32_t pass_count;
    uint32_t draw_count;
    uint32_t resource_count;
    uint32_t surface_edge_count;
    uint32_t ui_node_count;
    uint32_t ui_glyph_run_count;
    uint32_t ui_glyph_placement_count;
    uint32_t native_operation_count;
    uint32_t motion_resource_count;
    uint32_t temporal_coverage_count;
    uint32_t temporal_publication_count;
    uint32_t source_interval_vblanks;
    XgRenderSourceCommitState state;
    bool discontinuity;
    bool temporally_eligible;
    bool native_work;
    bool display_boundary;
} XgRenderSourceCommitHeader;

/* Destructive initialization only, with no live builders/commits/consumers.
 * Production caller: semantic_presentation.initialize_locked, once. */
void xg_render_source_commit_reset(void);
void xg_render_source_commit_cancel_builders(void);
/* Scene generation and interval may be zero while constructing native work
 * (e.g. boot GPU writes). Seal still requires both nonzero for a scene commit.
 * Ordinary scene commits default to display_boundary=true. */
XgRenderSourceCommitResult xg_render_source_commit_begin(
    const XgPresentationIdentity *identity,
    const XgSemanticSceneIdentity *scene,
    const XgSemanticDisplayState *display,
    uint32_t source_interval_vblanks,
    bool discontinuity,
    bool temporally_eligible,
    XgRenderSourceBuilder *out_builder);
/* Selects the native-work lane before appending any scene records. Work is
 * never temporally interpolated. An empty display boundary is legal, including
 * a disabled display; mutation-only commits need no pass or output surface. */
XgRenderSourceCommitResult xg_render_source_commit_set_native_work(
    XgRenderSourceBuilder builder, bool display_boundary);
/* Refreshes only a BUILDING native-work header, before its digest is sealed. */
XgRenderSourceCommitResult xg_render_source_commit_set_native_display(
    XgRenderSourceBuilder builder, const XgSemanticDisplayState *display);
/* Refreshes publication timing only while native work is BUILDING. Epoch,
 * scene generation and source sequence must match the original identity;
 * VBlank/cycle may advance (including from zero), but never go backwards. */
XgRenderSourceCommitResult xg_render_source_commit_set_native_identity(
    XgRenderSourceBuilder builder, const XgPresentationIdentity *identity);
/* Atomically copies one operation in FIFO order. UPLOAD acquires its immutable
 * resource; DRAW acquires each motion pose once per commit. No append_resource
 * call is required. Failure appends nothing. */
XgRenderSourceCommitResult xg_render_source_commit_append_native_operation(
    XgRenderSourceBuilder builder, const XgRenderNativeOperation *operation);
XgRenderSourceCommitResult xg_render_source_commit_copy_native_operation(
    XgRenderSourceCommitHandle commit, size_t index,
    XgRenderNativeOperation *out_operation);
/* Borrowed reference from the commit's unique motion-resource set. A consumer
 * keeping history beyond commit retirement must acquire_snapshot/release it. */
XgRenderSourceCommitResult xg_render_source_commit_motion_resource_copy(
    XgRenderSourceCommitHandle commit, size_t index, XgRenderMotionRef *out_resource);
/* Metadata lane only: no native operation, raster mutation or op-accounting entry.
 * Append and DRAW binding each retain the snapshot once per commit. */
XgRenderSourceCommitResult xg_render_source_commit_append_temporal_coverage(
    XgRenderSourceBuilder builder, XgSemanticResourceRef coverage);
XgRenderSourceCommitResult xg_render_source_commit_temporal_coverage_copy(
    XgRenderSourceCommitHandle commit, size_t index, XgSemanticResourceRef *out_coverage);
XgRenderSourceCommitResult xg_render_source_commit_temporal_publication_copy(
    XgRenderSourceCommitHandle commit, size_t index, XgRenderTemporalPublication *out_publication);
/* Guest-owner creation; copies/canonicalizes all inputs. Returns ONE caller retain, already retired
 * from current-resource lookup. Release with xg_render_resource_release.
 * Duplicate vertex keys must have identical projection samples or creation fails. */
bool xg_render_temporal_coverage_create(
    const XgPresentationIdentity *identity, uint64_t source_update, uint32_t producer_scope,
    const XgRenderTemporalComponent *components, uint32_t component_count,
    const XgRenderTemporalSample *samples, uint32_t sample_count,
    XgSemanticResourceRef *out_coverage);
bool xg_render_temporal_coverage_view(
    XgSemanticResourceRef coverage, XgRenderTemporalCoverageView *out_view);
bool xg_render_temporal_components_compatible(
    const XgRenderTemporalCoverageHeader *previous, const XgRenderTemporalComponent *a,
    const XgRenderTemporalCoverageHeader *current, const XgRenderTemporalComponent *b);
XgRenderSourceCommitResult xg_render_source_commit_append_pass(
    XgRenderSourceBuilder builder,
    const XgSemanticPassRecord *pass);
XgRenderSourceCommitResult xg_render_source_commit_append_draw(
    XgRenderSourceBuilder builder,
    const XgSemanticDrawRecord *draw);
XgRenderSourceCommitResult xg_render_source_commit_append_resource(
    XgRenderSourceBuilder builder,
    const XgSemanticResourceRef *resource);
XgRenderSourceCommitResult xg_render_source_commit_append_surface_edge(
    XgRenderSourceBuilder builder,
    const XgSemanticSurfaceEdge *edge);
XgRenderSourceCommitResult xg_render_source_commit_append_ui_node(
    XgRenderSourceBuilder builder,
    const XgSemanticUiNodeRecord *node);
XgRenderSourceCommitResult xg_render_source_commit_append_ui_glyph_run(
    XgRenderSourceBuilder builder,
    const XgSemanticUiGlyphRunRecord *glyph_run);
XgRenderSourceCommitResult xg_render_source_commit_append_ui_glyph_placement(
    XgRenderSourceBuilder builder,
    const XgSemanticUiGlyphPlacementRecord *placement);
XgRenderSourceCommitResult xg_render_source_commit_seal(
    XgRenderSourceBuilder builder,
    XgRenderSourceCommitHandle *out_commit);
void xg_render_source_commit_cancel(XgRenderSourceBuilder builder);
XgRenderSourceCommitResult xg_render_source_commit_mark_queued(
    XgRenderSourceCommitHandle commit);
XgRenderSourceCommitResult xg_render_source_commit_retire(
    XgRenderSourceCommitHandle commit);
XgRenderSourceCommitResult xg_render_source_commit_header_copy(
    XgRenderSourceCommitHandle commit,
    XgRenderSourceCommitHeader *out_header);
XgRenderSourceCommitResult xg_render_source_commit_pass_copy(
    XgRenderSourceCommitHandle commit,
    size_t index,
    XgSemanticPassRecord *out_pass);
XgRenderSourceCommitResult xg_render_source_commit_draw_copy(
    XgRenderSourceCommitHandle commit,
    size_t index,
    XgSemanticDrawRecord *out_draw);
XgRenderSourceCommitResult xg_render_source_commit_resource_copy(
    XgRenderSourceCommitHandle commit,
    size_t index,
    XgSemanticResourceRef *out_resource);
XgRenderSourceCommitResult xg_render_source_commit_surface_edge_copy(
    XgRenderSourceCommitHandle commit,
    size_t index,
    XgSemanticSurfaceEdge *out_edge);
XgRenderSourceCommitResult xg_render_source_commit_ui_node_copy(
    XgRenderSourceCommitHandle commit,
    size_t index,
    XgSemanticUiNodeRecord *out_node);
XgRenderSourceCommitResult xg_render_source_commit_ui_glyph_run_copy(
    XgRenderSourceCommitHandle commit,
    size_t index,
    XgSemanticUiGlyphRunRecord *out_glyph_run);
XgRenderSourceCommitResult xg_render_source_commit_ui_glyph_placement_copy(
    XgRenderSourceCommitHandle commit,
    size_t index,
    XgSemanticUiGlyphPlacementRecord *out_placement);

#ifdef __cplusplus
}
#endif

#endif
