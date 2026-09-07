#ifndef XG_RENDER_SEMANTIC_PRESENTATION_H
#define XG_RENDER_SEMANTIC_PRESENTATION_H

#include "xg_render_source_commit.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef XG_RENDER_PRESENTATION_BATCH_CAPACITY
/* Four guest ticks of visual latency, plus retained/compiling endpoints. */
#define XG_RENDER_PRESENTATION_BATCH_CAPACITY 8u
#endif

#ifndef XG_RENDER_PRESENTATION_TRACE_CAPACITY
#define XG_RENDER_PRESENTATION_TRACE_CAPACITY 1024u
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderTimelineInvalidationReason {
    XG_RENDER_TIMELINE_RESET = 0,
    XG_RENDER_TIMELINE_RESTORE,
    XG_RENDER_TIMELINE_ROLLBACK,
    XG_RENDER_TIMELINE_DISC_CHANGE,
    XG_RENDER_TIMELINE_ARTIFACT_CHANGE,
    XG_RENDER_TIMELINE_SCENE_CHANGE,
    XG_RENDER_TIMELINE_INVALIDATION_REASON_COUNT,
} XgRenderTimelineInvalidationReason;

typedef enum XgRenderTimelineResult {
    XG_RENDER_TIMELINE_OK = 0,
    XG_RENDER_TIMELINE_INVALID_ARGUMENT,
    XG_RENDER_TIMELINE_DUPLICATE_BOUNDARY,
    XG_RENDER_TIMELINE_STALE_COMMIT,
    XG_RENDER_TIMELINE_SEQUENCE_MISMATCH,
    XG_RENDER_TIMELINE_QUEUE_FAILED,
    XG_RENDER_TIMELINE_PUBLICATION_CLOSED,
    XG_RENDER_TIMELINE_SCENE_CHANGE_REQUIRES_INVALIDATION,
    XG_RENDER_TIMELINE_EPOCH_EXHAUSTED,
    XG_RENDER_TIMELINE_WOULD_BLOCK,
    XG_RENDER_TIMELINE_LANE_BLOCKED,
    XG_RENDER_TIMELINE_SEQUENCE_EXHAUSTED,
} XgRenderTimelineResult;

typedef enum XgRenderPresentationLifecycleResult {
    XG_RENDER_PRESENTATION_LIFECYCLE_OK = 0,
    XG_RENDER_PRESENTATION_LIFECYCLE_INVALID_ARGUMENT,
    XG_RENDER_PRESENTATION_LIFECYCLE_ALREADY_OPEN,
    XG_RENDER_PRESENTATION_LIFECYCLE_ALREADY_CLOSED,
    XG_RENDER_PRESENTATION_LIFECYCLE_OWNER_REJECTED,
    XG_RENDER_PRESENTATION_LIFECYCLE_NOT_QUIESCENT,
    XG_RENDER_PRESENTATION_LIFECYCLE_EPOCH_EXHAUSTED,
} XgRenderPresentationLifecycleResult;

typedef struct XgRenderPresentationBatchHandle {
    uint32_t slot;
    uint32_t generation;
} XgRenderPresentationBatchHandle;

typedef enum XgRenderFenceStatus {
    XG_RENDER_FENCE_PENDING = 0,
    XG_RENDER_FENCE_READY,
    XG_RENDER_FENCE_FAILED,
} XgRenderFenceStatus;

/*
 * Immutable value produced by the render worker.  opaque_handle is meaningful
 * only to the backend which produced it; the remaining fields let the core
 * validate and route the endpoint without retaining backend-owned pointers.
 */
typedef struct XgRenderCompiledEndpoint {
    uint64_t opaque_handle;
    XgPresentationIdentity identity;
    uint64_t digest;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t backend_generation;
    /* Backend-approved complete images at k/(N+1), k=1..N; 1/1 is always
     * the original logical endpoint. Zero N disables temporal presentation.
     * Missing interval/digest proof falls back to 1/1, never vertex blending. */
    uint32_t temporal_phase_count;
    uint64_t temporal_interval_ns;
    /* Logical final-image identity, not the last intermediate image swapped.
     * Zero is unknown (legacy/non-native endpoints) and cannot match a base. */
    uint64_t pixel_digest;
    uint64_t temporal_previous_pixel_digest;
} XgRenderCompiledEndpoint;

