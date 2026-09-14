#include "xg_render_semantic_presentation.h"
#include "xg_threads.h"

#include <stdatomic.h>
#include <string.h>

_Static_assert(XG_RENDER_SOURCE_COMMIT_CAPACITY > 0u, "FIFO must have capacity");
_Static_assert(XG_RENDER_PRESENTATION_BATCH_CAPACITY >= 2u,
               "Retaining an endpoint requires a second batch for new work");

/* Bootstrap budget until ACKed visual endpoints establish a source cadence.
 * It also bounds the intervals treated as regular updates rather than idle holds. */
#define XG_RENDER_TEMPORAL_BUDGET_NS UINT64_C(66666667)
#define XG_RENDER_GUEST_CYCLES_PER_SECOND UINT64_C(33868800)

typedef enum XgRenderPresentationBatchState {
    XG_RENDER_BATCH_FREE = 0,
    XG_RENDER_BATCH_COMPILING,
    XG_RENDER_BATCH_WAITING_COMPILE_FENCE,
    XG_RENDER_BATCH_FINALIZING_COMPILE,
    XG_RENDER_BATCH_PENDING,
    XG_RENDER_BATCH_CHECKING_COMPILE_FENCE,
    XG_RENDER_BATCH_SELECTING_PHASE,
    XG_RENDER_BATCH_COMPOSING,
    XG_RENDER_BATCH_COMPOSED,
    XG_RENDER_BATCH_SWAPPING,
    XG_RENDER_BATCH_COMPLETION_PENDING,
    XG_RENDER_BATCH_REAPING,
    XG_RENDER_BATCH_RETAINED,
} XgRenderPresentationBatchState;

typedef struct XgRenderFenceCleanup {
    uint64_t fence;
    void (*discard_fence)(uint64_t fence, void *user_data);
    void *user_data;
} XgRenderFenceCleanup;

typedef struct XgRenderEndpointCleanup {
    XgRenderCompiledEndpoint endpoint;
    void (*release_endpoint)(const XgRenderCompiledEndpoint *endpoint,
                             void *user_data);
    void *user_data;
    bool present;
} XgRenderEndpointCleanup;

typedef struct XgRenderPresentationBatch {
    XgRenderSourceCommitHandle commit;
    XgPresentationIdentity identity;
    XgRenderCompiledEndpoint endpoint;
    XgRenderFenceCleanup compile_fence;
    XgRenderFenceCleanup completion_fence;
    XgRenderEndpointCleanup endpoint_cleanup;
    XgRenderFenceStatus (*compile_fence_status)(uint64_t fence,
                                                 void *user_data);
    XgRenderFenceStatus (*completion_fence_status)(uint64_t fence,
                                                    void *user_data);
    void *compile_fence_status_user_data;
    void *completion_fence_status_user_data;
    uint64_t presentation_deadline_ns; /* Fixed at FIFO acceptance; zero is whole-only. */
    uint64_t phase_count; /* Effective denominator N+1; zero means whole-only. */
    uint64_t phase_index;
    uint32_t generation;
    XgRenderPresentationBatchState state;
    bool has_commit;
    bool has_endpoint;
    bool stale_counted;
    bool temporal_disabled;
} XgRenderPresentationBatch;

typedef struct XgRenderRetirement {
    XgRenderSourceCommitHandle commit;
    XgRenderFenceCleanup fences[2];
    XgRenderEndpointCleanup endpoint;
    uint32_t fence_count;
    bool has_commit;
} XgRenderRetirement;

typedef enum XgRenderCompletionReapResult {
    XG_RENDER_REAP_NONE = 0,
    XG_RENDER_REAP_PENDING,
    XG_RENDER_REAP_READY,
    XG_RENDER_REAP_FAILED,
} XgRenderCompletionReapResult;

#define XG_RENDER_DEFERRED_RETIREMENT_CAPACITY \
    (XG_RENDER_SOURCE_COMMIT_CAPACITY + \
     2u * XG_RENDER_PRESENTATION_BATCH_CAPACITY + 2u)

static XgRenderPresentationDiagnostics g_diagnostics;
static XgRenderPresentationTraceEvent
    g_trace[XG_RENDER_PRESENTATION_TRACE_CAPACITY];
static uint64_t g_trace_total;
static uint64_t g_source_queue[XG_RENDER_SOURCE_COMMIT_CAPACITY];
static uint64_t g_source_deadlines[XG_RENDER_SOURCE_COMMIT_CAPACITY];
static uint32_t g_source_head;
static uint32_t g_source_count;
static uint64_t g_work_batch;
static uint64_t g_retained_batch;
static uint64_t g_batch_pending;
static XgRenderPresentationBatch g_batches[XG_RENDER_PRESENTATION_BATCH_CAPACITY];
typedef struct XgRenderPhaseSelectionEvent {
    XgPresentationIdentity identity;
    uint64_t clock_ns, deadline_ns, interval_ns, phase_count, selected_phase;
    uint64_t expected_digest, previous_digest;
    uint32_t generated_phases;
} XgRenderPhaseSelectionEvent;
static volatile XgRenderPhaseSelectionEvent g_phase_selections[8192];
static volatile uint32_t g_phase_selection_count;
static volatile XgRenderPhaseSelectionEvent g_source_acceptances[8192];
static volatile uint32_t g_source_acceptance_count;
static XgRenderRetirement
    g_deferred_retirements[XG_RENDER_DEFERRED_RETIREMENT_CAPACITY];
static uint32_t g_deferred_retirement_count;
/* Includes queued and currently executing cleanup of invalidated compile work. */
static uint32_t g_deferred_compile_retirements;
static atomic_flag g_state_lock = ATOMIC_FLAG_INIT;
static uint64_t g_presenter_owner_token;
static uint64_t g_last_work_cycle;
static uint64_t (*g_phase_clock_ns)(void *);
static void *g_phase_clock_user_data;
static uint64_t g_phase_clock_last_ns;
static bool g_phase_clock_seen;
static uint64_t g_visual_origin_ns;
static uint64_t g_visual_origin_cycle;
static XgPresentationIdentity g_visual_source_identity;
static uint64_t g_visual_source_interval_ns;
static bool g_visual_origin_seen;
static bool g_visual_realtime;
static uint32_t g_worker_operations;
static bool g_presenter_busy;
static bool g_publication_open;
static bool g_epoch_terminal;
static bool g_initialized;

static void state_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_state_lock,
                                              memory_order_acquire)) {
        thrd_yield();
    }
}

static void state_unlock(void) {
    atomic_flag_clear_explicit(&g_state_lock, memory_order_release);
}

static uint64_t pack_handle(uint32_t slot, uint32_t generation) {
    return ((uint64_t)generation << 32) | ((uint64_t)slot + 1u);
}

static bool unpack_source(uint64_t packed, XgRenderSourceCommitHandle *out) {
    if (packed == 0u || out == NULL) return false;
    out->slot = (uint32_t)packed - 1u;
    out->generation = (uint32_t)(packed >> 32);
    return true;
}

static bool unpack_batch(uint64_t packed, XgRenderPresentationBatchHandle *out) {
    if (packed == 0u || out == NULL) return false;
    out->slot = (uint32_t)packed - 1u;
    out->generation = (uint32_t)(packed >> 32);
    return true;
}

static uint64_t source_head_locked(void) {
    return g_source_count != 0u ? g_source_queue[g_source_head] : 0u;
}

static void source_pop_locked(void) {
    if (g_source_count == 0u) return;
    g_source_queue[g_source_head] = 0u;
    g_source_deadlines[g_source_head] = 0u;
    g_source_head = (g_source_head + 1u) % XG_RENDER_SOURCE_COMMIT_CAPACITY;
    --g_source_count;
}

static uint32_t next_generation(uint32_t generation) {
    generation++;
    return generation == 0u ? 1u : generation;
}

static bool identity_equal(const XgPresentationIdentity *left,
                           const XgPresentationIdentity *right) {
    return left != NULL && right != NULL &&
        left->presentation_epoch == right->presentation_epoch &&
        left->source_sequence == right->source_sequence &&
        left->guest_vblank_sequence == right->guest_vblank_sequence &&
        left->guest_cycle == right->guest_cycle &&
        left->scene_generation == right->scene_generation;
}

static XgRenderPresentationReceipt receipt_from_header(
        const XgRenderSourceCommitHeader *header) {
    XgRenderPresentationReceipt receipt = {0};

    if (header == NULL) return receipt;
    receipt.identity = header->identity;
    receipt.semantic_digest = header->digest;
    receipt.width = header->display.width;
    receipt.height = header->display.height;
    receipt.valid = true;
    return receipt;
}

static bool endpoint_has_temporal_phases(
        const XgRenderCompiledEndpoint *endpoint) {
    return endpoint != NULL && endpoint->temporal_phase_count != 0u &&
        endpoint->temporal_interval_ns != 0u && endpoint->pixel_digest != 0u &&
        endpoint->temporal_previous_pixel_digest != 0u;
}

static uint64_t source_deadline_locked(const XgRenderSourceCommitHeader *header) {
    const uint64_t guest_cycle = header->identity.guest_cycle;
    uint64_t cycles;
    uint64_t seconds;
    uint64_t delta_ns;
    uint64_t budget_ns = XG_RENDER_TEMPORAL_BUDGET_NS;

    if (!g_visual_realtime || !g_visual_origin_seen ||
        header->display.temporal_hz == 0u || header->display.disabled ||
        header->display.depth24 || header->scene.module == XG_SEMANTIC_MODULE_MOVIE)
        return 0u;
    cycles = guest_cycle >= g_visual_origin_cycle
        ? guest_cycle - g_visual_origin_cycle : g_visual_origin_cycle - guest_cycle;
    seconds = cycles / XG_RENDER_GUEST_CYCLES_PER_SECOND;
    if (seconds >= UINT64_MAX / UINT64_C(1000000000)) return 0u;
    delta_ns = seconds * UINT64_C(1000000000) +
        (cycles % XG_RENDER_GUEST_CYCLES_PER_SECOND) * UINT64_C(1000000000) /
            XG_RENDER_GUEST_CYCLES_PER_SECOND;
    if (g_visual_source_interval_ns != 0u &&
        header->identity.presentation_epoch == g_visual_source_identity.presentation_epoch &&
        header->identity.scene_generation == g_visual_source_identity.scene_generation) {
        /* One source interval to bracket motion, one for construction, and one
         * presentation tick to sample a READY image. Transport chunks are not
         * source updates. Only newly accepted work uses the measured cadence. */
        budget_ns = 2u * g_visual_source_interval_ns +
            (UINT64_C(1000000000) + header->display.temporal_hz - 1u) / header->display.temporal_hz;
    }
    uint64_t source_ns;
    if (guest_cycle < g_visual_origin_cycle) {
        if (delta_ns >= g_visual_origin_ns) return 0u;
        source_ns = g_visual_origin_ns - delta_ns;
    } else {
        if (delta_ns > UINT64_MAX - g_visual_origin_ns) return 0u;
        source_ns = g_visual_origin_ns + delta_ns;
    }
    return budget_ns <= UINT64_MAX - source_ns ? source_ns + budget_ns : 0u;
}

