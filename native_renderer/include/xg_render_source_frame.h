#ifndef XG_RENDER_SOURCE_FRAME_H
#define XG_RENDER_SOURCE_FRAME_H

#include "xg_render_source_commit.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderSourceFrameDescription {
    XgSemanticSceneIdentity scene;
    XgSemanticDisplayState display;
    uint32_t scene_generation;
    uint32_t source_interval_vblanks;
    bool discontinuity;
    bool temporally_eligible;
} XgRenderSourceFrameDescription;

/* Synchronous, borrowed view over an immutable producer fragment. The source
 * frame copies every record before returning; producer storage is never kept.
 * One runtime-owned frame may therefore merge a primary scene and authenticated
 * companions without giving any producer begin/complete/publication ownership. */
typedef struct XgRenderSourceFrameFragment {
    const XgSemanticPassRecord *passes;
    const XgSemanticDrawRecord *draws;
    const XgSemanticResourceRef *resources;
    const XgSemanticSurfaceEdge *surface_edges;
    const XgSemanticUiNodeRecord *ui_nodes;
    const XgSemanticUiGlyphRunRecord *ui_glyph_runs;
    const XgSemanticUiGlyphPlacementRecord *ui_glyph_placements;
    uint32_t pass_count;
    uint32_t draw_count;
    uint32_t resource_count;
    uint32_t surface_edge_count;
    uint32_t ui_node_count;
    uint32_t ui_glyph_run_count;
    uint32_t ui_glyph_placement_count;
    /* Pre-submission records may opt into late binding when an authenticated
     * GP0 transfer replaces a sampled VRAM publication in the same frame. */
    bool resources_follow_vram_mutations;
} XgRenderSourceFrameFragment;

typedef enum XgRenderSourceFrameResult {
    XG_RENDER_SOURCE_FRAME_OK = 0,
    XG_RENDER_SOURCE_FRAME_EMPTY,
    XG_RENDER_SOURCE_FRAME_INCOMPLETE,
    XG_RENDER_SOURCE_FRAME_INVALID_ARGUMENT,
    XG_RENDER_SOURCE_FRAME_INVALID_TRANSITION,
    XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED,
    XG_RENDER_SOURCE_FRAME_REJECTED,
    XG_RENDER_SOURCE_FRAME_CAPTURE_FAILED,
} XgRenderSourceFrameResult;

typedef bool (*XgRenderSourceFrameSealedCaptureCallback)(
    XgRenderSourceCommitHandle commit,
    const XgRenderSourceCommitHeader *copied_header,
    void *user_data);
typedef void (*XgRenderSourceFramePublishedNotifyCallback)(void *user_data);

typedef struct XgRenderSourceFrameHostCallbacks {
    /* Called once for a sealed commit before queue publication. */
    XgRenderSourceFrameSealedCaptureCallback sealed_capture;
    /* Called only after that commit is successfully published. */
    XgRenderSourceFramePublishedNotifyCallback published_notify;
    void *user_data;
} XgRenderSourceFrameHostCallbacks;

typedef struct XgRenderSourceFrameSnapshot {
    uint64_t presentation_epoch;
    uint32_t scene_generation;
    uint32_t pass_count;
    uint32_t draw_count;
    uint32_t resource_count;
    uint32_t surface_edge_count;
    uint32_t ui_node_count;
    uint32_t ui_glyph_run_count;
    uint32_t ui_glyph_placement_count;
    bool active;
    bool complete;
    bool blocked;
} XgRenderSourceFrameSnapshot;

/* Cancels pending and captured-but-not-queued frames. Already-running host
 * callbacks are not drained by this reset. */
void xg_render_source_frame_reset(void);
/* Marks the active frame terminally invalid without discarding its resources.
 * The source boundary performs the single fail-closed reset. */
void xg_render_source_frame_reject(void);
/* Both callbacks are required. Reset preserves them; clear removes them.
 * Configure/clear require guest-owner quiescence outside publish_boundary;
 * main.cpp stops the host there. Neither operation drains captured callbacks. */
bool xg_render_source_frame_configure_host_callbacks(
    const XgRenderSourceFrameHostCallbacks *callbacks);
void xg_render_source_frame_clear_host_callbacks(void);
XgRenderSourceFrameResult xg_render_source_frame_begin(
    const XgRenderSourceFrameDescription *description);
/* Atomically begins-or-merges one fragment. On failure no fragment records are
 * appended and all resource acquisitions made by the attempt are rolled back. */
XgRenderSourceFrameResult xg_render_source_frame_append_fragment(
    const XgRenderSourceFrameDescription *description,
    const XgRenderSourceFrameFragment *fragment);
XgRenderSourceFrameResult xg_render_source_frame_append_pass(
    const XgSemanticPassRecord *pass);
XgRenderSourceFrameResult xg_render_source_frame_append_draw(
    const XgSemanticDrawRecord *draw);
XgRenderSourceFrameResult xg_render_source_frame_append_resource(
    const XgSemanticResourceRef *resource);
XgRenderSourceFrameResult xg_render_source_frame_append_surface_edge(
    const XgSemanticSurfaceEdge *edge);
XgRenderSourceFrameResult xg_render_source_frame_append_ui_node(
    const XgSemanticUiNodeRecord *node);
XgRenderSourceFrameResult xg_render_source_frame_append_ui_glyph_run(
    const XgSemanticUiGlyphRunRecord *glyph_run);
XgRenderSourceFrameResult xg_render_source_frame_append_ui_glyph_placement(
    const XgSemanticUiGlyphPlacementRecord *placement);
XgRenderSourceFrameResult xg_render_source_frame_complete(void);
XgRenderSourceFrameResult xg_render_source_frame_publish_boundary(void);
void xg_render_source_frame_snapshot(XgRenderSourceFrameSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif
