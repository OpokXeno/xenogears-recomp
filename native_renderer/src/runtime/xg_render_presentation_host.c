#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "xg_render_presentation_host.h"
#include "xg_threads.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define XG_RENDER_HOST_IDLE_RECHECK_MS 100L
#define XG_RENDER_HOST_WORKER_RECHECK_MS 2L
#define XG_RENDER_HOST_RETIREMENT_PERIOD_NS UINT64_C(2000000)

struct XgRenderPresentationHost {
    XgRenderWorkerServices worker_services;
    XgRenderPresenterServices presenter_services;
    XgRenderPresentationHoldPresenter present_hold;
    uint64_t presentation_period_ns;

    mtx_t mutex;
    mtx_t lifecycle_mutex;
    cnd_t worker_condition;
    cnd_t join_condition;
    thrd_t worker_thread;
    thrd_t presenter_owner_thread;
    uint64_t present_period_start_ns;
    uint64_t retirement_period_start_ns;

    atomic_bool stop_requested;
    atomic_bool shutdown_invalidation_started;
    atomic_bool shutdown_invalidation_complete;
    atomic_bool worker_running;
    atomic_bool presenter_running;
    atomic_bool worker_done;
    atomic_bool presenter_owner_ready;
    atomic_bool join_complete;
    atomic_uint_fast64_t worker_sequence;
    atomic_int state;

    bool join_started;
    bool join_succeeded;
    bool worker_created;
    XgRenderPresentationDiagnostics final_diagnostics;
    atomic_bool final_diagnostics_valid;

    atomic_uint_fast64_t notifications;
    atomic_uint_fast64_t invalidations;
    atomic_uint_fast64_t worker_attempts;
    atomic_uint_fast64_t worker_compiled;
    atomic_uint_fast64_t worker_empty;
    atomic_uint_fast64_t worker_compile_failures;
    atomic_uint_fast64_t worker_capacity_exceeded;
    atomic_uint_fast64_t worker_stale;
    atomic_uint_fast64_t worker_applied;
    atomic_uint_fast64_t worker_would_block;
    atomic_uint_fast64_t worker_lane_blocked;
    atomic_uint_fast64_t presenter_attempts;
    atomic_uint_fast64_t presenter_presented;
    atomic_uint_fast64_t presenter_empty;
    atomic_uint_fast64_t presenter_fence_pending;
    atomic_uint_fast64_t presenter_fence_failures;
    atomic_uint_fast64_t presenter_stale;
    atomic_uint_fast64_t presenter_compose_failures;
    atomic_uint_fast64_t presenter_owner_rejections;
    atomic_uint_fast64_t presenter_hold_attempts;
    atomic_uint_fast64_t presenter_held;
    atomic_uint_fast64_t retirement_drains;
    atomic_uint_fast64_t retirement_drain_failures;
    atomic_int last_worker_result;
    atomic_int last_presenter_result;
};

static atomic_flag g_exclusive_host = ATOMIC_FLAG_INIT;
static atomic_uint_fast64_t g_presenter_owner_sequence = 1u;

static void counter_increment(atomic_uint_fast64_t *counter) {
    (void)atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
}

static uint64_t next_owner_token(void) {
    uint64_t token;
    do {
        token = atomic_fetch_add_explicit(&g_presenter_owner_sequence, 1u,
                                          memory_order_relaxed);
    } while (token == 0u);
    return token;
}