static XgRenderPresentationReceipt receipt_from_endpoint(
        const XgRenderCompiledEndpoint *endpoint) {
    XgRenderPresentationReceipt receipt = {0};

    if (endpoint == NULL) return receipt;
    receipt.identity = endpoint->identity;
    receipt.semantic_digest = endpoint->digest;
    receipt.opaque_handle = endpoint->opaque_handle;
    receipt.backend_generation = endpoint->backend_generation;
    receipt.width = endpoint->width;
    receipt.height = endpoint->height;
    receipt.format = endpoint->format;
    receipt.pixel_digest = endpoint->pixel_digest;
    /* Optional/incomplete temporal metadata never changes the semantic digest
     * or rejects already-applied work. Receipts normalize it to whole-only. */
    if (endpoint_has_temporal_phases(endpoint)) {
        receipt.temporal_phase_count = endpoint->temporal_phase_count;
        receipt.temporal_interval_ns = endpoint->temporal_interval_ns;
        receipt.temporal_previous_pixel_digest =
            endpoint->temporal_previous_pixel_digest;
    }
    receipt.valid = true;
    return receipt;
}

static XgRenderPresentationTraceEvent *trace_find_locked(
        const XgPresentationIdentity *identity) {
    uint64_t available;
    uint64_t offset;

    if (identity == NULL || g_trace_total == 0u) return NULL;
    available = g_trace_total < XG_RENDER_PRESENTATION_TRACE_CAPACITY
        ? g_trace_total : XG_RENDER_PRESENTATION_TRACE_CAPACITY;
    for (offset = 0u; offset < available; ++offset) {
        const uint64_t sequence = g_trace_total - offset - 1u;
        XgRenderPresentationTraceEvent *event =
            &g_trace[sequence % XG_RENDER_PRESENTATION_TRACE_CAPACITY];

        if (event->trace_sequence == sequence &&
            identity_equal(&event->source.identity, identity))
            return event;
    }
    return NULL;
}

static XgRenderPresentationTraceEvent *trace_begin_locked(
        const XgRenderSourceCommitHeader *header) {
    const uint64_t sequence = g_trace_total++;
    XgRenderPresentationTraceEvent *event =
        &g_trace[sequence % XG_RENDER_PRESENTATION_TRACE_CAPACITY];

    memset(event, 0, sizeof(*event));
    event->trace_sequence = sequence;
    event->flags = XG_RENDER_PRESENTATION_TRACE_PUBLISHED;
    event->source = receipt_from_header(header);
    event->worker_result = XG_RENDER_WORKER_EMPTY;
    event->presenter_result = XG_RENDER_PRESENTER_EMPTY;
    return event;
}

static void trace_mark_identity_locked(
        const XgPresentationIdentity *identity, uint64_t flags) {
    XgRenderPresentationTraceEvent *event = trace_find_locked(identity);

    if (event != NULL) event->flags |= flags;
}

static void trace_mark_batch_locked(
        const XgRenderPresentationBatch *batch, uint64_t flags) {
    if (batch != NULL) trace_mark_identity_locked(&batch->identity, flags);
}

static void trace_set_presenter_result_locked(
        const XgRenderPresentationBatch *batch,
        XgRenderPresenterResult result) {
    XgRenderPresentationTraceEvent *event;

    if (batch == NULL) return;
    event = trace_find_locked(&batch->identity);
    if (event != NULL) event->presenter_result = result;
}

static void trace_mark_packed_source_locked(uint64_t packed, uint64_t flags) {
    XgRenderSourceCommitHandle commit;
    XgRenderSourceCommitHeader header = {0};

    if (unpack_source(packed, &commit) &&
        xg_render_source_commit_header_copy(commit, &header) ==
            XG_RENDER_SOURCE_COMMIT_OK)
        trace_mark_identity_locked(&header.identity, flags);
}

static uint32_t endpoint_mismatch_mask(
        bool callback_ok, uint64_t compile_fence,
        const XgRenderCompiledEndpoint *endpoint,
        const XgRenderSourceCommitHeader *header) {
    uint32_t mismatch = 0u;

    if (!callback_ok) return XG_RENDER_ENDPOINT_MISMATCH_CALLBACK;
    if (compile_fence == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_COMPILE_FENCE;
    if (endpoint == NULL || header == NULL)
        return mismatch | XG_RENDER_ENDPOINT_MISMATCH_HANDLE;
    if (endpoint->opaque_handle == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_HANDLE;
    if (endpoint->identity.presentation_epoch !=
            header->identity.presentation_epoch)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_PRESENTATION_EPOCH;
    if (endpoint->identity.source_sequence !=
            header->identity.source_sequence)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_SOURCE_SEQUENCE;
    if (endpoint->identity.guest_vblank_sequence !=
            header->identity.guest_vblank_sequence)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_GUEST_VBLANK;
    if (endpoint->identity.guest_cycle != header->identity.guest_cycle)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_GUEST_CYCLE;
    if (endpoint->identity.scene_generation !=
            header->identity.scene_generation)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_SCENE_GENERATION;
    if (endpoint->digest != header->digest)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_DIGEST;
    if (endpoint->width != header->display.width || endpoint->width == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_WIDTH;
    if (endpoint->height != header->display.height || endpoint->height == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_HEIGHT;
    if (endpoint->format == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_FORMAT;
    if (endpoint->backend_generation == 0u)
        mismatch |= XG_RENDER_ENDPOINT_MISMATCH_BACKEND_GENERATION;
    return mismatch;
}

static void count_endpoint_mismatches_locked(uint32_t mismatch) {
    uint32_t index;

    for (index = 0u; index < XG_RENDER_ENDPOINT_MISMATCH_COUNT; ++index)
        if ((mismatch & (UINT32_C(1) << index)) != 0u)
            g_diagnostics.endpoint_mismatches[index]++;
}

static bool fence_status_valid(XgRenderFenceStatus status) {
    return status == XG_RENDER_FENCE_PENDING ||
        status == XG_RENDER_FENCE_READY ||
        status == XG_RENDER_FENCE_FAILED;
}

static void initialize_locked(void) {
    uint32_t index;

    memset(&g_diagnostics, 0, sizeof(g_diagnostics));
    memset(g_trace, 0, sizeof(g_trace));
    g_trace_total = 0u;
    g_diagnostics.presentation_epoch = 1u;
    memset(g_source_queue, 0, sizeof(g_source_queue));
    memset(g_source_deadlines, 0, sizeof(g_source_deadlines));
    g_source_head = 0u;
    g_source_count = 0u;
    g_work_batch = 0u;
    g_retained_batch = 0u;
    g_batch_pending = 0u;
    g_deferred_retirement_count = 0u;
    g_deferred_compile_retirements = 0u;
    g_presenter_owner_token = 0u;
    g_last_work_cycle = 0u;
    g_phase_clock_ns = NULL;
    g_phase_clock_user_data = NULL;
    g_phase_clock_last_ns = 0u;
    g_phase_clock_seen = false;
    g_visual_origin_seen = false;
    memset(&g_visual_source_identity, 0, sizeof(g_visual_source_identity));
    g_visual_source_interval_ns = 0u;
    g_visual_realtime = true;
    g_worker_operations = 0u;
    g_presenter_busy = false;
    g_publication_open = false;
    g_epoch_terminal = false;
    memset(g_deferred_retirements, 0, sizeof(g_deferred_retirements));
    for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        uint32_t generation = g_batches[index].generation;
        memset(&g_batches[index], 0, sizeof(g_batches[index]));
        g_batches[index].generation = generation == 0u ? 1u : generation;
        g_batches[index].state = XG_RENDER_BATCH_FREE;
    }
    xg_render_source_commit_reset();
    g_initialized = true;
}

/* Returns with the state lock held. */
static void lock_ready(void) {
    state_lock();
    if (!g_initialized) initialize_locked();
}

static XgRenderPresentationBatch *batch_from_handle_locked(
        XgRenderPresentationBatchHandle handle) {
    XgRenderPresentationBatch *batch;
    if (handle.slot >= XG_RENDER_PRESENTATION_BATCH_CAPACITY) return NULL;
    batch = &g_batches[handle.slot];
    if (batch->state == XG_RENDER_BATCH_FREE ||
        batch->generation != handle.generation)
        return NULL;
    return batch;
}

static bool batch_is_current_locked(const XgRenderPresentationBatch *batch) {
    return batch != NULL && !g_epoch_terminal &&
        batch->identity.presentation_epoch ==
            g_diagnostics.presentation_epoch &&
        batch->identity.scene_generation == g_diagnostics.scene_generation;
}

static bool batch_has_temporal_base_locked(
        const XgRenderPresentationBatch *batch,
        const XgRenderPresentationBatch *previous) {
    return batch_is_current_locked(batch) && !batch->temporal_disabled &&
        batch->presentation_deadline_ns != 0u &&
        endpoint_has_temporal_phases(&batch->endpoint) &&
        batch_is_current_locked(previous) && previous->has_endpoint &&
        previous->endpoint.pixel_digest == batch->endpoint.temporal_previous_pixel_digest;
}

static bool core_is_quiescent_locked(void) {
    uint32_t index;
    if (g_source_count != 0u || g_batch_pending != 0u ||
        g_work_batch != 0u || g_retained_batch != 0u ||
        g_deferred_retirement_count != 0u ||
        g_deferred_compile_retirements != 0u || g_worker_operations != 0u ||
        g_presenter_busy)
        return false;
    for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        if (g_batches[index].state != XG_RENDER_BATCH_FREE) return false;
    }
    return true;
}

static uint64_t advance_epoch_locked(void) {
    if (g_diagnostics.presentation_epoch < UINT64_MAX)
        g_diagnostics.presentation_epoch++;
    if (g_diagnostics.presentation_epoch == UINT64_MAX) {
        g_epoch_terminal = true;
        g_publication_open = false;
    }
    return g_diagnostics.presentation_epoch;
}

static void refresh_batch_pending_locked(void) {
    uint64_t oldest_sequence = UINT64_MAX;
    uint32_t index;

    g_batch_pending = 0u;
    for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        const XgRenderPresentationBatch *batch = &g_batches[index];

        if (batch->state != XG_RENDER_BATCH_PENDING ||
            batch->identity.source_sequence >= oldest_sequence)
            continue;
        oldest_sequence = batch->identity.source_sequence;
        g_batch_pending = pack_handle(index, batch->generation);
    }
}

static bool presenter_acquire_locked(
        const XgRenderPresenterServices *services) {
    if (services == NULL || services->owner_token == 0u || g_presenter_busy)
        return false;
    if (g_presenter_owner_token == 0u ||
        g_presenter_owner_token != services->owner_token)
        return false;
    g_presenter_busy = true;
    return true;
}

static void presenter_release(void) {
    state_lock();
    g_presenter_busy = false;
    state_unlock();
}

static void retirement_add_fence(
        XgRenderRetirement *retirement, uint64_t fence,
        void (*discard_fence)(uint64_t fence, void *user_data),
        void *user_data) {
    uint32_t index;
    XgRenderFenceCleanup *cleanup;

    if (retirement == NULL || fence == 0u || discard_fence == NULL ||
        retirement->fence_count >= 2u)
        return;
    for (index = 0u; index < retirement->fence_count; ++index) {
        const XgRenderFenceCleanup *existing = &retirement->fences[index];
        if (existing->fence == fence &&
            existing->discard_fence == discard_fence &&
            existing->user_data == user_data)
            return;
    }
    cleanup = &retirement->fences[retirement->fence_count++];
    cleanup->fence = fence;
    cleanup->discard_fence = discard_fence;
    cleanup->user_data = user_data;
}

static void retirement_add_endpoint(
        XgRenderRetirement *retirement,
        const XgRenderCompiledEndpoint *endpoint,
        void (*release_endpoint)(const XgRenderCompiledEndpoint *, void *),
        void *user_data) {
    if (retirement == NULL || endpoint == NULL ||
        endpoint->opaque_handle == 0u || release_endpoint == NULL ||
        retirement->endpoint.present)
        return;
    retirement->endpoint.endpoint = *endpoint;
    retirement->endpoint.release_endpoint = release_endpoint;
    retirement->endpoint.user_data = user_data;
    retirement->endpoint.present = true;
}

static bool retirement_has_work(const XgRenderRetirement *retirement) {
    return retirement != NULL &&
        (retirement->has_commit || retirement->fence_count != 0u ||
         retirement->endpoint.present);
}

/* Backend callbacks are deliberately made only here, with no core lock held. */
static void run_retirement(XgRenderRetirement *retirement) {
    uint32_t index;

    if (retirement == NULL) return;
    for (index = 0u; index < retirement->fence_count; ++index) {
        XgRenderFenceCleanup *cleanup = &retirement->fences[index];
        cleanup->discard_fence(cleanup->fence, cleanup->user_data);
    }
    if (retirement->endpoint.present) {
        retirement->endpoint.release_endpoint(
            &retirement->endpoint.endpoint,
            retirement->endpoint.user_data);
    }
    if (retirement->has_commit)
        (void)xg_render_source_commit_retire(retirement->commit);
    memset(retirement, 0, sizeof(*retirement));
}

static bool defer_retirement_locked(XgRenderRetirement *retirement) {
    if (!retirement_has_work(retirement)) return true;
    if (g_deferred_retirement_count >=
            XG_RENDER_DEFERRED_RETIREMENT_CAPACITY) {
        g_diagnostics.retirement_capacity_failures++;
        return false;
    }
    g_deferred_retirements[g_deferred_retirement_count++] = *retirement;
    if (retirement->has_commit && retirement->fence_count != 0u)
        ++g_deferred_compile_retirements;
    memset(retirement, 0, sizeof(*retirement));
    return true;
}

static uint32_t drain_deferred_retirements(void) {
    XgRenderRetirement retirement;
    uint32_t count = 0u;

    for (;;) {
        bool compile_retirement;
        state_lock();
        if (g_deferred_retirement_count == 0u) {
            state_unlock();
            return count;
        }
        retirement = g_deferred_retirements[0];
        compile_retirement = retirement.has_commit && retirement.fence_count != 0u;
        g_deferred_retirement_count--;
        if (g_deferred_retirement_count != 0u) {
            memmove(&g_deferred_retirements[0],
                    &g_deferred_retirements[1],
                    sizeof(g_deferred_retirements[0]) *
                        g_deferred_retirement_count);
        }
        memset(&g_deferred_retirements[g_deferred_retirement_count], 0,
               sizeof(g_deferred_retirements[0]));
        state_unlock();
        run_retirement(&retirement);
        if (compile_retirement) {
            state_lock();
            --g_deferred_compile_retirements;
            state_unlock();
        }
        count++;
    }
}

static void set_source_retirement(uint64_t packed,
                                  XgRenderRetirement *retirement) {
    XgRenderSourceCommitHandle commit;
    if (retirement != NULL && unpack_source(packed, &commit)) {
        retirement->commit = commit;
        retirement->has_commit = true;
    }
}

static void count_batch_stale_locked(XgRenderPresentationBatch *batch) {
    if (batch == NULL || batch->stale_counted) return;
    g_diagnostics.stale_batches++;
    trace_mark_batch_locked(batch, XG_RENDER_PRESENTATION_TRACE_STALE);
    batch->stale_counted = true;
}

static void count_batch_superseded_locked(void) {
    g_diagnostics.superseded_endpoints++;
    g_diagnostics.batch_coalesces++;
    g_diagnostics.batch_capacity_drops++;
    g_diagnostics.dropped_phases++;
}

static void detach_batch_locked(XgRenderPresentationBatch *batch,
                                XgRenderRetirement *retirement) {
    uint32_t generation;

    if (batch == NULL || retirement == NULL ||
        batch->state == XG_RENDER_BATCH_FREE)
        return;
    if (batch->compile_fence.fence != 0u) {
        retirement_add_fence(retirement, batch->compile_fence.fence,
                             batch->compile_fence.discard_fence,
                             batch->compile_fence.user_data);
    }
    if (batch->completion_fence.fence != 0u) {
        retirement_add_fence(retirement, batch->completion_fence.fence,
                             batch->completion_fence.discard_fence,
                             batch->completion_fence.user_data);
    }
    if (batch->has_endpoint) {
        retirement_add_endpoint(retirement, &batch->endpoint,
                                batch->endpoint_cleanup.release_endpoint,
                                batch->endpoint_cleanup.user_data);
    }
    if (batch->has_commit) {
        retirement->commit = batch->commit;
        retirement->has_commit = true;
    }
    {
        const uint64_t packed = pack_handle(
            (uint32_t)(batch - g_batches), batch->generation);
        if (g_retained_batch == packed) g_retained_batch = 0u;
        if (g_work_batch == packed) g_work_batch = 0u;
    }
    generation = next_generation(batch->generation);
    memset(batch, 0, sizeof(*batch));
    batch->generation = generation;
    batch->state = XG_RENDER_BATCH_FREE;
}

/* Compile work and SourceCommit ownership end independently of the endpoint. */
static void extract_compile_retirement_locked(
        XgRenderPresentationBatch *batch, XgRenderRetirement *retirement) {
    if (batch == NULL || retirement == NULL) return;
    if (batch->compile_fence.fence != 0u) {
        retirement_add_fence(retirement, batch->compile_fence.fence,
                             batch->compile_fence.discard_fence,
                             batch->compile_fence.user_data);
        memset(&batch->compile_fence, 0, sizeof(batch->compile_fence));
        batch->compile_fence_status = NULL;
        batch->compile_fence_status_user_data = NULL;
    }
    if (batch->has_commit) {
        retirement->commit = batch->commit;
        retirement->has_commit = true;
        memset(&batch->commit, 0, sizeof(batch->commit));
        batch->has_commit = false;
    }
}

static bool detach_batch_to_deferred_locked(XgRenderPresentationBatch *batch) {
    XgRenderRetirement retirement = {0};
    if (batch == NULL || batch->state == XG_RENDER_BATCH_FREE) return true;
    if (g_deferred_retirement_count >=
            XG_RENDER_DEFERRED_RETIREMENT_CAPACITY) {
        g_diagnostics.retirement_capacity_failures++;
        return false;
    }
    detach_batch_locked(batch, &retirement);
    return defer_retirement_locked(&retirement);
}

static void logical_invalidate_locked(XgRenderTimelineInvalidationReason reason,
                                      bool reset_diagnostics) {
    XgRenderPresentationBatchHandle pending_handle;
    XgRenderPresentationBatchHandle work_handle;
    uint64_t work_source = 0u;
    uint64_t epoch = g_diagnostics.presentation_epoch;
    uint32_t index;

    if (reset_diagnostics) {
        memset(&g_diagnostics, 0, sizeof(g_diagnostics));
        g_diagnostics.presentation_epoch = epoch;
        memset(g_trace, 0, sizeof(g_trace));
        g_trace_total = 0u;
    }
    epoch = advance_epoch_locked();
    g_diagnostics.invalidation_count++;
    g_diagnostics.source_lane_blocked = false;
    memset(&g_diagnostics.blocked_source, 0, sizeof(g_diagnostics.blocked_source));
    g_retained_batch = 0u;

    if (unpack_batch(g_work_batch, &work_handle)) {
        const XgRenderPresentationBatch *work =
            batch_from_handle_locked(work_handle);
        if (work != NULL && work->has_commit)
            work_source = pack_handle(work->commit.slot, work->commit.generation);
    }
    while (g_source_count != 0u) {
        XgRenderRetirement source_retirement = {0};
        const uint64_t packed = source_head_locked();
        trace_mark_packed_source_locked(
            packed, XG_RENDER_PRESENTATION_TRACE_INVALIDATED |
                XG_RENDER_PRESENTATION_TRACE_STALE);
        /* The active worker/fence, not the FIFO, protects this input now. */
        if (packed != work_source) {
            set_source_retirement(packed, &source_retirement);
            if (!defer_retirement_locked(&source_retirement)) break;
        }
        source_pop_locked();
        g_diagnostics.invalidated_source_work++;
        g_diagnostics.stale_commits++;
    }

    for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        XgRenderPresentationBatch *batch = &g_batches[index];
        if (batch->state == XG_RENDER_BATCH_FREE) continue;
        {
            XgRenderPresentationTraceEvent *trace =
                trace_find_locked(&batch->identity);

            if (trace != NULL) {
                trace->flags |= XG_RENDER_PRESENTATION_TRACE_INVALIDATED |
                    XG_RENDER_PRESENTATION_TRACE_STALE;
                if (batch->has_endpoint &&
                    (trace->flags &
                        XG_RENDER_PRESENTATION_TRACE_SWAP_CALLBACK_RETURNED) ==
                            0u)
                    trace->flags |=
                        XG_RENDER_PRESENTATION_TRACE_RETIRED_BEFORE_SWAP;
            }
        }
        if (batch->state != XG_RENDER_BATCH_SWAPPING)
            count_batch_stale_locked(batch);
        if (batch->state == XG_RENDER_BATCH_PENDING ||
            batch->state == XG_RENDER_BATCH_COMPLETION_PENDING ||
            batch->state == XG_RENDER_BATCH_RETAINED ||
            batch->state == XG_RENDER_BATCH_WAITING_COMPILE_FENCE) {
            (void)detach_batch_to_deferred_locked(batch);
        }
    }
    if (unpack_batch(g_batch_pending, &pending_handle) &&
        batch_from_handle_locked(pending_handle) == NULL)
        g_batch_pending = 0u;

    g_diagnostics.presentation_epoch = epoch;
    if ((unsigned int)reason <
            (unsigned int)XG_RENDER_TIMELINE_INVALIDATION_REASON_COUNT) {
        g_diagnostics.invalidations_by_reason[reason]++;
        g_diagnostics.last_invalidation_reason = reason;
    }
    g_diagnostics.source_sequence = 0u;
    g_diagnostics.guest_vblank_sequence = 0u;
    g_diagnostics.guest_cycle = 0u;
    g_diagnostics.scene_generation = 0u;
    g_last_work_cycle = 0u;
    g_phase_clock_ns = NULL;
    g_phase_clock_user_data = NULL;
    g_phase_clock_last_ns = 0u;
    g_phase_clock_seen = false;
    g_visual_origin_seen = false;
    xg_render_source_commit_cancel_builders();
}

void xg_render_semantic_presentation_reset(void) {
    state_lock();
    if (!g_initialized) {
        initialize_locked();
        state_unlock();
        return;
    }
    g_publication_open = false;
    logical_invalidate_locked(XG_RENDER_TIMELINE_RESET, true);
    state_unlock();
}

XgRenderPresentationLifecycleResult
xg_render_presentation_lifecycle_claim_open(uint64_t owner_token) {
    XgRenderPresentationLifecycleResult result;

    lock_ready();
    if (owner_token == 0u) {
        result = XG_RENDER_PRESENTATION_LIFECYCLE_INVALID_ARGUMENT;
    } else if (g_epoch_terminal) {
        result = XG_RENDER_PRESENTATION_LIFECYCLE_EPOCH_EXHAUSTED;
    } else if (g_publication_open) {
        result = g_presenter_owner_token == owner_token
            ? XG_RENDER_PRESENTATION_LIFECYCLE_ALREADY_OPEN
            : XG_RENDER_PRESENTATION_LIFECYCLE_OWNER_REJECTED;
    } else if (!core_is_quiescent_locked()) {
        result = XG_RENDER_PRESENTATION_LIFECYCLE_NOT_QUIESCENT;
    } else {
        g_presenter_owner_token = owner_token;
        g_publication_open = true;
        g_visual_realtime = true;
        g_diagnostics.lifecycle_open_count++;
        result = XG_RENDER_PRESENTATION_LIFECYCLE_OK;
    }
    if (result != XG_RENDER_PRESENTATION_LIFECYCLE_OK)
        g_diagnostics.lifecycle_rejections++;
    state_unlock();
    return result;
}

XgRenderPresentationLifecycleResult
xg_render_presentation_lifecycle_close(
        uint64_t owner_token, XgRenderTimelineInvalidationReason reason,
        uint64_t *out_epoch) {
    XgRenderPresentationLifecycleResult result;

    lock_ready();
    if (out_epoch != NULL)
        *out_epoch = g_diagnostics.presentation_epoch;
    if (owner_token == 0u ||
        (unsigned int)reason >=
            (unsigned int)XG_RENDER_TIMELINE_INVALIDATION_REASON_COUNT) {
        result = XG_RENDER_PRESENTATION_LIFECYCLE_INVALID_ARGUMENT;
    } else if (g_presenter_owner_token != owner_token) {
        result = XG_RENDER_PRESENTATION_LIFECYCLE_OWNER_REJECTED;
    } else if (!g_publication_open) {
        result = XG_RENDER_PRESENTATION_LIFECYCLE_ALREADY_CLOSED;
    } else {
        /* Publication closure and epoch invalidation share one lock point. */
        g_publication_open = false;
        logical_invalidate_locked(reason, false);
        g_diagnostics.lifecycle_close_count++;
        if (out_epoch != NULL)
            *out_epoch = g_diagnostics.presentation_epoch;
        result = g_epoch_terminal
            ? XG_RENDER_PRESENTATION_LIFECYCLE_EPOCH_EXHAUSTED
            : XG_RENDER_PRESENTATION_LIFECYCLE_OK;
    }
    if (result != XG_RENDER_PRESENTATION_LIFECYCLE_OK &&
        result != XG_RENDER_PRESENTATION_LIFECYCLE_EPOCH_EXHAUSTED)
        g_diagnostics.lifecycle_rejections++;
    state_unlock();
    return result;
}

XgRenderTimelineResult xg_render_timeline_source_boundary(
        uint64_t guest_vblank_sequence, uint64_t guest_cycle) {
    XgRenderTimelineResult result = XG_RENDER_TIMELINE_OK;

    lock_ready();
    if (g_epoch_terminal) {
        result = XG_RENDER_TIMELINE_EPOCH_EXHAUSTED;
    } else if (!g_publication_open) {
        result = XG_RENDER_TIMELINE_PUBLICATION_CLOSED;
    } else if (guest_vblank_sequence == 0u) {
        result = XG_RENDER_TIMELINE_INVALID_ARGUMENT;
    } else if (g_diagnostics.guest_vblank_sequence != 0u &&
               guest_vblank_sequence <= g_diagnostics.guest_vblank_sequence) {
        g_diagnostics.duplicate_boundaries++;
        result = XG_RENDER_TIMELINE_DUPLICATE_BOUNDARY;
    } else {
        if (g_diagnostics.guest_vblank_sequence != 0u &&
            guest_vblank_sequence !=
                g_diagnostics.guest_vblank_sequence + 1u) {
            g_diagnostics.missed_boundaries +=
                guest_vblank_sequence -
                g_diagnostics.guest_vblank_sequence - 1u;
        }
        g_diagnostics.guest_vblank_sequence = guest_vblank_sequence;
        g_diagnostics.guest_cycle = guest_cycle;
        g_diagnostics.boundary_count++;
    }
    state_unlock();
    return result;
}

uint64_t xg_render_timeline_invalidate(XgRenderTimelineInvalidationReason reason) {
    uint64_t epoch;
    lock_ready();
    logical_invalidate_locked(reason, false);
    epoch = g_diagnostics.presentation_epoch;
    state_unlock();
    return epoch;
}

static XgRenderTimelineResult timeline_next_identity(
        uint32_t scene_generation, uint64_t guest_cycle, bool at_boundary,
        XgPresentationIdentity *out_identity) {
    XgRenderTimelineResult result = XG_RENDER_TIMELINE_OK;

    lock_ready();
    if (at_boundary) guest_cycle = g_diagnostics.guest_cycle;
    if (g_epoch_terminal) {
        result = XG_RENDER_TIMELINE_EPOCH_EXHAUSTED;
    } else if (!g_publication_open) {
        result = XG_RENDER_TIMELINE_PUBLICATION_CLOSED;
    } else if (g_diagnostics.source_lane_blocked) {
        result = XG_RENDER_TIMELINE_LANE_BLOCKED;
    } else if (g_diagnostics.source_sequence == UINT64_MAX) {
        result = XG_RENDER_TIMELINE_SEQUENCE_EXHAUSTED;
    } else if (out_identity == NULL || scene_generation == 0u ||
        (at_boundary && g_diagnostics.guest_vblank_sequence == 0u) ||
        guest_cycle < g_last_work_cycle ||
        guest_cycle < g_diagnostics.guest_cycle) {
        result = XG_RENDER_TIMELINE_INVALID_ARGUMENT;
    } else if (g_diagnostics.scene_generation != 0u &&
               scene_generation != g_diagnostics.scene_generation) {
        result = XG_RENDER_TIMELINE_SCENE_CHANGE_REQUIRES_INVALIDATION;
    } else {
        *out_identity = (XgPresentationIdentity){
            .presentation_epoch = g_diagnostics.presentation_epoch,
            .source_sequence = g_diagnostics.source_sequence + 1u,
            .guest_vblank_sequence = g_diagnostics.guest_vblank_sequence,
            .guest_cycle = guest_cycle,
            .scene_generation = scene_generation,
        };
    }
    state_unlock();
    return result;
}

XgRenderTimelineResult xg_render_timeline_next_identity(
        uint32_t scene_generation, XgPresentationIdentity *out_identity) {
    return timeline_next_identity(scene_generation, 0u, true, out_identity);
}

XgRenderTimelineResult xg_render_timeline_next_work_identity(
        uint32_t scene_generation, uint64_t guest_cycle,
        XgPresentationIdentity *out_identity) {
    return timeline_next_identity(scene_generation, guest_cycle, false,
                                  out_identity);
}

void xg_render_timeline_note_hold(void) {
    lock_ready();
    g_diagnostics.presentation_holds++;
    state_unlock();
}

void xg_render_timeline_note_rejection(void) {
    lock_ready();
    g_diagnostics.rejected_commits++;
    state_unlock();
}

bool xg_render_source_queue_has_capacity(void) {
    bool available;
    lock_ready();
    available = g_publication_open && !g_epoch_terminal &&
        !g_diagnostics.source_lane_blocked &&
        g_diagnostics.source_sequence != UINT64_MAX &&
        g_source_count < XG_RENDER_SOURCE_COMMIT_CAPACITY;
    state_unlock();
    return available;
}

XgRenderTimelineResult xg_render_source_queue_publish(
        XgRenderSourceCommitHandle commit) {
    XgRenderSourceCommitHeader header;
    XgRenderTimelineResult result = XG_RENDER_TIMELINE_OK;

    lock_ready();
    if (g_epoch_terminal) {
        result = XG_RENDER_TIMELINE_EPOCH_EXHAUSTED;
    } else if (!g_publication_open) {
        result = XG_RENDER_TIMELINE_PUBLICATION_CLOSED;
    } else if (g_diagnostics.source_lane_blocked) {
        result = XG_RENDER_TIMELINE_LANE_BLOCKED;
    } else if (g_source_count >= XG_RENDER_SOURCE_COMMIT_CAPACITY) {
        g_diagnostics.source_queue_would_block++;
        result = XG_RENDER_TIMELINE_WOULD_BLOCK;
    } else if (xg_render_source_commit_header_copy(commit, &header) !=
            XG_RENDER_SOURCE_COMMIT_OK) {
        result = XG_RENDER_TIMELINE_INVALID_ARGUMENT;
    } else if (header.identity.presentation_epoch !=
                   g_diagnostics.presentation_epoch ||
               header.identity.guest_vblank_sequence >
                    g_diagnostics.guest_vblank_sequence ||
               header.identity.guest_cycle < g_last_work_cycle) {
        g_diagnostics.stale_commits++;
        result = XG_RENDER_TIMELINE_STALE_COMMIT;
    } else if (g_diagnostics.scene_generation != 0u &&
               header.identity.scene_generation !=
                   g_diagnostics.scene_generation) {
        result = XG_RENDER_TIMELINE_SCENE_CHANGE_REQUIRES_INVALIDATION;
    } else if (g_diagnostics.source_sequence == UINT64_MAX) {
        result = XG_RENDER_TIMELINE_SEQUENCE_EXHAUSTED;
    } else if (header.identity.source_sequence !=
               g_diagnostics.source_sequence + 1u) {
        result = XG_RENDER_TIMELINE_SEQUENCE_MISMATCH;
    } else if (xg_render_source_commit_mark_queued(commit) !=
               XG_RENDER_SOURCE_COMMIT_OK) {
        result = XG_RENDER_TIMELINE_QUEUE_FAILED;
    } else {
        const uint32_t tail = (g_source_head + g_source_count) %
            XG_RENDER_SOURCE_COMMIT_CAPACITY;
        g_source_queue[tail] = pack_handle(commit.slot, commit.generation);
        /* A later guest pause/debt reset must not postpone accepted work,
         * including inputs still waiting for a free compiler batch. */
        g_source_deadlines[tail] = source_deadline_locked(&header);
        if (g_source_acceptance_count < 8192u)
            g_source_acceptances[g_source_acceptance_count++] = (XgRenderPhaseSelectionEvent){
                .identity = header.identity, .clock_ns = g_phase_clock_last_ns,
                .deadline_ns = g_source_deadlines[tail], .interval_ns = g_visual_source_interval_ns};
        ++g_source_count;
        g_diagnostics.source_sequence = header.identity.source_sequence;
        g_diagnostics.scene_generation = header.identity.scene_generation;
        g_last_work_cycle = header.identity.guest_cycle;
        g_diagnostics.published_commits++;
        g_diagnostics.last_published_source = receipt_from_header(&header);
        (void)trace_begin_locked(&header);
    }
    state_unlock();

    return result;
}

bool xg_render_worker_source_deadline(
        XgRenderSourceCommitHandle commit, uint64_t *out_deadline_ns) {
    XgRenderPresentationBatchHandle handle;
    const XgRenderPresentationBatch *batch = NULL;
    bool available = false;

    if (out_deadline_ns == NULL || commit.slot >= XG_RENDER_SOURCE_COMMIT_CAPACITY ||
        commit.generation == 0u)
        return false;
    state_lock();
    if (g_initialized && unpack_batch(g_work_batch, &handle))
        batch = batch_from_handle_locked(handle);
    if (batch_is_current_locked(batch) &&
        batch->state == XG_RENDER_BATCH_COMPILING && batch->has_commit &&
        batch->commit.slot == commit.slot &&
        batch->commit.generation == commit.generation &&
        source_head_locked() == pack_handle(commit.slot, commit.generation)) {
        *out_deadline_ns = batch->presentation_deadline_ns;
        available = true;
    }
    state_unlock();
    return available;
}

XgRenderWorkerResult xg_render_worker_compile_next(
        const XgRenderWorkerServices *services) {
    XgRenderSourceCommitHandle commit;
    XgRenderSourceCommitHeader header = {0};
    XgRenderCompiledEndpoint endpoint = {0};
    XgRenderPresentationBatch *batch = NULL;
    XgRenderPresentationBatchHandle handle;
    XgRenderRetirement retirement = {0};
    XgRenderWorkerResult result;
    XgRenderCompileResult compile_result = XG_RENDER_COMPILE_ENDPOINT_READY;
    XgRenderFenceStatus status = XG_RENDER_FENCE_PENDING;
    XgRenderFenceStatus (*fence_status)(uint64_t, void *);
    void *fence_user_data;
    uint64_t packed;
    uint64_t compile_fence = 0u;
    uint32_t index;
    uint32_t generation;
    bool polling;
    bool failed = false;
    uint32_t mismatch = 0u;

    if (services == NULL || services->compile == NULL ||
        services->fence_status == NULL ||
        services->discard_fence == NULL ||
        services->release_endpoint == NULL)
        return XG_RENDER_WORKER_COMPILE_FAILED;

    lock_ready();
    if (g_worker_operations != 0u) {
        g_diagnostics.worker_serialization_rejections++;
        state_unlock();
        return XG_RENDER_WORKER_WOULD_BLOCK;
    }
    /* Invalidation can move a pending compile fence out of g_work_batch.
     * Its effects must finish/cancel before any new epoch starts compiling,
     * including while the presenter has popped its retirement for cleanup. */
    if (g_deferred_compile_retirements != 0u) {
        g_diagnostics.worker_backpressure++;
        state_unlock();
        return XG_RENDER_WORKER_WOULD_BLOCK;
    }
    if (g_diagnostics.source_lane_blocked) {
        state_unlock();
        return XG_RENDER_WORKER_LANE_BLOCKED;
    }
    if (unpack_batch(g_work_batch, &handle))
        batch = batch_from_handle_locked(handle);
    packed = batch != NULL
        ? pack_handle(batch->commit.slot, batch->commit.generation)
        : source_head_locked();
    if (!unpack_source(packed, &commit)) {
        state_unlock();
        return XG_RENDER_WORKER_EMPTY;
    }

    if (xg_render_source_commit_header_copy(commit, &header) !=
            XG_RENDER_SOURCE_COMMIT_OK) {
        g_diagnostics.source_lane_blocked = true;
        g_diagnostics.compile_failures++;
        g_diagnostics.last_compile_result = XG_RENDER_COMPILE_FAILED;
        state_unlock();
        return XG_RENDER_WORKER_COMPILE_FAILED;
    }
    if (g_epoch_terminal ||
        header.identity.presentation_epoch !=
            g_diagnostics.presentation_epoch ||
        header.identity.scene_generation != g_diagnostics.scene_generation) {
        g_worker_operations = 1u;
        if (batch != NULL) {
            count_batch_stale_locked(batch);
            detach_batch_locked(batch, &retirement);
        } else {
            source_pop_locked();
            retirement.commit = commit;
            retirement.has_commit = true;
        }
        g_diagnostics.stale_commits++;
        trace_mark_identity_locked(
            &header.identity, XG_RENDER_PRESENTATION_TRACE_STALE);
        result = XG_RENDER_WORKER_STALE;
        goto finished;
    }

    polling = batch != NULL;
    if (!polling) {
        /* Reserve space for invalidation of every live source and batch. */
        if (g_deferred_retirement_count < XG_RENDER_PRESENTATION_BATCH_CAPACITY) {
            for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY;
                 ++index) {
                if (g_batches[index].state == XG_RENDER_BATCH_FREE) {
                    batch = &g_batches[index];
                    break;
                }
            }
        }
        if (batch == NULL) {
            g_diagnostics.worker_backpressure++;
            state_unlock();
            return XG_RENDER_WORKER_CAPACITY_EXCEEDED;
        }
        generation = batch->generation;
        memset(batch, 0, sizeof(*batch));
        batch->generation = generation;
        batch->state = XG_RENDER_BATCH_COMPILING;
        batch->commit = commit;
        batch->has_commit = true;
        batch->identity = header.identity;
        batch->presentation_deadline_ns = g_source_deadlines[g_source_head];
        g_work_batch = pack_handle(index, generation);
        trace_mark_batch_locked(
            batch, XG_RENDER_PRESENTATION_TRACE_COMPILE_STARTED);
    } else {
        batch->state = XG_RENDER_BATCH_CHECKING_COMPILE_FENCE;
    }
    g_worker_operations = 1u;
    fence_status = batch->compile_fence_status;
    fence_user_data = batch->compile_fence_status_user_data;
    compile_fence = batch->compile_fence.fence;
    state_unlock();

    if (!polling) {
        compile_result = services->compile(commit, &endpoint, &compile_fence,
                                           services->user_data);
        if (compile_result == XG_RENDER_COMPILE_ENDPOINT_READY) {
            mismatch = endpoint_mismatch_mask(
                true, compile_fence, &endpoint, &header);
        } else if ((compile_result != XG_RENDER_COMPILE_APPLIED &&
                    compile_result != XG_RENDER_COMPILE_WOULD_BLOCK) ||
                   endpoint.opaque_handle != 0u || compile_fence != 0u) {
            mismatch = XG_RENDER_ENDPOINT_MISMATCH_CALLBACK;
        }
        failed = mismatch != 0u;
        state_lock();
        batch->endpoint = endpoint;
        batch->has_endpoint = endpoint.opaque_handle != 0u;
        batch->endpoint_cleanup.endpoint = endpoint;
        batch->endpoint_cleanup.release_endpoint = services->release_endpoint;
        batch->endpoint_cleanup.user_data = services->user_data;
        batch->endpoint_cleanup.present = batch->has_endpoint;
        batch->compile_fence.fence = compile_fence;
        batch->compile_fence.discard_fence = services->discard_fence;
        batch->compile_fence.user_data = services->user_data;
        batch->compile_fence_status = services->fence_status;
        batch->compile_fence_status_user_data = services->user_data;
        g_diagnostics.worker_compile_attempts++;
        g_diagnostics.last_compile_result = compile_result;
        g_diagnostics.last_compile_source = receipt_from_header(&header);
        g_diagnostics.last_compile_endpoint = receipt_from_endpoint(&endpoint);
        g_diagnostics.last_compile_endpoint.valid =
            !failed && compile_result == XG_RENDER_COMPILE_ENDPOINT_READY;
        g_diagnostics.last_endpoint_mismatch_mask = mismatch;
        if (failed) {
            g_diagnostics.endpoint_validation_failures++;
            count_endpoint_mismatches_locked(mismatch);
        } else if (compile_result == XG_RENDER_COMPILE_ENDPOINT_READY) {
            g_diagnostics.validated_endpoints++;
        }
        {
            XgRenderPresentationTraceEvent *trace =
                trace_find_locked(&header.identity);
            if (trace != NULL) {
                trace->endpoint = g_diagnostics.last_compile_endpoint;
                trace->compile_fence = compile_fence;
                trace->endpoint_mismatch_mask = mismatch;
                if (failed)
                    trace->flags |= XG_RENDER_PRESENTATION_TRACE_COMPILE_FAILED;
                else if (compile_result != XG_RENDER_COMPILE_WOULD_BLOCK)
                    trace->flags |=
                        XG_RENDER_PRESENTATION_TRACE_COMPILE_CALLBACK_OK;
                if (trace->endpoint.valid)
                    trace->flags |=
                        XG_RENDER_PRESENTATION_TRACE_ENDPOINT_VALIDATED;
            }
        }
        if (failed || compile_result != XG_RENDER_COMPILE_ENDPOINT_READY ||
            !batch_is_current_locked(batch))
            goto outcome;
        batch->state = XG_RENDER_BATCH_CHECKING_COMPILE_FENCE;
        fence_status = services->fence_status;
        fence_user_data = services->user_data;
        state_unlock();
    }

    status = fence_status(compile_fence, fence_user_data);
    if (!fence_status_valid(status)) status = XG_RENDER_FENCE_FAILED;
    failed = status == XG_RENDER_FENCE_FAILED;
    state_lock();

outcome:
    if (!batch_is_current_locked(batch) || source_head_locked() != packed) {
        count_batch_stale_locked(batch);
        detach_batch_locked(batch, &retirement);
        result = XG_RENDER_WORKER_STALE;
        goto finished;
    }
    if (failed) {
        g_diagnostics.source_lane_blocked = true;
        g_diagnostics.blocked_source = receipt_from_header(&header);
        g_diagnostics.compile_failures++;
        trace_mark_batch_locked(
            batch, XG_RENDER_PRESENTATION_TRACE_COMPILE_FAILED);
        if (status == XG_RENDER_FENCE_FAILED) {
            g_diagnostics.fence_failures++;
            trace_mark_batch_locked(
                batch, XG_RENDER_PRESENTATION_TRACE_COMPILE_FENCE_FAILED);
        }
        /* Invalidation must not free this head during output/fence cleanup. */
        retirement_add_fence(&retirement, batch->compile_fence.fence,
            batch->compile_fence.discard_fence, batch->compile_fence.user_data);
        retirement_add_endpoint(&retirement, &batch->endpoint,
            batch->endpoint_cleanup.release_endpoint,
            batch->endpoint_cleanup.user_data);
        memset(&batch->compile_fence, 0, sizeof(batch->compile_fence));
        batch->has_endpoint = false;
        batch->state = XG_RENDER_BATCH_FINALIZING_COMPILE;
        state_unlock();
        run_retirement(&retirement);
        state_lock();
        if (batch_is_current_locked(batch) && source_head_locked() == packed) {
            batch->has_commit = false; /* Return ownership to the blocked FIFO. */
            result = XG_RENDER_WORKER_COMPILE_FAILED;
        } else {
            count_batch_stale_locked(batch);
            result = XG_RENDER_WORKER_STALE;
        }
        detach_batch_locked(batch, &retirement);
        goto finished;
    }
    if (compile_result == XG_RENDER_COMPILE_WOULD_BLOCK) {
        g_diagnostics.compile_would_block++;
        trace_mark_batch_locked(
            batch, XG_RENDER_PRESENTATION_TRACE_WORK_WOULD_BLOCK);
        batch->has_commit = false; /* No effects: keep the same FIFO head. */
        detach_batch_locked(batch, &retirement);
        result = XG_RENDER_WORKER_WOULD_BLOCK;
        goto finished;
    }
    if (compile_result == XG_RENDER_COMPILE_ENDPOINT_READY &&
        status == XG_RENDER_FENCE_PENDING) {
        trace_mark_batch_locked(
            batch, XG_RENDER_PRESENTATION_TRACE_COMPILE_FENCE_PENDING);
        batch->state = XG_RENDER_BATCH_WAITING_COMPILE_FENCE;
        result = XG_RENDER_WORKER_WOULD_BLOCK;
        goto finished;
    }

    source_pop_locked();
    g_diagnostics.source_work_acks++;
    g_diagnostics.last_acked_source = receipt_from_header(&header);
    trace_mark_batch_locked(batch, XG_RENDER_PRESENTATION_TRACE_WORK_ACKED);
    if (compile_result == XG_RENDER_COMPILE_APPLIED) {
        g_diagnostics.applied_source_work++;
        trace_mark_batch_locked(batch, XG_RENDER_PRESENTATION_TRACE_WORK_APPLIED);
        detach_batch_locked(batch, &retirement);
        result = XG_RENDER_WORKER_APPLIED;
        goto finished;
    }

    if (header.native_work && header.display_boundary) {
        const XgPresentationIdentity *identity = &batch->endpoint.identity;
        if (identity->presentation_epoch == g_visual_source_identity.presentation_epoch &&
            identity->scene_generation == g_visual_source_identity.scene_generation &&
            identity->guest_cycle > g_visual_source_identity.guest_cycle) {
            const uint64_t cycles = identity->guest_cycle - g_visual_source_identity.guest_cycle;
            /* Long idle holds do not establish a slow simulation cadence or
             * enlarge the queue budget. Observe ordinary source updates only. */
            if (cycles <= XG_RENDER_TEMPORAL_BUDGET_NS * XG_RENDER_GUEST_CYCLES_PER_SECOND /
                    UINT64_C(1000000000))
                g_visual_source_interval_ns = cycles * UINT64_C(1000000000) /
                    XG_RENDER_GUEST_CYCLES_PER_SECOND;
        } else g_visual_source_interval_ns = 0u;
        g_visual_source_identity = *identity;
    }

    extract_compile_retirement_locked(batch, &retirement);
    batch->state = XG_RENDER_BATCH_FINALIZING_COMPILE;
    state_unlock();
    run_retirement(&retirement);
    state_lock();
    if (!batch_is_current_locked(batch)) {
        count_batch_stale_locked(batch);
        detach_batch_locked(batch, &retirement);
        result = XG_RENDER_WORKER_STALE;
    } else {
        const bool supersedes = !endpoint_has_temporal_phases(&batch->endpoint) ||
            batch->presentation_deadline_ns <= g_phase_clock_last_ns;
        /* Publish only after source/fence cleanup; no delta depends on this
         * batch anymore. Composing/swapping/retained endpoints are untouched. */
        batch->state = XG_RENDER_BATCH_PENDING;
        g_work_batch = 0u;
        for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
            XgRenderPresentationBatch *older = &g_batches[index];
            /* Temporal candidates wait for their source timeline. A discrete
             * complete image has no interpolation window to buffer; all its
             * source mutations have already been ACKed before supersession. */
            if (!supersedes || older == batch ||
                older->state != XG_RENDER_BATCH_PENDING ||
                older->identity.source_sequence >=
                    batch->identity.source_sequence ||
                g_deferred_retirement_count >=
                    XG_RENDER_PRESENTATION_BATCH_CAPACITY)
                continue;
            trace_mark_batch_locked(older,
                XG_RENDER_PRESENTATION_TRACE_BATCH_COALESCED |
                XG_RENDER_PRESENTATION_TRACE_RETIRED_BEFORE_SWAP);
            if (detach_batch_to_deferred_locked(older))
                count_batch_superseded_locked();
        }
        refresh_batch_pending_locked();
        result = XG_RENDER_WORKER_OK;
    }