typedef enum XgRenderCompileResult {
    XG_RENDER_COMPILE_FAILED = 0,
    XG_RENDER_COMPILE_ENDPOINT_READY = 1,
    XG_RENDER_COMPILE_APPLIED = 2,
    XG_RENDER_COMPILE_WOULD_BLOCK = 3,
} XgRenderCompileResult;

typedef struct XgRenderWorkerServices {
    /*
     * Serialized FIFO work, including offscreen/display-disabled mutations.
     * ENDPOINT_READY transfers an endpoint and a nonzero compile fence; the
     * source is ACKed only when that fence reports READY. The endpoint must
     * own/copy its inputs independently of the commit by then.
     * APPLIED is a synchronous mutation-only ACK, with both outputs zero.
     * WOULD_BLOCK must have NO effects and both outputs zero; the same head
     * is retried. FAILED (including malformed outputs or a failed fence)
     * blocks the lane until invalidation; later deltas are never skipped to.
     * Epoch changes require backend mutation state to be reset/rebased before
     * applying the new epoch. No next work starts while old compile cleanup
     * is queued or running, even when invalidation deferred it to the presenter.
     * Any outstanding asynchronous input access, even on FAILED, must return
     * a fence. A zero fence promises no remaining source/endpoint access.
     */
    XgRenderCompileResult (*compile)(XgRenderSourceCommitHandle commit,
                    XgRenderCompiledEndpoint *out_endpoint,
                    uint64_t *out_compile_fence,
                    void *user_data);
    /* Any-thread callback; the adapter must marshal context-affine work. */
    XgRenderFenceStatus (*fence_status)(uint64_t fence, void *user_data);
    /*
     * Any-thread callback. Required; must cancel or wait until the fence no
     * longer protects work, marshaling to the backend owner when necessary.
     */
    void (*discard_fence)(uint64_t fence, void *user_data);
    /*
     * Required.  A nonzero out_endpoint->opaque_handle transfers exactly one
     * ownership reference to the core, even when compile subsequently reports
     * failure.  The core calls release_endpoint exactly once, after every fence
     * which can protect the endpoint has been discarded.  It is never called
     * while the presentation lock is held. It is an any-thread callback; a GL
     * adapter must marshal destruction to its context owner.
     */
    void (*release_endpoint)(const XgRenderCompiledEndpoint *endpoint,
                             void *user_data);
    void *user_data;
} XgRenderWorkerServices;

typedef struct XgRenderPresenterServices {
    /* Any-thread callback; the adapter must marshal context-affine work. */
    XgRenderFenceStatus (*fence_status)(uint64_t fence, void *user_data);
    /* Alpha selects a backend-approved complete image (pose/geometry replay),
     * never independent vertex interpolation or a mutation of canonical source.
     * Success requires a nonzero completion fence. Failure must also return a
     * fence for any submitted work; zero promises no remaining endpoint access. */
    bool (*compose)(const XgRenderCompiledEndpoint *endpoint,
                    uint64_t alpha_numerator,
                    uint64_t alpha_denominator,
                    uint64_t *out_completion_fence,
                    void *user_data);
    /* Called only after an irrevocable, lock-serialized swap authorization. */
    void (*swap_window)(void *user_data);
    /* Must cancel/wait until no work is protected before returning, like the
     * worker discard_fence. Any-thread; marshal to the owner as necessary. */
    void (*discard_fence)(uint64_t fence, void *user_data);
    void *user_data;
    /* Required stable token for the single presenter owner. */
    uint64_t owner_token;
    /* Optional for whole endpoints. Presenter-only monotonic nanoseconds;
     * called without the core lock. No clock or a backwards sample selects
     * 1/1 permanently for that endpoint. Callback/user_data must remain stable
     * within an epoch; a changed clock also falls back. Zero time is valid. */
    uint64_t (*clock_ns)(void *user_data);
    /* Optional owner-sampled deadline in clock_ns's domain. Both next and hold
     * in one host tick use this same sample, excluding wake/compose jitter. */
    uint64_t clock_sample_ns;
    bool clock_sample_valid;
} XgRenderPresenterServices;

typedef enum XgRenderWorkerResult {
    XG_RENDER_WORKER_OK = 0,
    XG_RENDER_WORKER_EMPTY,
    XG_RENDER_WORKER_COMPILE_FAILED,
    XG_RENDER_WORKER_CAPACITY_EXCEEDED,
    XG_RENDER_WORKER_STALE,
    XG_RENDER_WORKER_APPLIED,
    XG_RENDER_WORKER_WOULD_BLOCK,
    XG_RENDER_WORKER_LANE_BLOCKED,
} XgRenderWorkerResult;

typedef enum XgRenderPresenterResult {
    XG_RENDER_PRESENTER_PRESENTED = 0,
    XG_RENDER_PRESENTER_EMPTY,
    XG_RENDER_PRESENTER_FENCE_PENDING,
    XG_RENDER_PRESENTER_FENCE_FAILED,
    XG_RENDER_PRESENTER_STALE,
    XG_RENDER_PRESENTER_COMPOSE_FAILED,
    XG_RENDER_PRESENTER_OWNER_REJECTED,
} XgRenderPresenterResult;

typedef enum XgRenderEndpointMismatch {
    XG_RENDER_ENDPOINT_MISMATCH_CALLBACK = 1u << 0,
    XG_RENDER_ENDPOINT_MISMATCH_COMPILE_FENCE = 1u << 1,
    XG_RENDER_ENDPOINT_MISMATCH_HANDLE = 1u << 2,
    XG_RENDER_ENDPOINT_MISMATCH_PRESENTATION_EPOCH = 1u << 3,
    XG_RENDER_ENDPOINT_MISMATCH_SOURCE_SEQUENCE = 1u << 4,
    XG_RENDER_ENDPOINT_MISMATCH_GUEST_VBLANK = 1u << 5,
    XG_RENDER_ENDPOINT_MISMATCH_GUEST_CYCLE = 1u << 6,
    XG_RENDER_ENDPOINT_MISMATCH_SCENE_GENERATION = 1u << 7,
    XG_RENDER_ENDPOINT_MISMATCH_DIGEST = 1u << 8,
    XG_RENDER_ENDPOINT_MISMATCH_WIDTH = 1u << 9,
    XG_RENDER_ENDPOINT_MISMATCH_HEIGHT = 1u << 10,
    XG_RENDER_ENDPOINT_MISMATCH_FORMAT = 1u << 11,
    XG_RENDER_ENDPOINT_MISMATCH_BACKEND_GENERATION = 1u << 12,
    XG_RENDER_ENDPOINT_MISMATCH_COUNT = 13,
} XgRenderEndpointMismatch;

typedef struct XgRenderPresentationReceipt {
    XgPresentationIdentity identity;
    uint64_t semantic_digest;
    uint64_t opaque_handle;
    uint64_t backend_generation;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t temporal_phase_count;
    uint64_t temporal_interval_ns;
    uint64_t pixel_digest;
    uint64_t temporal_previous_pixel_digest;
    bool valid;
} XgRenderPresentationReceipt;

