#ifndef XG_RENDER_NATIVE_WORK_H
#define XG_RENDER_NATIVE_WORK_H

#include "gpu.h"
#include "xg_render_source_frame.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderNativeWorkServices {
    bool (*describe)(XgRenderSourceFrameDescription *description);
    bool (*wait)(void *user_data);
    void (*notify)(void *user_data);
    void *user_data;
    /* Optional non-waiting host service after an operation is copied/retained
     * and collector locks are released. Must return on the same guest stack,
     * without guest execution or collector reentry; false stops the caller. */
    bool (*service)(void *user_data);
} XgRenderNativeWorkServices;

/* Guest-owner only, including configuration and cancellation. Callbacks must
 * not reenter the collector; no collector/core lock is held across them.
 * describe and notify are required; wait may be NULL during pre-simulation
 * setup. NULL disables without cancelling pending or already-queued work. */
bool xg_render_native_work_configure(const XgRenderNativeWorkServices *services);
bool xg_render_native_work_enabled(void);
typedef struct XgRenderNativeWorkSnapshot {
    uint32_t buffered_operations;
    uint32_t buffered_uploads;
    bool building;
    bool sealed;
} XgRenderNativeWorkSnapshot;
/* Guest-owner only. Unpublished work is not included in the core FIFO depth. */
void xg_render_native_work_snapshot(XgRenderNativeWorkSnapshot *out_snapshot);
/* Inputs are borrowed only for the call and copied into immutable source work.
 * False means disabled, unavailable wait, failed lane, invalid input or epoch
 * cancellation; the caller must stop, not skip the rejected GPU operation. */
bool xg_render_native_work_draw(const GpuRenderSemantic *semantic,
                                uint64_t guest_cycle);
/* Host HD texture replacement, owned by the Native work stream while it is
 * enabled: the stream's own VRAM operations (UPLOAD/MOVE/CLEAR, never a
 * RESTORE rebase) feed the upload tracker, and every DRAW operation carries
 * the tracker's decision to the Native GPU. All callbacks run on the guest
 * owner in stream order. Zero-init (the default) leaves every draw on guest
 * VRAM. */
typedef struct XgRenderNativeHdTextureHooks {
    int (*resolve)(const GpuRenderSemantic *semantic, GpuRenderHdTexture *out);
    void (*upload)(int x, int y, int w, int h, const uint16_t *pixels);
    void (*copy)(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
    void (*fill)(int x, int y, int w, int h);
} XgRenderNativeHdTextureHooks;
void xg_render_native_work_set_hd_texture_hooks(
    const XgRenderNativeHdTextureHooks *hooks);
/* Guest-owner producer publication. Copies and retains metadata without emitting
 * an operation. out_coverage receives one caller retain; empty arrays are a valid
 * full-scope replacement, not permission to reuse last-seen geometry. */
bool xg_render_native_work_temporal_coverage(
    uint32_t producer_scope, const XgRenderTemporalComponent *components,
    uint32_t component_count, const XgRenderTemporalSample *samples,
    uint32_t sample_count, uint64_t guest_cycle, XgSemanticResourceRef *out_coverage);
/* Generic pointer-free FIFO operation, including explicit TARGET selection. */
bool xg_render_native_work_operation(const XgRenderNativeOperation *operation,
                                     uint64_t guest_cycle);
bool xg_render_native_work_vram_event(const GpuVramEvent *event,
                                      uint64_t guest_cycle);
/* Every chunk refreshes its publication identity before sealing, without
 * changing its source sequence. A boundary also refreshes the display
 * description, even for an empty chunk.
 * Sealed work remains owned here on WOULD_BLOCK and is retried via wait. */
bool xg_render_native_work_flush(bool display_boundary, uint64_t guest_cycle);
bool xg_render_native_work_drain(uint64_t guest_cycle);
/* Discards only this collector's unqueued builder/sealed work. Lifecycle
 * invalidation and queued-work retirement belong to the caller/core. Preserving
 * mutations requires drain before invalidation, then cancel_pending to rebase
 * the collector. Disabling services does not replace this cancellation. RESTORE
 * requires that invalidation already happened, then cancels and drains before
 * recording the complete replacement VRAM image in the new epoch. */
void xg_render_native_work_cancel_pending(void);

#ifdef __cplusplus
}
#endif

#endif