finished:
    {
        XgRenderPresentationTraceEvent *trace =
            trace_find_locked(&header.identity);
        if (trace != NULL) trace->worker_result = result;
    }
    state_unlock();

    run_retirement(&retirement);
    state_lock();
    g_worker_operations = 0u;
    state_unlock();
    return result;
}

static XgRenderCompletionReapResult reap_one_completion(void) {
    XgRenderPresentationBatch *batch = NULL;
    XgRenderRetirement retirement = {0};
    XgRenderFenceStatus status;
    XgRenderFenceStatus (*fence_status)(uint64_t, void *) = NULL;
    uint64_t fence = 0u;
    void *user_data = NULL;
    uint32_t index;
    uint32_t generation;
    bool stale;

    lock_ready();
    for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        if (g_batches[index].state == XG_RENDER_BATCH_COMPLETION_PENDING) {
            batch = &g_batches[index];
            break;
        }
    }
    if (batch == NULL) {
        state_unlock();
        return XG_RENDER_REAP_NONE;
    }
    batch->state = XG_RENDER_BATCH_REAPING;
    generation = batch->generation;
    fence = batch->completion_fence.fence;
    fence_status = batch->completion_fence_status;
    user_data = batch->completion_fence_status_user_data;
    state_unlock();

    status = fence_status(fence, user_data);
    if (!fence_status_valid(status)) status = XG_RENDER_FENCE_FAILED;

    state_lock();
    batch = &g_batches[index];
    if (batch->generation != generation ||
        batch->state != XG_RENDER_BATCH_REAPING) {
        state_unlock();
        return XG_RENDER_REAP_READY;
    }
    stale = !batch_is_current_locked(batch);
    if (status == XG_RENDER_FENCE_PENDING && !stale) {
        trace_mark_batch_locked(
            batch, XG_RENDER_PRESENTATION_TRACE_COMPLETION_FENCE_PENDING);
        batch->state = XG_RENDER_BATCH_COMPLETION_PENDING;
    } else {
        if (stale) count_batch_stale_locked(batch);
        if (status == XG_RENDER_FENCE_FAILED)
            trace_mark_batch_locked(
                batch, XG_RENDER_PRESENTATION_TRACE_COMPLETION_FENCE_FAILED);
        if (!stale && g_retained_batch == pack_handle(index, generation)) {
            retirement_add_fence(&retirement, batch->completion_fence.fence,
                batch->completion_fence.discard_fence,
                batch->completion_fence.user_data);
            memset(&batch->completion_fence, 0, sizeof(batch->completion_fence));
            batch->completion_fence_status = NULL;
            batch->completion_fence_status_user_data = NULL;
            batch->state = XG_RENDER_BATCH_RETAINED;
        } else {
            detach_batch_locked(batch, &retirement);
        }
        if (status == XG_RENDER_FENCE_FAILED)
            g_diagnostics.fence_failures++;
    }
    state_unlock();

    run_retirement(&retirement);
    if (stale && status == XG_RENDER_FENCE_PENDING)
        return XG_RENDER_REAP_READY;
    if (status == XG_RENDER_FENCE_PENDING) return XG_RENDER_REAP_PENDING;
    if (status == XG_RENDER_FENCE_FAILED) return XG_RENDER_REAP_FAILED;
    return XG_RENDER_REAP_READY;
}