typedef enum XgRenderPresentationTraceFlag {
    XG_RENDER_PRESENTATION_TRACE_PUBLISHED = UINT64_C(1) << 0,
    XG_RENDER_PRESENTATION_TRACE_SOURCE_COALESCED = UINT64_C(1) << 1,
    XG_RENDER_PRESENTATION_TRACE_COMPILE_STARTED = UINT64_C(1) << 2,
    XG_RENDER_PRESENTATION_TRACE_COMPILE_CALLBACK_OK = UINT64_C(1) << 3,
    XG_RENDER_PRESENTATION_TRACE_ENDPOINT_VALIDATED = UINT64_C(1) << 4,
    XG_RENDER_PRESENTATION_TRACE_COMPILE_FAILED = UINT64_C(1) << 5,
    XG_RENDER_PRESENTATION_TRACE_COMPILE_SUPERSEDED = UINT64_C(1) << 6,
    XG_RENDER_PRESENTATION_TRACE_BATCH_COALESCED = UINT64_C(1) << 7,
    XG_RENDER_PRESENTATION_TRACE_COMPILE_FENCE_PENDING = UINT64_C(1) << 8,
    XG_RENDER_PRESENTATION_TRACE_COMPILE_FENCE_FAILED = UINT64_C(1) << 9,
    XG_RENDER_PRESENTATION_TRACE_COMPOSE_STARTED = UINT64_C(1) << 10,
    XG_RENDER_PRESENTATION_TRACE_COMPOSE_SUCCEEDED = UINT64_C(1) << 11,
    XG_RENDER_PRESENTATION_TRACE_COMPOSE_FAILED = UINT64_C(1) << 12,
    XG_RENDER_PRESENTATION_TRACE_RETIRED_BEFORE_SWAP = UINT64_C(1) << 13,
    XG_RENDER_PRESENTATION_TRACE_SWAP_AUTHORIZED = UINT64_C(1) << 14,
    XG_RENDER_PRESENTATION_TRACE_SWAP_CALLBACK_RETURNED = UINT64_C(1) << 15,
    XG_RENDER_PRESENTATION_TRACE_COMPLETION_FENCE_PENDING = UINT64_C(1) << 16,
    XG_RENDER_PRESENTATION_TRACE_COMPLETION_FENCE_FAILED = UINT64_C(1) << 17,
    XG_RENDER_PRESENTATION_TRACE_STALE = UINT64_C(1) << 18,
    XG_RENDER_PRESENTATION_TRACE_INVALIDATED = UINT64_C(1) << 19,
    XG_RENDER_PRESENTATION_TRACE_WORK_ACKED = UINT64_C(1) << 20,
    XG_RENDER_PRESENTATION_TRACE_WORK_APPLIED = UINT64_C(1) << 21,
    XG_RENDER_PRESENTATION_TRACE_WORK_WOULD_BLOCK = UINT64_C(1) << 22,
} XgRenderPresentationTraceFlag;

typedef struct XgRenderPresentationTraceEvent {
    uint64_t trace_sequence;
    uint64_t flags;
    XgRenderPresentationReceipt source;
    XgRenderPresentationReceipt endpoint;
    uint64_t compile_fence;
    uint64_t completion_fence;
    uint32_t endpoint_mismatch_mask;
    XgRenderWorkerResult worker_result;
    XgRenderPresenterResult presenter_result;
} XgRenderPresentationTraceEvent;