static struct timespec deadline_after_milliseconds(long milliseconds) {
    struct timespec deadline = {0};
    /* C11 cnd_timedwait requires an absolute TIME_UTC deadline. */
    (void)timespec_get(&deadline, TIME_UTC);
    deadline.tv_sec += milliseconds / 1000L;
    deadline.tv_nsec += (milliseconds % 1000L) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static bool monotonic_nanoseconds(uint64_t *nanoseconds) {
#if defined(_WIN32)
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t ticks;
    uint64_t ticks_per_second;

    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
        return false;
    ticks = (uint64_t)counter.QuadPart;
    ticks_per_second = (uint64_t)frequency.QuadPart;
    /* Scale only the fractional second in floating point to avoid overflow
     * without losing precision as uptime grows. */
    *nanoseconds = (ticks / ticks_per_second) * 1000000000u +
        (uint64_t)((long double)(ticks % ticks_per_second) * 1000000000.0L /
                   (long double)ticks_per_second);
#else
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return false;
    *nanoseconds = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
#endif
    return true;
}

static void signal_worker(XgRenderPresentationHost *host) {
    /* Serialize with the predicate-to-wait transition. This mutex never
     * covers backend work or thrd_join, so guest notification stays short. */
    (void)mtx_lock(&host->mutex);
    (void)atomic_fetch_add_explicit(&host->worker_sequence, 1u,
                                    memory_order_release);
    (void)cnd_signal(&host->worker_condition);
    (void)mtx_unlock(&host->mutex);
}

static bool wait_for_sequence(XgRenderPresentationHost *host,
                              cnd_t *condition,
                              atomic_uint_fast64_t *sequence,
                              uint64_t *seen_sequence,
                              long recheck_ms) {
    bool ready;
    const struct timespec deadline = deadline_after_milliseconds(recheck_ms);

    (void)mtx_lock(&host->mutex);
    for (;;) {
        const uint64_t current =
            atomic_load_explicit(sequence, memory_order_acquire);
        if (current != *seen_sequence ||
            atomic_load_explicit(&host->stop_requested, memory_order_acquire)) {
            *seen_sequence = current;
            ready = true;
            break;
        }
        {
            const int result = cnd_timedwait(condition, &host->mutex,
                                             &deadline);
            if (result == thrd_timedout) {
                ready = false;
                break;
            }
            if (result != thrd_success) {
                ready = true;
                break;
            }
        }
    }
    (void)mtx_unlock(&host->mutex);
    return ready;
}

static bool caller_is_presenter_owner(XgRenderPresentationHost *host) {
    return atomic_load_explicit(&host->presenter_owner_ready,
                                memory_order_acquire) &&
        thrd_equal(thrd_current(), host->presenter_owner_thread) != 0;
}

static bool caller_is_worker_thread(XgRenderPresentationHost *host) {
    return host->worker_created &&
        !atomic_load_explicit(&host->worker_done, memory_order_acquire) &&
        thrd_equal(thrd_current(), host->worker_thread) != 0;
}

static bool diagnostics_are_quiescent(
        const XgRenderPresentationDiagnostics *diagnostics) {
    return diagnostics != NULL && !diagnostics->source_pending &&
        !diagnostics->batch_pending &&
        !diagnostics->worker_busy && !diagnostics->retained_endpoint &&
        !diagnostics->publication_open &&
        diagnostics->source_queue_depth == 0u &&
        diagnostics->batch_queue_depth == 0u &&
        diagnostics->retirement_queue_depth == 0u;
}

static void record_worker_result(XgRenderPresentationHost *host,
                                 XgRenderWorkerResult result) {
    counter_increment(&host->worker_attempts);
    atomic_store_explicit(&host->last_worker_result, (int)result,
                          memory_order_relaxed);
    switch (result) {
    case XG_RENDER_WORKER_OK:
        counter_increment(&host->worker_compiled);
        break;
    case XG_RENDER_WORKER_EMPTY:
        counter_increment(&host->worker_empty);
        break;
    case XG_RENDER_WORKER_COMPILE_FAILED:
        counter_increment(&host->worker_compile_failures);
        break;
    case XG_RENDER_WORKER_CAPACITY_EXCEEDED:
        counter_increment(&host->worker_capacity_exceeded);
        break;
    case XG_RENDER_WORKER_STALE:
        counter_increment(&host->worker_stale);
        break;
    case XG_RENDER_WORKER_APPLIED:
        counter_increment(&host->worker_applied);
        break;
    case XG_RENDER_WORKER_WOULD_BLOCK:
        counter_increment(&host->worker_would_block);
        break;
    case XG_RENDER_WORKER_LANE_BLOCKED:
        counter_increment(&host->worker_lane_blocked);
        break;
    }
}

static void record_presenter_result(XgRenderPresentationHost *host,
                                    XgRenderPresenterResult result) {
    counter_increment(&host->presenter_attempts);
    atomic_store_explicit(&host->last_presenter_result, (int)result,
                          memory_order_relaxed);
    switch (result) {
    case XG_RENDER_PRESENTER_PRESENTED:
        counter_increment(&host->presenter_presented);
        break;
    case XG_RENDER_PRESENTER_EMPTY:
        counter_increment(&host->presenter_empty);
        break;
    case XG_RENDER_PRESENTER_FENCE_PENDING:
        counter_increment(&host->presenter_fence_pending);
        break;
    case XG_RENDER_PRESENTER_FENCE_FAILED:
        counter_increment(&host->presenter_fence_failures);
        break;
    case XG_RENDER_PRESENTER_STALE:
        counter_increment(&host->presenter_stale);
        break;
    case XG_RENDER_PRESENTER_COMPOSE_FAILED:
        counter_increment(&host->presenter_compose_failures);
        break;
    case XG_RENDER_PRESENTER_OWNER_REJECTED:
        counter_increment(&host->presenter_owner_rejections);
        break;
    }
}

static bool drain_retirements(XgRenderPresentationHost *host) {
    bool drained;

    if (!caller_is_presenter_owner(host)) {
        counter_increment(&host->retirement_drain_failures);
        return false;
    }
    drained = xg_render_presenter_drain_retirements(
        &host->presenter_services);
    if (drained)
        counter_increment(&host->retirement_drains);
    else
        counter_increment(&host->retirement_drain_failures);
    return drained;
}

static void fail_host_from_presenter(XgRenderPresentationHost *host) {
    atomic_store_explicit(&host->state, XG_RENDER_PRESENTATION_HOST_FAILED,
                          memory_order_release);
    (void)mtx_lock(&host->lifecycle_mutex);
    atomic_store_explicit(&host->stop_requested, true, memory_order_release);
    if (!atomic_exchange_explicit(&host->shutdown_invalidation_started, true,
                                  memory_order_acq_rel)) {
        const XgRenderPresentationLifecycleResult result =
            xg_render_presentation_lifecycle_close(
                host->presenter_services.owner_token,
                XG_RENDER_TIMELINE_RESET, NULL);
        if (result == XG_RENDER_PRESENTATION_LIFECYCLE_OK ||
            result == XG_RENDER_PRESENTATION_LIFECYCLE_EPOCH_EXHAUSTED) {
            counter_increment(&host->invalidations);
            atomic_store_explicit(&host->shutdown_invalidation_complete, true,
                                  memory_order_release);
        }
    }
    (void)mtx_unlock(&host->lifecycle_mutex);
    signal_worker(host);
}

static int worker_main(void *user_data) {
    XgRenderPresentationHost *host =
        (XgRenderPresentationHost *)user_data;
    uint64_t seen_sequence = 0u;
    bool retry_pending = false;
    bool lane_blocked = false;

    atomic_store_explicit(&host->worker_running, true, memory_order_release);
    for (;;) {
        bool notified = wait_for_sequence(
            host, &host->worker_condition, &host->worker_sequence, &seen_sequence,
            retry_pending ? XG_RENDER_HOST_WORKER_RECHECK_MS
                          : XG_RENDER_HOST_IDLE_RECHECK_MS);
        if (atomic_load_explicit(&host->stop_requested,
                                  memory_order_acquire))
            break;
        if (lane_blocked) {
            XgRenderPresentationDiagnostics diagnostics;
            xg_render_semantic_presentation_diagnostics(&diagnostics);
            /* Publication and retirement wakes cannot repair a failed delta.
             * Only core invalidation clears this latch. Polling also observes
             * invalidations performed outside the host notification facade. */
            if (diagnostics.source_lane_blocked)
                continue;
            lane_blocked = false;
            notified = true;
        }
        if (!notified && !retry_pending)
            continue;

        for (;;) {
            const XgRenderWorkerResult result =
                xg_render_worker_compile_next(&host->worker_services);
            record_worker_result(host, result);
            if (atomic_load_explicit(&host->stop_requested,
                                     memory_order_acquire))
                break;
            if (result == XG_RENDER_WORKER_OK ||
                result == XG_RENDER_WORKER_APPLIED)
                continue;
            lane_blocked = result == XG_RENDER_WORKER_COMPILE_FAILED ||
                           result == XG_RENDER_WORKER_LANE_BLOCKED;
            /* Pending compile fences and backend/batch capacity can change
             * without another publication. Retry with a timed wait, not a
             * loop over the same non-discardable FIFO head. */
            retry_pending = result == XG_RENDER_WORKER_WOULD_BLOCK ||
                            result == XG_RENDER_WORKER_CAPACITY_EXCEEDED ||
                            result == XG_RENDER_WORKER_STALE;
            break;
        }
    }

    atomic_store_explicit(&host->worker_running, false, memory_order_release);
    atomic_store_explicit(&host->worker_done, true, memory_order_release);
    return 0;
}

static bool services_are_valid(
        const XgRenderWorkerServices *worker_services,
        const XgRenderPresenterServices *presenter_services) {
    return worker_services != NULL && worker_services->compile != NULL &&
        worker_services->fence_status != NULL &&
        worker_services->discard_fence != NULL &&
        worker_services->release_endpoint != NULL &&
        presenter_services != NULL &&
        presenter_services->fence_status != NULL &&
        presenter_services->compose != NULL &&
        presenter_services->swap_window != NULL &&
        presenter_services->discard_fence != NULL;
}

XgRenderPresentationHost *xg_render_presentation_host_start(
        const XgRenderWorkerServices *worker_services,
        const XgRenderPresenterServices *presenter_services,
        uint64_t presentation_period_ns) {
    XgRenderPresentationHost *host;
    XgRenderPresentationDiagnostics diagnostics;
    bool lifecycle_opened = false;
    bool release_exclusive = true;

    if (!services_are_valid(worker_services, presenter_services) ||
        presentation_period_ns == 0u ||
        atomic_flag_test_and_set_explicit(&g_exclusive_host,
                                          memory_order_acquire))
        return NULL;
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    if (!diagnostics_are_quiescent(&diagnostics)) {
        atomic_flag_clear_explicit(&g_exclusive_host, memory_order_release);
        return NULL;
    }

    host = (XgRenderPresentationHost *)calloc(1u, sizeof(*host));
    if (host == NULL) {
        atomic_flag_clear_explicit(&g_exclusive_host, memory_order_release);
        return NULL;
    }
    host->worker_services = *worker_services;
    host->presenter_services = *presenter_services;
    host->presenter_services.clock_sample_valid = false;
    host->presentation_period_ns = presentation_period_ns;
    host->presenter_services.owner_token = next_owner_token();
    host->presenter_owner_thread = thrd_current();
    if (!monotonic_nanoseconds(&host->present_period_start_ns))
        goto fail_all;
    host->retirement_period_start_ns = host->present_period_start_ns;
    atomic_init(&host->stop_requested, false);
    atomic_init(&host->shutdown_invalidation_started, false);
    atomic_init(&host->shutdown_invalidation_complete, false);
    atomic_init(&host->worker_running, false);
    atomic_init(&host->presenter_running, false);
    atomic_init(&host->worker_done, false);
    atomic_init(&host->presenter_owner_ready, true);
    atomic_init(&host->join_complete, false);
    atomic_init(&host->worker_sequence, 0u);
    atomic_init(&host->state, XG_RENDER_PRESENTATION_HOST_RUNNING);
    atomic_init(&host->final_diagnostics_valid, false);
    atomic_init(&host->notifications, 0u);
    atomic_init(&host->invalidations, 0u);
    atomic_init(&host->worker_attempts, 0u);
    atomic_init(&host->worker_compiled, 0u);
    atomic_init(&host->worker_empty, 0u);
    atomic_init(&host->worker_compile_failures, 0u);
    atomic_init(&host->worker_capacity_exceeded, 0u);
    atomic_init(&host->worker_stale, 0u);
    atomic_init(&host->worker_applied, 0u);
    atomic_init(&host->worker_would_block, 0u);
    atomic_init(&host->worker_lane_blocked, 0u);
    atomic_init(&host->presenter_attempts, 0u);
    atomic_init(&host->presenter_presented, 0u);
    atomic_init(&host->presenter_empty, 0u);
    atomic_init(&host->presenter_fence_pending, 0u);
    atomic_init(&host->presenter_fence_failures, 0u);
    atomic_init(&host->presenter_stale, 0u);
    atomic_init(&host->presenter_compose_failures, 0u);
    atomic_init(&host->presenter_owner_rejections, 0u);
    atomic_init(&host->presenter_hold_attempts, 0u);
    atomic_init(&host->presenter_held, 0u);
    atomic_init(&host->retirement_drains, 0u);
    atomic_init(&host->retirement_drain_failures, 0u);
    atomic_init(&host->last_worker_result, XG_RENDER_WORKER_EMPTY);
    atomic_init(&host->last_presenter_result, XG_RENDER_PRESENTER_EMPTY);

    if (mtx_init(&host->mutex, mtx_plain) != thrd_success)
        goto fail_all;
    if (mtx_init(&host->lifecycle_mutex, mtx_plain) != thrd_success)
        goto fail_mutex;
    if (cnd_init(&host->worker_condition) != thrd_success)
        goto fail_lifecycle_mutex;
    if (cnd_init(&host->join_condition) != thrd_success)
        goto fail_worker_condition;

    if (xg_render_presentation_lifecycle_claim_open(
            host->presenter_services.owner_token) !=
            XG_RENDER_PRESENTATION_LIFECYCLE_OK)
        goto fail_conditions;
    lifecycle_opened = true;

    if (!drain_retirements(host))
        goto fail_conditions;
    if (thrd_create(&host->worker_thread, worker_main, host) != thrd_success)
        goto fail_conditions;
    host->worker_created = true;
    return host;

fail_conditions:
    if (lifecycle_opened) {
        const XgRenderPresentationLifecycleResult close_result =
            xg_render_presentation_lifecycle_close(
                host->presenter_services.owner_token,
                XG_RENDER_TIMELINE_RESET, NULL);
        if (close_result == XG_RENDER_PRESENTATION_LIFECYCLE_OK ||
            close_result == XG_RENDER_PRESENTATION_LIFECYCLE_EPOCH_EXHAUSTED) {
            lifecycle_opened = false;
            if (!drain_retirements(host)) release_exclusive = false;
        } else {
            release_exclusive = false;
        }
    }
    atomic_store_explicit(&host->presenter_owner_ready, false,
                          memory_order_release);
    cnd_destroy(&host->join_condition);
fail_worker_condition:
    cnd_destroy(&host->worker_condition);
fail_lifecycle_mutex:
    mtx_destroy(&host->lifecycle_mutex);
fail_mutex:
    mtx_destroy(&host->mutex);
fail_all:
    free(host);
    if (release_exclusive && !lifecycle_opened)
        atomic_flag_clear_explicit(&g_exclusive_host, memory_order_release);
    return NULL;
}

void xg_render_presentation_host_notify(XgRenderPresentationHost *host) {
    if (host == NULL) return;
    if (atomic_load_explicit(&host->state, memory_order_acquire) !=
            XG_RENDER_PRESENTATION_HOST_RUNNING ||
        atomic_load_explicit(&host->stop_requested, memory_order_acquire))
        return;
    counter_increment(&host->notifications);
    signal_worker(host);
}

bool xg_render_presentation_host_pump(XgRenderPresentationHost *host) {
    XgRenderPresenterResult result;
    XgRenderPresentationDiagnostics after;
    uint64_t now_ns;
    uint64_t elapsed_ns;
    uint64_t advance_ns;
    bool accepted = true;

    if (host == NULL) return false;
    if (!caller_is_presenter_owner(host)) {
        counter_increment(&host->presenter_owner_rejections);
        return false;
    }
    if (atomic_exchange_explicit(&host->presenter_running, true,
                                 memory_order_acq_rel)) {
        counter_increment(&host->presenter_owner_rejections);
        return false;
    }
    if (atomic_load_explicit(&host->state, memory_order_acquire) !=
            XG_RENDER_PRESENTATION_HOST_RUNNING ||
        atomic_load_explicit(&host->stop_requested, memory_order_acquire)) {
        accepted = false;
        goto done;
    }

    if (!monotonic_nanoseconds(&now_ns)) {
        fail_host_from_presenter(host);
        accepted = false;
        goto done;
    }
    elapsed_ns = now_ns - host->retirement_period_start_ns;
    if (elapsed_ns >= XG_RENDER_HOST_RETIREMENT_PERIOD_NS) {
        host->retirement_period_start_ns = now_ns;
        if (!drain_retirements(host)) {
            fail_host_from_presenter(host);
            accepted = false;
            goto done;
        }
        xg_render_semantic_presentation_diagnostics(&after);
        if (after.source_pending && !after.source_lane_blocked)
            signal_worker(host);
        /* Retirement callbacks can wait on GPU work. Select the latest tick
         * after that work, rather than composing an already missed deadline. */
        if (!monotonic_nanoseconds(&now_ns)) {
            fail_host_from_presenter(host);
            accepted = false;
            goto done;
        }
    }
    /* Unsigned elapsed time avoids overflowing a future deadline. Skip missed
     * periods in O(1), preserving the host cadence independently of guest work. */
    elapsed_ns = now_ns - host->present_period_start_ns;
    if (elapsed_ns < host->presentation_period_ns)
        goto done;
    advance_ns = elapsed_ns - elapsed_ns % host->presentation_period_ns;
    host->present_period_start_ns += advance_ns;
    /* The phase selector samples the scheduled tick, not when SDL/GL happened
     * to get CPU time. Keep backend clock identity and callback ownership. */
    if (host->presenter_services.clock_sample_valid) {
        if (advance_ns > UINT64_MAX - host->presenter_services.clock_sample_ns) {
            fail_host_from_presenter(host);
            accepted = false;
            goto done;
        }
        host->presenter_services.clock_sample_ns += advance_ns;
    } else if (host->presenter_services.clock_ns != NULL) {
        const uint64_t clock_ns = host->presenter_services.clock_ns(
            host->presenter_services.user_data);
        uint64_t sampled_ns;
        if (!monotonic_nanoseconds(&sampled_ns)) {
            fail_host_from_presenter(host);
            accepted = false;
            goto done;
        }
        const uint64_t lateness_ns = sampled_ns - host->present_period_start_ns;
        host->presenter_services.clock_sample_ns = clock_ns >= lateness_ns
            ? clock_ns - lateness_ns : 0u;
        host->presenter_services.clock_sample_valid = true;
    }
    result = xg_render_presenter_present_next(&host->presenter_services);
    record_presenter_result(host, result);
    if ((result == XG_RENDER_PRESENTER_EMPTY ||
         result == XG_RENDER_PRESENTER_FENCE_PENDING ||
         result == XG_RENDER_PRESENTER_COMPOSE_FAILED) && host->present_hold) {
        counter_increment(&host->presenter_hold_attempts);
        if (host->present_hold(&host->presenter_services)) {
            counter_increment(&host->presenter_held);
            record_presenter_result(host, XG_RENDER_PRESENTER_PRESENTED);
        }
    }
    /* Completion-fence reap or a hold can free capacity without reducing the
     * visible batch depth. Wake pending FIFO work after all owner-side work. */
    xg_render_semantic_presentation_diagnostics(&after);
    if (after.source_pending && !after.source_lane_blocked)
        signal_worker(host);
    if (result == XG_RENDER_PRESENTER_OWNER_REJECTED) {
        fail_host_from_presenter(host);
        accepted = false;
    }

done:
    atomic_store_explicit(&host->presenter_running, false,
                          memory_order_release);
    return accepted;
}

static bool time_until_pump(
        XgRenderPresentationHost *host, uint64_t *out_nanoseconds,
        bool include_retirements) {
    uint64_t now_ns;
    uint64_t elapsed_ns;

    if (host == NULL || out_nanoseconds == NULL) return false;
    *out_nanoseconds = 0u;
    if (!caller_is_presenter_owner(host) ||
        atomic_load_explicit(&host->presenter_running, memory_order_acquire)) {
        counter_increment(&host->presenter_owner_rejections);
        return false;
    }
    if (atomic_load_explicit(&host->state, memory_order_acquire) !=
            XG_RENDER_PRESENTATION_HOST_RUNNING ||
        atomic_load_explicit(&host->stop_requested, memory_order_acquire))
        return false;
    if (!monotonic_nanoseconds(&now_ns)) {
        fail_host_from_presenter(host);
        return false;
    }
    elapsed_ns = now_ns - host->present_period_start_ns;
    if (elapsed_ns < host->presentation_period_ns)
        *out_nanoseconds = host->presentation_period_ns - elapsed_ns;
    if (!include_retirements) return true;
    elapsed_ns = now_ns - host->retirement_period_start_ns;
    if (elapsed_ns >= XG_RENDER_HOST_RETIREMENT_PERIOD_NS)
        *out_nanoseconds = 0u;
    else if (*out_nanoseconds > XG_RENDER_HOST_RETIREMENT_PERIOD_NS - elapsed_ns)
        *out_nanoseconds = XG_RENDER_HOST_RETIREMENT_PERIOD_NS - elapsed_ns;
    return true;
}

bool xg_render_presentation_host_time_until_pump(
        XgRenderPresentationHost *host, uint64_t *out_nanoseconds) {
    return time_until_pump(host, out_nanoseconds, true);
}

bool xg_render_presentation_host_time_until_present(
        XgRenderPresentationHost *host, uint64_t *out_nanoseconds) {
    return time_until_pump(host, out_nanoseconds, false);
}

/* Owner-only, between pumps. Retargets the presenter tick cadence live (the
 * Toggles FPS selector): the next pump schedules against the new period, one
 * phase-selected compose+swap per tick, so a shorter period multiplies output
 * and a longer one divides it. No rebase: shortening fires the next tick
 * immediately (responsive speed-up), lengthening simply waits out the longer
 * period. Pumps already emit at most one present each, so no burst is
 * possible in either direction. */
bool xg_render_presentation_host_set_period(
        XgRenderPresentationHost *host, uint64_t period_ns) {
    if (host == NULL || period_ns == 0u) return false;
    /* Owner-only, but explicitly ALLOWED while a pump is in flight: the
     * debug overlay applies this from inside its tools window, which draws
     * from within pre_swap — i.e. inside the pump's dynamic extent, where
     * presenter_running is always true. A plain aligned u64 store takes
     * effect on the next pump; the pump reads the period at well-defined
     * points, so a mid-pump change cannot tear a tick. */
    if (!caller_is_presenter_owner(host)) {
        counter_increment(&host->presenter_owner_rejections);
        return false;
    }
    if (atomic_load_explicit(&host->state, memory_order_acquire) !=
            XG_RENDER_PRESENTATION_HOST_RUNNING ||
        atomic_load_explicit(&host->stop_requested, memory_order_acquire))
        return false;
    host->presentation_period_ns = period_ns;
    return true;
}

bool xg_render_presentation_host_sync_source_clock(
        XgRenderPresentationHost *host, uint64_t guest_cycle,
        int64_t guest_time_offset_ns, bool realtime, bool rebase) {
    uint64_t guest_time_ns = 0u;
    bool accepted = false;

    if (host == NULL) return false;
    if (!caller_is_presenter_owner(host) ||
        atomic_exchange_explicit(&host->presenter_running, true, memory_order_acq_rel)) {
        counter_increment(&host->presenter_owner_rejections);
        return false;
    }
    if (atomic_load_explicit(&host->state, memory_order_acquire) !=
            XG_RENDER_PRESENTATION_HOST_RUNNING ||
        atomic_load_explicit(&host->stop_requested, memory_order_acquire))
        goto done;
    if (realtime && host->presenter_services.clock_ns != NULL) {
        if (host->presenter_services.clock_sample_valid) {
            uint64_t now_ns;
            uint64_t elapsed_ns;
            if (!monotonic_nanoseconds(&now_ns)) goto done;
            elapsed_ns = now_ns - host->present_period_start_ns;
            if (elapsed_ns > UINT64_MAX - host->presenter_services.clock_sample_ns)
                goto done;
            guest_time_ns = host->presenter_services.clock_sample_ns + elapsed_ns;
        } else {
            guest_time_ns = host->presenter_services.clock_ns(
                host->presenter_services.user_data);
        }
        if (guest_time_offset_ns < 0) {
            const uint64_t earlier_ns = (uint64_t)(-(guest_time_offset_ns + 1)) + 1u;
            guest_time_ns = earlier_ns < guest_time_ns ? guest_time_ns - earlier_ns : 0u;
        } else {
            const uint64_t later_ns = (uint64_t)guest_time_offset_ns;
            if (later_ns > UINT64_MAX - guest_time_ns) goto done;
            guest_time_ns += later_ns;
        }
    }
    accepted = xg_render_presenter_sync_source_clock(
        &host->presenter_services, guest_cycle, guest_time_ns, realtime, rebase);
done:
    atomic_store_explicit(&host->presenter_running, false, memory_order_release);
    return accepted;
}

bool xg_render_presentation_host_set_hold_presenter(
        XgRenderPresentationHost *host,
        XgRenderPresentationHoldPresenter present_hold) {
    if (host == NULL) return false;
    if (!caller_is_presenter_owner(host) ||
        atomic_load_explicit(&host->presenter_running, memory_order_acquire)) {
        counter_increment(&host->presenter_owner_rejections);
        return false;
    }
    if (atomic_load_explicit(&host->state, memory_order_acquire) !=
            XG_RENDER_PRESENTATION_HOST_RUNNING ||
        atomic_load_explicit(&host->stop_requested, memory_order_acquire))
        return false;
    host->present_hold = present_hold;
    return true;
}

uint64_t xg_render_presentation_host_invalidate_or_wake(
        XgRenderPresentationHost *host,
        XgRenderTimelineInvalidationReason reason) {
    uint64_t epoch;

    if (host == NULL) return 0u;
    (void)mtx_lock(&host->lifecycle_mutex);
    if (atomic_load_explicit(&host->state, memory_order_acquire) !=
            XG_RENDER_PRESENTATION_HOST_RUNNING ||
        atomic_load_explicit(&host->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&host->shutdown_invalidation_started,
                             memory_order_acquire)) {
        XgRenderPresentationDiagnostics diagnostics;
        xg_render_semantic_presentation_diagnostics(&diagnostics);
        epoch = diagnostics.presentation_epoch;
    } else {
        epoch = xg_render_timeline_invalidate(reason);
        counter_increment(&host->invalidations);
    }
    (void)mtx_unlock(&host->lifecycle_mutex);
    signal_worker(host);
    return epoch;
}

void xg_render_presentation_host_shutdown(XgRenderPresentationHost *host) {
    if (host == NULL) return;
    (void)mtx_lock(&host->lifecycle_mutex);
    atomic_store_explicit(&host->stop_requested, true, memory_order_release);
    if (!atomic_exchange_explicit(&host->shutdown_invalidation_started, true,
                                  memory_order_acq_rel)) {
        XgRenderPresentationLifecycleResult close_result;
        int expected_state = XG_RENDER_PRESENTATION_HOST_RUNNING;
        (void)atomic_compare_exchange_strong_explicit(
            &host->state, &expected_state,
            XG_RENDER_PRESENTATION_HOST_SHUTTING_DOWN,
            memory_order_acq_rel, memory_order_acquire);
        close_result = xg_render_presentation_lifecycle_close(
            host->presenter_services.owner_token,
            XG_RENDER_TIMELINE_RESET, NULL);
        if (close_result == XG_RENDER_PRESENTATION_LIFECYCLE_OK ||
            close_result == XG_RENDER_PRESENTATION_LIFECYCLE_EPOCH_EXHAUSTED) {
            counter_increment(&host->invalidations);
            atomic_store_explicit(&host->shutdown_invalidation_complete, true,
                                  memory_order_release);
        } else {
            atomic_store_explicit(&host->state,
                                  XG_RENDER_PRESENTATION_HOST_FAILED,
                                  memory_order_release);
        }
    }
    (void)mtx_unlock(&host->lifecycle_mutex);
    signal_worker(host);
}

bool xg_render_presentation_host_join(XgRenderPresentationHost *host) {
    bool result;
    bool retirement_result = false;
    int worker_result = thrd_success;
    XgRenderPresentationHostState previous_state;

    if (host == NULL) return false;
    (void)mtx_lock(&host->mutex);
    if (host->join_started) {
        while (!atomic_load_explicit(&host->join_complete,
                                     memory_order_acquire)) {
            if (caller_is_worker_thread(host)) {
                (void)mtx_unlock(&host->mutex);
                return false;
            }
            (void)cnd_wait(&host->join_condition, &host->mutex);
        }
        result = host->join_succeeded;
        (void)mtx_unlock(&host->mutex);
        return result;
    }
    if (!caller_is_presenter_owner(host) ||
        atomic_load_explicit(&host->presenter_running,
                             memory_order_acquire)) {
        (void)mtx_unlock(&host->mutex);
        return false;
    }
    host->join_started = true;
    (void)mtx_unlock(&host->mutex);

    xg_render_presentation_host_shutdown(host);
    if (host->worker_created)
        worker_result = thrd_join(host->worker_thread, NULL);
    if (worker_result == thrd_success)
        retirement_result = drain_retirements(host);

    xg_render_semantic_presentation_diagnostics(&host->final_diagnostics);
    atomic_store_explicit(&host->final_diagnostics_valid, true,
                          memory_order_release);

    result = worker_result == thrd_success && retirement_result &&
        atomic_load_explicit(&host->shutdown_invalidation_complete,
                              memory_order_acquire) &&
        atomic_load_explicit(&host->worker_done, memory_order_acquire) &&
        !atomic_load_explicit(&host->worker_running, memory_order_acquire) &&
        !atomic_load_explicit(&host->presenter_running, memory_order_acquire) &&
        diagnostics_are_quiescent(&host->final_diagnostics);
    previous_state = (XgRenderPresentationHostState)
        atomic_load_explicit(&host->state, memory_order_acquire);
    if (!result) {
        atomic_store_explicit(&host->state, XG_RENDER_PRESENTATION_HOST_FAILED,
                              memory_order_release);
    } else if (previous_state != XG_RENDER_PRESENTATION_HOST_FAILED) {
        atomic_store_explicit(&host->state, XG_RENDER_PRESENTATION_HOST_JOINED,
                              memory_order_release);
    }
    if (result) {
        atomic_store_explicit(&host->presenter_owner_ready, false,
                              memory_order_release);
        atomic_flag_clear_explicit(&g_exclusive_host, memory_order_release);
    }

    (void)mtx_lock(&host->mutex);
    host->join_succeeded = result;
    atomic_store_explicit(&host->join_complete, true, memory_order_release);
    (void)cnd_broadcast(&host->join_condition);
    (void)mtx_unlock(&host->mutex);
    return result;
}

bool xg_render_presentation_host_snapshot(
        XgRenderPresentationHost *host,
        XgRenderPresentationHostSnapshot *out_snapshot) {
#define XG_HOST_LOAD(name) \
    atomic_load_explicit(&host->name, memory_order_relaxed)

    if (host == NULL || out_snapshot == NULL) return false;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->state = (XgRenderPresentationHostState)
        atomic_load_explicit(&host->state, memory_order_acquire);
    out_snapshot->worker_running = XG_HOST_LOAD(worker_running);
    out_snapshot->presenter_running = XG_HOST_LOAD(presenter_running);
    out_snapshot->shutdown_requested = XG_HOST_LOAD(stop_requested);
    out_snapshot->shutdown_complete =
        XG_HOST_LOAD(shutdown_invalidation_complete);
    out_snapshot->join_complete = XG_HOST_LOAD(join_complete);
    out_snapshot->presenter_owner_identity_valid =
        XG_HOST_LOAD(presenter_owner_ready);
    out_snapshot->notifications = XG_HOST_LOAD(notifications);
    out_snapshot->invalidations = XG_HOST_LOAD(invalidations);
    out_snapshot->worker_attempts = XG_HOST_LOAD(worker_attempts);
    out_snapshot->worker_compiled = XG_HOST_LOAD(worker_compiled);
    out_snapshot->worker_empty = XG_HOST_LOAD(worker_empty);
    out_snapshot->worker_compile_failures =
        XG_HOST_LOAD(worker_compile_failures);
    out_snapshot->worker_capacity_exceeded =
        XG_HOST_LOAD(worker_capacity_exceeded);
    out_snapshot->worker_stale = XG_HOST_LOAD(worker_stale);
    out_snapshot->worker_applied = XG_HOST_LOAD(worker_applied);
    out_snapshot->worker_would_block = XG_HOST_LOAD(worker_would_block);
    out_snapshot->worker_lane_blocked = XG_HOST_LOAD(worker_lane_blocked);
    out_snapshot->presenter_attempts = XG_HOST_LOAD(presenter_attempts);
    out_snapshot->presenter_presented = XG_HOST_LOAD(presenter_presented);
    out_snapshot->presenter_empty = XG_HOST_LOAD(presenter_empty);
    out_snapshot->presenter_fence_pending =
        XG_HOST_LOAD(presenter_fence_pending);
    out_snapshot->presenter_fence_failures =
        XG_HOST_LOAD(presenter_fence_failures);
    out_snapshot->presenter_stale = XG_HOST_LOAD(presenter_stale);
    out_snapshot->presenter_compose_failures =
        XG_HOST_LOAD(presenter_compose_failures);
    out_snapshot->presenter_owner_rejections =
        XG_HOST_LOAD(presenter_owner_rejections);
    out_snapshot->presenter_hold_attempts = XG_HOST_LOAD(presenter_hold_attempts);
    out_snapshot->presenter_held = XG_HOST_LOAD(presenter_held);
    out_snapshot->retirement_drains = XG_HOST_LOAD(retirement_drains);
    out_snapshot->retirement_drain_failures =
        XG_HOST_LOAD(retirement_drain_failures);
    out_snapshot->last_worker_result = (XgRenderWorkerResult)
        XG_HOST_LOAD(last_worker_result);
    out_snapshot->last_presenter_result = (XgRenderPresenterResult)
        XG_HOST_LOAD(last_presenter_result);
    if (atomic_load_explicit(&host->final_diagnostics_valid,
                             memory_order_acquire))
        out_snapshot->presentation = host->final_diagnostics;
    else
        xg_render_semantic_presentation_diagnostics(
            &out_snapshot->presentation);
    out_snapshot->presentation.presenter_owner_token = 0u;
    return true;
#undef XG_HOST_LOAD
}

bool xg_render_presentation_host_destroy(XgRenderPresentationHost *host) {
    if (host == NULL || !xg_render_presentation_host_join(host) ||
        !atomic_load_explicit(&host->join_complete, memory_order_acquire) ||
        !host->join_succeeded)
        return false;
    cnd_destroy(&host->join_condition);
    cnd_destroy(&host->worker_condition);
    mtx_destroy(&host->lifecycle_mutex);
    mtx_destroy(&host->mutex);
    free(host);
    return true;
}
