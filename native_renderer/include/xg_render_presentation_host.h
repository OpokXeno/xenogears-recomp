#ifndef XG_RENDER_PRESENTATION_HOST_H
#define XG_RENDER_PRESENTATION_HOST_H

#include "xg_render_semantic_presentation.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderPresentationHost XgRenderPresentationHost;

/* Implemented by the semantic presenter, not by the window backend. It must
 * retain/retire its endpoint and fences, validate the current epoch, and use
 * the same serialized swap authorization as present_next. No guest work. */
typedef bool (*XgRenderPresentationHoldPresenter)(
    const XgRenderPresenterServices *services);

typedef enum XgRenderPresentationHostState {
    XG_RENDER_PRESENTATION_HOST_RUNNING = 0,
    XG_RENDER_PRESENTATION_HOST_SHUTTING_DOWN,
    XG_RENDER_PRESENTATION_HOST_JOINED,
    XG_RENDER_PRESENTATION_HOST_FAILED,
} XgRenderPresentationHostState;

typedef struct XgRenderPresentationHostSnapshot {
    XgRenderPresentationHostState state;
    bool worker_running;
    /* True while an accepted owner pump or source-clock synchronization executes. */
    bool presenter_running;
    bool shutdown_requested;
    bool shutdown_complete;
    bool join_complete;
    /* Remains true through shutdown and is cleared by successful owner join. */
    bool presenter_owner_identity_valid;

    uint64_t notifications;
    uint64_t invalidations;
    uint64_t worker_attempts;
    uint64_t worker_compiled;
    uint64_t worker_empty;
    uint64_t worker_compile_failures;
    uint64_t worker_capacity_exceeded;
    uint64_t worker_stale;
    uint64_t worker_applied;
    uint64_t worker_would_block;
    uint64_t worker_lane_blocked;
    uint64_t presenter_attempts;
    uint64_t presenter_presented;
    uint64_t presenter_empty;
    uint64_t presenter_fence_pending;
    uint64_t presenter_fence_failures;
    uint64_t presenter_stale;
    uint64_t presenter_compose_failures;
    uint64_t presenter_owner_rejections;
    uint64_t presenter_hold_attempts;
    uint64_t presenter_held;
    uint64_t retirement_drains;
    uint64_t retirement_drain_failures;
    XgRenderWorkerResult last_worker_result;
    XgRenderPresenterResult last_presenter_result;
    XgRenderPresentationDiagnostics presentation;
} XgRenderPresentationHostSnapshot;

/*
 * Claims the process-wide semantic presentation pipeline, binds thrd_current()
 * as its immutable presenter owner, and starts one asynchronous render worker.
 * Callback-owned resources and user_data must remain valid until join
 * completes. presenter_services->owner_token is deliberately ignored: the host
 * creates a private token whose use is restricted to the bound owner thread.
 * A nonzero presentation period and a working monotonic clock are required.
 */
XgRenderPresentationHost *xg_render_presentation_host_start(
    const XgRenderWorkerServices *worker_services,
    const XgRenderPresenterServices *presenter_services,
    uint64_t presentation_period_ns);

/*
 * Guest-facing publication notification. This wakes only the asynchronous
 * worker and never performs or schedules presentation by itself.
 * Takes a short control-mutex critical section, never waiting for backend work.
 */
void xg_render_presentation_host_notify(XgRenderPresentationHost *host);

/*
 * Owner-thread service. Retirement polling has its own short deadline to
 * release FIFO capacity even between presents. At a presentation deadline it
 * attempts a new endpoint, then an optional hold if
 * no new endpoint is ready (EMPTY or FENCE_PENDING), or composition failed,
 * without waiting for a deadline or fence. Missed periods are skipped in O(1).
 * Foreign and reentrant calls return false and increment
 * presenter_owner_rejections before any backend operation.
 */
bool xg_render_presentation_host_pump(XgRenderPresentationHost *host);

/* Owner-only, non-reentrant. Relative monotonic duration, zero when either
 * retirement polling or presentation is due. Lets the outer SDL loop wait
 * for min(simulation deadline, host service deadline)
 * without sharing clock origins or making presentation advance the guest. */
bool xg_render_presentation_host_time_until_pump(
    XgRenderPresentationHost *host, uint64_t *out_nanoseconds);

/* Same owner/clock contract, but ignores the retirement-poll deadline. Lets a
 * cooperative guest execution boundary yield only when presentation is due. */
bool xg_render_presentation_host_time_until_present(
    XgRenderPresentationHost *host, uint64_t *out_nanoseconds);

/* Owner-only, between pumps. Retargets the presenter tick cadence live. */
bool xg_render_presentation_host_set_period(
    XgRenderPresentationHost *host, uint64_t period_ns);

/* Owner-only, between pumps. Pair a guest cycle with its signed wall-time offset
 * from now, without sharing the simulation clock's origin. Bind on a fresh epoch
 * or rate change; rebase also handles deliberate pauses/debt resets. This only
 * dates future publications, never postpones accepted work. Realtime means 1x. */
bool xg_render_presentation_host_sync_source_clock(
    XgRenderPresentationHost *host, uint64_t guest_cycle,
    int64_t guest_time_offset_ns, bool realtime, bool rebase);

/* Owner-only, between pumps. NULL disables idle holds, not completion of an
 * already accepted temporal interval. Use the semantic core's bool
 * present_hold directly; false means absent/pending/rejected, not an enum.
 * The callback uses the private owner token and must not reenter the host. */
bool xg_render_presentation_host_set_hold_presenter(
    XgRenderPresentationHost *host,
    XgRenderPresentationHoldPresenter present_hold);

/* Performs logical invalidation, then wakes the asynchronous worker. */
uint64_t xg_render_presentation_host_invalidate_or_wake(
    XgRenderPresentationHost *host,
    XgRenderTimelineInvalidationReason reason);

/* Idempotent.  The first call logically invalidates outstanding work. */
void xg_render_presentation_host_shutdown(XgRenderPresentationHost *host);

/*
 * The first join must be initiated by the bound presenter owner. It requests
 * shutdown, joins the worker, and drains retirement/quiesces the pipeline on
 * the owner before releasing the owner identity and exclusive claim. A
 * non-owner may wait only after an owner has initiated join; the worker itself
 * never waits in join. Repeated calls return the completed join result.
 */
bool xg_render_presentation_host_join(XgRenderPresentationHost *host);

bool xg_render_presentation_host_snapshot(
    XgRenderPresentationHost *host,
    XgRenderPresentationHostSnapshot *out_snapshot);

/*
 * Calls join when necessary. Once the owner has joined successfully, destroy
 * may be called from another thread. Returns false without freeing the host if
 * join/quiescence could not be confirmed. No other host API may race with
 * destroy.
 */
bool xg_render_presentation_host_destroy(XgRenderPresentationHost *host);

#ifdef __cplusplus
}
#endif

#endif