typedef struct XgRenderPresentationDiagnostics {
    uint64_t presentation_epoch;
    uint64_t source_sequence; /* Last published work, including mutation-only. */
    uint64_t guest_vblank_sequence;
    uint64_t guest_cycle;
    uint64_t boundary_count;
    uint64_t missed_boundaries;
    uint64_t duplicate_boundaries;
    uint64_t invalidation_count;
    uint64_t invalidations_by_reason[
        XG_RENDER_TIMELINE_INVALIDATION_REASON_COUNT];
    uint64_t published_commits;
    uint64_t presentation_holds; /* Source holds; host swaps use presented_holds. */
    uint64_t hold_attempts;
    uint64_t presented_holds;
    uint64_t source_work_acks;
    /* Published FIFO entries cancelled by lifecycle invalidation, not ACKs. */
    uint64_t invalidated_source_work;
    uint64_t applied_source_work;
    uint64_t source_queue_would_block;
    uint64_t compile_would_block;
    uint64_t worker_serialization_rejections;
    uint64_t rejected_commits;
    uint64_t superseded_endpoints;
    uint64_t stale_commits;
    uint64_t stale_batches;
    uint64_t compile_failures;
    uint64_t fence_failures;
    uint64_t presented_endpoints;
    uint64_t worker_compile_attempts;
    uint64_t validated_endpoints;
    uint64_t endpoint_validation_failures;
    uint64_t endpoint_mismatches[XG_RENDER_ENDPOINT_MISMATCH_COUNT];
    uint64_t compose_attempts;
    uint64_t composed_endpoints;
    uint64_t compose_failures;
    uint64_t swap_authorizations;
    uint64_t visual_only_updates;
    uint64_t source_coalesces;
    uint64_t batch_coalesces;
    uint64_t worker_backpressure;
    uint64_t source_capacity_drops;
    uint64_t batch_capacity_drops;
    uint64_t dropped_phases;
    uint64_t retirement_capacity_failures;
    uint64_t lifecycle_open_count;
    uint64_t lifecycle_close_count;
    uint64_t lifecycle_rejections;
    uint64_t presenter_owner_token;
    XgRenderTimelineInvalidationReason last_invalidation_reason;
    uint32_t scene_generation;
    uint32_t source_queue_depth;
    uint32_t source_queue_capacity;
    uint32_t batch_queue_depth;
    /* Excludes idle retained history; includes unfinished completion fences. */
    uint32_t pending_batch_count;
    uint32_t batch_queue_capacity;
    uint32_t retirement_queue_depth;
    uint32_t retirement_queue_capacity;
    uint32_t last_endpoint_mismatch_mask;
    XgRenderPresentationReceipt last_published_source;
    XgRenderPresentationReceipt last_compile_source;
    XgRenderPresentationReceipt last_compile_endpoint;
    XgRenderPresentationReceipt last_composed_endpoint;
    XgRenderPresentationReceipt last_swap_authorized_endpoint;
    XgRenderPresentationReceipt last_acked_source;
    XgRenderPresentationReceipt blocked_source;
    XgRenderCompileResult last_compile_result;
    /* Last swap-authorized fraction. Whole/fallback endpoints use exactly 1/1. */
    uint64_t last_alpha_numerator;
    uint64_t last_alpha_denominator;
    bool source_pending;
    bool source_lane_blocked;
    /* Also true until deferred invalidated compile fences/inputs are retired. */
    bool worker_busy;
    bool retained_endpoint;
    bool publication_open;
    bool epoch_terminal;
    /*
     * False while an irrevocably authorized swap executes; post-swap fence
     * retirement may make this true again until cleanup completes.
     */
    bool batch_pending;
} XgRenderPresentationDiagnostics;

/*
 * Logical, non-blocking reset. Epochs never move backwards and publication is
 * left closed; tests must explicitly claim/open a fresh quiescent lifecycle.
 */
void xg_render_semantic_presentation_reset(void);
/*
 * Claims the presenter token and opens publication only from a fully quiescent
 * closed core. This prevents a newly started host from adopting prior work.
 */
XgRenderPresentationLifecycleResult
xg_render_presentation_lifecycle_claim_open(uint64_t owner_token);
/* Atomically closes publication and logically invalidates the current epoch. */
XgRenderPresentationLifecycleResult
xg_render_presentation_lifecycle_close(
    uint64_t owner_token,
    XgRenderTimelineInvalidationReason reason,
    uint64_t *out_epoch);
XgRenderTimelineResult xg_render_timeline_source_boundary(
    uint64_t guest_vblank_sequence, uint64_t guest_cycle);
/*
 * Invalidation is logical/non-blocking; backend cleanup is deferred. Reaching
 * UINT64_MAX permanently closes publication and leaves the core fail-closed.
 */
uint64_t xg_render_timeline_invalidate(
    XgRenderTimelineInvalidationReason reason);
XgRenderTimelineResult xg_render_timeline_next_identity(
    uint32_t scene_generation, XgPresentationIdentity *out_identity);
/* Guest-owner identity peek, not a reservation. source_sequence orders ALL
 * work, not just visible frames. VBlank zero is valid before the first boundary.
 * Serialize identity/build/publish; preserve the sealed work on WOULD_BLOCK. */
XgRenderTimelineResult xg_render_timeline_next_work_identity(
    uint32_t scene_generation, uint64_t guest_cycle,
    XgPresentationIdentity *out_identity);
void xg_render_timeline_note_hold(void);
void xg_render_timeline_note_rejection(void);
/* Advisory preflight, not a slot reservation. Also false if closed/blocked.
 * Commit-pool capacity is shared with builders and in-flight work. */
bool xg_render_source_queue_has_capacity(void);
/* Only OK transfers ownership. A full FIFO returns WOULD_BLOCK without
 * marking QUEUED or consuming the commit. Invalidation cancels the old epoch. */
XgRenderTimelineResult xg_render_source_queue_publish(
    XgRenderSourceCommitHandle commit);