bool xg_render_presenter_drain_retirements(
        const XgRenderPresenterServices *services) {
    bool acquired;

    lock_ready();
    acquired = presenter_acquire_locked(services);
    state_unlock();
    if (!acquired) return false;
    (void)drain_deferred_retirements();
    presenter_release();
    return true;
}

bool xg_render_presenter_sync_source_clock(
        const XgRenderPresenterServices *services, uint64_t guest_cycle,
        uint64_t guest_time_ns, bool realtime, bool rebase) {
    lock_ready();
    if (services == NULL || services->owner_token == 0u || g_presenter_busy ||
        services->owner_token != g_presenter_owner_token || !g_publication_open ||
        (realtime && guest_time_ns > UINT64_MAX - XG_RENDER_TEMPORAL_BUDGET_NS)) {
        state_unlock();
        return false;
    }
    if (g_visual_realtime != realtime || rebase) g_visual_origin_seen = false;
    g_visual_realtime = realtime;
    if (realtime && !g_visual_origin_seen && services->clock_ns != NULL) {
        /* This pair comes from the simulation pacer, even if all recent
         * sources were duplicate/APPLIED-only. Never date an old hold as now. */
        g_visual_origin_cycle = guest_cycle;
        g_visual_origin_ns = guest_time_ns;
        g_visual_origin_seen = true;
    }
    state_unlock();
    return true;
}