/* One compile or fence poll, never a polling loop. Continue on OK/APPLIED;
 * sleep/rearm on WOULD_BLOCK or CAPACITY_EXCEEDED, and stop the lane on
 * COMPILE_FAILED/LANE_BLOCKED until invalidation. Fence readiness/retirement
 * capacity must wake or periodically rearm the host worker without guest work. */
XgRenderWorkerResult xg_render_worker_compile_next(
    const XgRenderWorkerServices *services);
/* Read-only snapshot for the active compile callback's current-epoch FIFO head.
 * Returns its fixed temporal-candidate deadline in XgRenderPresenterServices.clock_ns's
 * domain. A successful zero means known discrete/whole-only/unpaced/unbound, not
 * query failure. If the compiled endpoint has no usable phases, the presenter
 * does not delay that whole image until this reserved deadline.
 * Sample a compatible clock outside this API. Zero or now >= deadline permits
 * omitting visual phase construction only: source operations, logical history,
 * current endpoint and normal ACK/fence/ownership obligations remain unchanged.
 * False means no budget available (NULL output, invalid/non-active/stale commit)
 * and leaves the output untouched. No clock calls, initialization or state writes;
 * the snapshot does not prevent subsequent epoch invalidation. */
bool xg_render_worker_source_deadline(
    XgRenderSourceCommitHandle commit, uint64_t *out_deadline_ns);
/* Matching retained logical pixel digest + epoch/scene enables approved phases.
 * Guest cycles map to a fixed-latency presenter timeline at FIFO acceptance;
 * approved images are selected monotonically, never timed from endpoint arrival.
 * An accepted temporal interval finishes its whole before a compatible temporal
 * successor or EOF (even with idle holds disabled). A discrete/incompatible
 * successor can replace it immediately with the newer complete image.
 * An early incoming endpoint returns EMPTY so the owner can advance/hold the
 * retained image. Missing temporal metadata/base selects immediate whole 1/1;
 * there is no added temporal-buffer delay for discrete images. */
XgRenderPresenterResult xg_render_presenter_present_next(
    const XgRenderPresenterServices *services);
/* Recompose/swap the retained endpoint, advancing only its approved temporal
 * phases on the presenter clock. Whole/fallback endpoints use 1/1. The same
 * owner/epoch authorization and fences as present_next apply. No source
 * publication or guest-memory capture. False if absent, pending or rejected.
 * The original endpoint reference lives until replacement/invalidation. */
bool xg_render_presenter_present_hold(
    const XgRenderPresenterServices *services);
/*
 * Runs deferred fence/endpoint/commit reclamation on the registered presenter
 * owner.  The drain is part of lifecycle quiescence; reset only marks live
 * work stale and never waits for a reclamation callback.
 */
bool xg_render_presenter_drain_retirements(
    const XgRenderPresenterServices *services);
/* Owner-only pacer correspondence: guest_cycle occurs at guest_time_ns in the
 * presenter's clock domain. Bind on a fresh epoch/rate change or explicit rebase
 * (pause/debt reset), not on endpoint arrival. Only future FIFO publications use
 * the new mapping; accepted source/batch deadlines and resources are unchanged.
 * Non-realtime/unbound sources use immediate whole images. */
bool xg_render_presenter_sync_source_clock(
    const XgRenderPresenterServices *services, uint64_t guest_cycle,
    uint64_t guest_time_ns, bool realtime, bool rebase);
bool xg_render_presentation_phase_count(
    uint32_t source_interval_vblanks,
    uint32_t refresh_numerator,
    uint32_t refresh_denominator,
    uint64_t *out_phase_count);
bool xg_render_presentation_phase_alpha(
    uint64_t phase_index,
    uint64_t phase_count,
    uint64_t *out_numerator,
    uint64_t *out_denominator);
void xg_render_semantic_presentation_diagnostics(
    XgRenderPresentationDiagnostics *out_diagnostics);
/* Trace sequences are monotonic; get returns false after ring eviction. */
uint64_t xg_render_presentation_trace_total(void);
bool xg_render_presentation_trace_get(
    uint64_t trace_sequence, XgRenderPresentationTraceEvent *out_event);

#ifdef __cplusplus
}
#endif

#endif