static XgRenderPresenterResult presenter_present(
        const XgRenderPresenterServices *services, bool hold) {
    XgRenderCompletionReapResult reap_result;
    XgRenderPresentationBatchHandle handle;
    XgRenderPresentationBatch *batch;
    XgRenderCompiledEndpoint endpoint;
    XgRenderRetirement retirement = {0};
    uint64_t packed;
    uint64_t completion_fence = 0u;
    uint64_t alpha_numerator = 1u;
    uint64_t alpha_denominator = 1u;
    uint64_t now = 0u;
    uint64_t deadline_ns = 0u;
    uint64_t selected_phase = 0u;
    uint32_t index;
    uint32_t generation;
    bool compose_ok;
    bool stale;
    bool clock_valid = false;
    XgRenderPresenterResult final_result;

    if (services == NULL || services->fence_status == NULL ||
        services->compose == NULL || services->swap_window == NULL ||
        services->discard_fence == NULL)
        return XG_RENDER_PRESENTER_COMPOSE_FAILED;

    lock_ready();
    if (!presenter_acquire_locked(services)) {
        state_unlock();
        return XG_RENDER_PRESENTER_OWNER_REJECTED;
    }
    if (hold) g_diagnostics.hold_attempts++;
    state_unlock();
    (void)drain_deferred_retirements();

    reap_result = reap_one_completion();
    if (reap_result == XG_RENDER_REAP_FAILED) {
        presenter_release();
        return XG_RENDER_PRESENTER_FENCE_FAILED;
    }

    lock_ready();
    refresh_batch_pending_locked();
    packed = hold ? g_retained_batch : g_batch_pending;
    if (!hold && unpack_batch(g_retained_batch, &handle)) {
        const XgRenderPresentationBatch *retained = batch_from_handle_locked(handle);
        XgRenderPresentationBatchHandle incoming_handle;
        const XgRenderPresentationBatch *incoming = NULL;

        if (unpack_batch(g_batch_pending, &incoming_handle))
            incoming = batch_from_handle_locked(incoming_handle);

        /* Finishing an accepted interval is work, not an optional idle hold.
         * EOF still finishes it; a discrete successor instead cuts the temporal
         * chain so a newer whole is not held behind the old reserved deadline. */
        if (retained != NULL && retained->phase_count != 0u &&
            retained->phase_index < retained->phase_count &&
            (incoming == NULL || batch_has_temporal_base_locked(incoming, retained))) {
            packed = g_retained_batch;
            hold = true;
            g_diagnostics.hold_attempts++;
        }
    }
    if (!unpack_batch(packed, &handle)) {
        state_unlock();
        final_result = reap_result == XG_RENDER_REAP_PENDING
            ? XG_RENDER_PRESENTER_FENCE_PENDING
            : XG_RENDER_PRESENTER_EMPTY;
        presenter_release();
        return final_result;
    }
    batch = batch_from_handle_locked(handle);
    if (hold && batch != NULL &&
        batch->state == XG_RENDER_BATCH_COMPLETION_PENDING) {
        state_unlock();
        presenter_release();
        return XG_RENDER_PRESENTER_FENCE_PENDING;
    }
    if (batch == NULL || batch->state !=
            (hold ? XG_RENDER_BATCH_RETAINED : XG_RENDER_BATCH_PENDING)) {
        if (!hold) g_batch_pending = 0u;
        state_unlock();
        presenter_release();
        return XG_RENDER_PRESENTER_EMPTY;
    }
    if (!batch_is_current_locked(batch)) {
        g_batch_pending = 0u;
        count_batch_stale_locked(batch);
        trace_mark_batch_locked(
            batch, XG_RENDER_PRESENTATION_TRACE_RETIRED_BEFORE_SWAP);
        trace_set_presenter_result_locked(batch, XG_RENDER_PRESENTER_STALE);
        detach_batch_locked(batch, &retirement);
        state_unlock();
        run_retirement(&retirement);
        presenter_release();
        return XG_RENDER_PRESENTER_STALE;
    }

    index = handle.slot;
    generation = handle.generation;
    endpoint = batch->endpoint;
    if (!hold) {
        XgRenderPresentationBatchHandle previous_handle;
        XgRenderPresentationBatch *previous = NULL;

        batch->phase_count = 0u;
        batch->phase_index = 0u;
        if (unpack_batch(g_retained_batch, &previous_handle))
            previous = batch_from_handle_locked(previous_handle);
        /* Compare logical final images, not an intermediate last swapped.
         * Source-work ordinals may have APPLIED-only gaps between endpoints. */
        if (batch_has_temporal_base_locked(batch, previous))
            batch->phase_count = (uint64_t)endpoint.temporal_phase_count + 1u;
    }
    {
        uint64_t (*clock_ns)(void *) = services->clock_ns;
        void *clock_user_data = services->user_data;

        if (clock_ns == NULL ||
            (g_phase_clock_seen &&
             (clock_ns != g_phase_clock_ns ||
              clock_user_data != g_phase_clock_user_data))) {
            batch->phase_count = 0u;
        } else {
            /* Clock callbacks can reenter/invalidate just like backend calls.
             * Reserve the batch before unlocking so it cannot be coalesced. */
            batch->state = XG_RENDER_BATCH_SELECTING_PHASE;
            refresh_batch_pending_locked();
            state_unlock();
            now = services->clock_sample_valid ? services->clock_sample_ns
                : clock_ns(clock_user_data);
            state_lock();
            batch = &g_batches[index];
            stale = batch->generation != generation ||
                batch->state != XG_RENDER_BATCH_SELECTING_PHASE ||
                !batch_is_current_locked(batch);
            if (stale) {
                if (batch->generation == generation &&
                    batch->state == XG_RENDER_BATCH_SELECTING_PHASE) {
                    count_batch_stale_locked(batch);
                    trace_set_presenter_result_locked(
                        batch, XG_RENDER_PRESENTER_STALE);
                    detach_batch_locked(batch, &retirement);
                }
                state_unlock();
                run_retirement(&retirement);
                presenter_release();
                return XG_RENDER_PRESENTER_STALE;
            }
            if (g_phase_clock_seen && clock_ns == g_phase_clock_ns &&
                clock_user_data == g_phase_clock_user_data &&
                now < g_phase_clock_last_ns) {
                /* Keep the high-water mark and never restart this batch. */
                batch->phase_count = 0u;
            } else {
                g_phase_clock_ns = clock_ns;
                g_phase_clock_user_data = clock_user_data;
                g_phase_clock_last_ns = now;
                g_phase_clock_seen = true;
                deadline_ns = batch->presentation_deadline_ns;
                clock_valid = deadline_ns != 0u;
            }
        }
    }
    if (clock_valid) {
        bool due = now >= deadline_ns;
        uint64_t phase = batch->phase_count;

        if (batch->phase_count != 0u && !due) {
            const uint64_t interval = endpoint.temporal_interval_ns;
            const uint64_t remaining = deadline_ns - now;
            /* Latest due approved image, including the retained base at zero.
             * Never advance to the authored whole before its accepted deadline:
             * rounding to nearest would discard a ready last phase half a tick
             * early. Quotient/remainder avoids an overflowing time*count. */
            const uint64_t count = batch->phase_count;
            const uint64_t quotient = interval / count;
            const uint64_t remainder = interval % count;
            uint64_t low = 0u;
            uint64_t high = batch->phase_count;

            while (low < high) {
                const uint64_t middle = low + (high - low + 1u) / 2u;
                const uint64_t whole = count - middle;
                /* remainder and whole are at most UINT32_MAX. */
                const uint64_t product = remainder * whole;
                const uint64_t threshold = quotient * whole + product / count;
                if (remaining <= threshold) low = middle;
                else high = middle - 1u;
            }
            phase = low;
            due = phase != 0u;
        }
        if (hold && batch->temporal_disabled && now < deadline_ns)
            phase = batch->phase_index;
        /* Missing/disabled phases select the authored whole immediately. The
         * reserved temporal budget must not become discrete input latency. */
        if (!hold && batch->phase_count != 0u && g_retained_batch != 0u && !due) {
            batch->state = XG_RENDER_BATCH_PENDING;
            refresh_batch_pending_locked();
            state_unlock();
            presenter_release();
            return XG_RENDER_PRESENTER_EMPTY;
        }
        selected_phase = phase > batch->phase_index ? phase : batch->phase_index;
    } else {
        batch->phase_count = 0u;
    }
    if (batch->phase_count != 0u && selected_phase < batch->phase_count) {
        alpha_numerator = selected_phase;
        alpha_denominator = batch->phase_count;
    }
    if (g_phase_selection_count < 8192u) {
        XgRenderPresentationBatchHandle previous_handle;
        const XgRenderPresentationBatch *previous =
            unpack_batch(g_retained_batch, &previous_handle)
                ? batch_from_handle_locked(previous_handle) : NULL;
        g_phase_selections[g_phase_selection_count++] = (XgRenderPhaseSelectionEvent){
            .identity = endpoint.identity, .clock_ns = now, .deadline_ns = deadline_ns,
            .interval_ns = endpoint.temporal_interval_ns, .phase_count = batch->phase_count,
            .selected_phase = selected_phase, .generated_phases = endpoint.temporal_phase_count,
            .expected_digest = endpoint.temporal_previous_pixel_digest,
            .previous_digest = previous ? previous->endpoint.pixel_digest : 0u};
    }
    batch->state = XG_RENDER_BATCH_COMPOSING;
    refresh_batch_pending_locked();
    trace_mark_batch_locked(
        batch, XG_RENDER_PRESENTATION_TRACE_COMPOSE_STARTED);
    g_diagnostics.compose_attempts++;
    state_unlock();

    compose_ok = services->compose(&endpoint, alpha_numerator, alpha_denominator,
                                   &completion_fence,
                                   services->user_data);

    state_lock();
    {
        XgRenderPresentationTraceEvent *trace =
            trace_find_locked(&endpoint.identity);

        if (trace != NULL) {
            trace->completion_fence = completion_fence;
            if (compose_ok && completion_fence != 0u)
                trace->flags |=
                    XG_RENDER_PRESENTATION_TRACE_COMPOSE_SUCCEEDED;
            else
                trace->flags |= XG_RENDER_PRESENTATION_TRACE_COMPOSE_FAILED;
        }
    }
    if (compose_ok && completion_fence != 0u) {
        if (!hold) g_diagnostics.composed_endpoints++;
        g_diagnostics.last_composed_endpoint =
            receipt_from_endpoint(&endpoint);
    } else {
        g_diagnostics.compose_failures++;
    }
    batch = &g_batches[index];
    stale = batch->generation != generation ||
        batch->state != XG_RENDER_BATCH_COMPOSING ||
        !batch_is_current_locked(batch);
    if (!compose_ok || completion_fence == 0u || stale) {
        retirement_add_fence(&retirement, completion_fence,
                             services->discard_fence, services->user_data);
        if (batch->generation == generation &&
            batch->state == XG_RENDER_BATCH_COMPOSING) {
            if (stale) count_batch_stale_locked(batch);
            trace_set_presenter_result_locked(batch,
                stale ? XG_RENDER_PRESENTER_STALE
                    : XG_RENDER_PRESENTER_COMPOSE_FAILED);
            if (!stale && alpha_numerator < alpha_denominator) {
                /* An unavailable phase does not invalidate the authored whole
                 * image or its logical base. Retry whole-only at its deadline. */
                batch->temporal_disabled = true;
                if (!hold) batch->phase_count = 0u;
                batch->state = hold
                    ? XG_RENDER_BATCH_RETAINED : XG_RENDER_BATCH_PENDING;
                refresh_batch_pending_locked();
            } else if (hold && !stale) {
                /* Failed composition does not consume the last good image. */
                batch->state = XG_RENDER_BATCH_RETAINED;
            } else {
                trace_mark_batch_locked(
                    batch, XG_RENDER_PRESENTATION_TRACE_RETIRED_BEFORE_SWAP);
                detach_batch_locked(batch, &retirement);
            }
        }
        state_unlock();
        run_retirement(&retirement);
        presenter_release();
        if (stale) return XG_RENDER_PRESENTER_STALE;
        return XG_RENDER_PRESENTER_COMPOSE_FAILED;
    }

    batch->completion_fence.fence = completion_fence;
    batch->completion_fence.discard_fence = services->discard_fence;
    batch->completion_fence.user_data = services->user_data;
    batch->completion_fence_status = services->fence_status;
    batch->completion_fence_status_user_data = services->user_data;
    batch->state = XG_RENDER_BATCH_COMPOSED;
    state_unlock();

    /* Once composition mutates the target, a newer batch cannot cancel it. */
    state_lock();
    batch = &g_batches[index];
    stale = batch->generation != generation ||
        batch->state != XG_RENDER_BATCH_COMPOSED ||
        !batch_is_current_locked(batch);
    if (stale) {
        if (batch->generation == generation &&
            batch->state == XG_RENDER_BATCH_COMPOSED) {
            count_batch_stale_locked(batch);
            trace_mark_batch_locked(
                batch, XG_RENDER_PRESENTATION_TRACE_RETIRED_BEFORE_SWAP);
            trace_set_presenter_result_locked(
                batch, XG_RENDER_PRESENTER_STALE);
            detach_batch_locked(batch, &retirement);
        }
        state_unlock();
        run_retirement(&retirement);
        presenter_release();
        return XG_RENDER_PRESENTER_STALE;
    }
    batch->state = XG_RENDER_BATCH_SWAPPING;
    trace_mark_batch_locked(
        batch, XG_RENDER_PRESENTATION_TRACE_SWAP_AUTHORIZED);
    g_diagnostics.swap_authorizations++;
    g_diagnostics.last_swap_authorized_endpoint =
        receipt_from_endpoint(&endpoint);
    g_diagnostics.last_alpha_numerator = alpha_numerator;
    g_diagnostics.last_alpha_denominator = alpha_denominator;
    state_unlock();

    services->swap_window(services->user_data);

    state_lock();
    batch = &g_batches[index];
    if (batch->generation == generation &&
        batch->state == XG_RENDER_BATCH_SWAPPING) {
        trace_mark_batch_locked(
            batch, XG_RENDER_PRESENTATION_TRACE_SWAP_CALLBACK_RETURNED);
        trace_set_presenter_result_locked(
            batch, XG_RENDER_PRESENTER_PRESENTED);
        batch->phase_index = selected_phase;
        if (hold) g_diagnostics.presented_holds++;
        else g_diagnostics.presented_endpoints++;
        if (alpha_numerator < alpha_denominator)
            g_diagnostics.visual_only_updates++;
        if (batch_is_current_locked(batch)) {
            XgRenderPresentationBatchHandle previous;
            const uint64_t current = pack_handle(index, generation);
            if (g_retained_batch != current &&
                unpack_batch(g_retained_batch, &previous)) {
                XgRenderPresentationBatch *old =
                    batch_from_handle_locked(previous);
                if (old != NULL) detach_batch_locked(old, &retirement);
            }
            g_retained_batch = current;
            batch->state = XG_RENDER_BATCH_COMPLETION_PENDING;
        } else {
            count_batch_stale_locked(batch);
            detach_batch_locked(batch, &retirement);
        }
    }
    state_unlock();

    run_retirement(&retirement);
    presenter_release();
    return XG_RENDER_PRESENTER_PRESENTED;
}

XgRenderPresenterResult xg_render_presenter_present_next(
        const XgRenderPresenterServices *services) {
    return presenter_present(services, false);
}

bool xg_render_presenter_present_hold(
        const XgRenderPresenterServices *services) {
    return presenter_present(services, true) == XG_RENDER_PRESENTER_PRESENTED;
}

bool xg_render_presentation_phase_count(uint32_t source_interval_vblanks,
        uint32_t refresh_numerator, uint32_t refresh_denominator,
        uint64_t *out_phase_count) {
    uint64_t numerator;
    uint64_t denominator;
    if (source_interval_vblanks == 0u || refresh_numerator == 0u ||
        refresh_denominator == 0u || out_phase_count == NULL)
        return false;
    numerator = (uint64_t)source_interval_vblanks * refresh_numerator;
    denominator = (uint64_t)60u * refresh_denominator;
    *out_phase_count = numerator / denominator +
        (numerator % denominator != 0u ? 1u : 0u);
    if (*out_phase_count == 0u) *out_phase_count = 1u;
    return true;
}

bool xg_render_presentation_phase_alpha(uint64_t phase_index,
        uint64_t phase_count, uint64_t *out_numerator,
        uint64_t *out_denominator) {
    if (phase_index == 0u || phase_count == 0u || phase_index > phase_count ||
        out_numerator == NULL || out_denominator == NULL)
        return false;
    *out_numerator = phase_index;
    *out_denominator = phase_count;
    if (phase_index < phase_count) {
        lock_ready();
        g_diagnostics.visual_only_updates++;
        state_unlock();
    }
    return true;
}

void xg_render_semantic_presentation_diagnostics(
        XgRenderPresentationDiagnostics *out_diagnostics) {
    uint32_t index;

    if (out_diagnostics == NULL) return;
    lock_ready();
    *out_diagnostics = g_diagnostics;
    out_diagnostics->source_pending = g_source_count != 0u;
    out_diagnostics->source_queue_depth = g_source_count;
    out_diagnostics->source_queue_capacity = XG_RENDER_SOURCE_COMMIT_CAPACITY;
    out_diagnostics->worker_busy = g_worker_operations != 0u ||
        g_deferred_compile_retirements != 0u;
    out_diagnostics->retained_endpoint = g_retained_batch != 0u;
    out_diagnostics->batch_queue_depth = 0u;
    out_diagnostics->pending_batch_count = 0u;
    out_diagnostics->batch_queue_capacity =
        XG_RENDER_PRESENTATION_BATCH_CAPACITY;
    out_diagnostics->retirement_queue_depth = g_deferred_retirement_count;
    out_diagnostics->retirement_queue_capacity =
        XG_RENDER_DEFERRED_RETIREMENT_CAPACITY;
    out_diagnostics->presenter_owner_token = g_presenter_owner_token;
    out_diagnostics->publication_open = g_publication_open;
    out_diagnostics->epoch_terminal = g_epoch_terminal;
    out_diagnostics->batch_pending = false;
    for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        if (g_batches[index].state != XG_RENDER_BATCH_FREE)
            out_diagnostics->batch_queue_depth++;
        if (g_batches[index].state != XG_RENDER_BATCH_FREE &&
            g_batches[index].state != XG_RENDER_BATCH_RETAINED)
            out_diagnostics->pending_batch_count++;
        if (g_batches[index].state != XG_RENDER_BATCH_FREE &&
            g_batches[index].state != XG_RENDER_BATCH_SWAPPING)
            out_diagnostics->batch_pending = true;
    }
    state_unlock();
}

uint64_t xg_render_presentation_trace_total(void) {
    uint64_t total;

    lock_ready();
    total = g_trace_total;
    state_unlock();
    return total;
}

bool xg_render_presentation_trace_get(
        uint64_t trace_sequence, XgRenderPresentationTraceEvent *out_event) {
    bool available;

    if (out_event == NULL) return false;
    lock_ready();
    available = trace_sequence < g_trace_total &&
        g_trace_total - trace_sequence <= XG_RENDER_PRESENTATION_TRACE_CAPACITY &&
        g_trace[trace_sequence % XG_RENDER_PRESENTATION_TRACE_CAPACITY]
            .trace_sequence == trace_sequence;
    if (available)
        *out_event =
            g_trace[trace_sequence % XG_RENDER_PRESENTATION_TRACE_CAPACITY];
    state_unlock();
    return available;
}
