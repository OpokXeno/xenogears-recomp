#include "xg_render_presentation_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#define TEST_ENDPOINT_CAPACITY 32u
#define TEST_GUEST_CAPACITY 16u
#define TEST_WAIT_STEPS 250u
#define TEST_WAIT_STEP_MS 20L

#define COMPILE_FENCE_BASE UINT64_C(0x1000000000000000)
#define COMPLETION_FENCE_BASE UINT64_C(0x2000000000000000)
#define ENDPOINT_BASE UINT64_C(0x5033454e44000000)

typedef struct EndpointRecord {
    XgRenderCompiledEndpoint endpoint;
    uint64_t compile_fence;
    uint64_t completion_fence;
    uint32_t compose_count;
    uint32_t release_count;
    uint32_t compile_discard_count;
    uint32_t completion_discard_count;
} EndpointRecord;

typedef struct FakeTrace {
    mtx_t mutex;
    cnd_t changed;
    XgRenderPresentationHost *host;
    thrd_t guest_threads[TEST_GUEST_CAPACITY];
    uint32_t guest_thread_count;
    thrd_t worker_thread;
    thrd_t presenter_thread;
    bool worker_thread_valid;
    bool presenter_thread_valid;
    EndpointRecord endpoints[TEST_ENDPOINT_CAPACITY];
    uint32_t endpoint_count;
    uint32_t compile_by_source[TEST_ENDPOINT_CAPACITY + 1u];

    atomic_uint compile_count;
    atomic_uint compose_count;
    atomic_uint swap_count;
    atomic_uint release_count;
    atomic_uint worker_fence_count;
    atomic_uint completion_fence_count;
    atomic_uint discard_count;
    atomic_uint guest_backend_calls;
    atomic_uint guest_driver_waits;
    atomic_uint guest_publications_active;
    atomic_uint abi_validations;
    atomic_uint compile_waiters;
    atomic_bool block_compile;
    atomic_bool fail_next_compile;
    atomic_bool compose_ok;
    atomic_bool reenter_compose_pump;
    atomic_bool reentrant_pump_result;
    atomic_int compile_status;
    atomic_int completion_status;
} FakeTrace;

typedef struct TestHost {
    FakeTrace trace;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    XgRenderPresentationHost *host;
    uint64_t next_boundary;
} TestHost;

typedef struct PublisherGroup PublisherGroup;

typedef struct PublisherContext {
    PublisherGroup *group;
    uint32_t turn;
    bool published;
} PublisherContext;

struct PublisherGroup {
    TestHost *test;
    mtx_t mutex;
    cnd_t changed;
    uint32_t count;
    uint32_t ready;
    uint32_t turn;
    bool start;
    PublisherContext contexts[TEST_GUEST_CAPACITY];
    thrd_t threads[TEST_GUEST_CAPACITY];
};

typedef struct JoinContext {
    XgRenderPresentationHost *host;
    FakeTrace *trace;
    atomic_uint *completed;
    bool result;
} JoinContext;

typedef struct ShutdownPublisherContext {
    TestHost *test;
    atomic_uint stage;
    XgRenderTimelineResult publish_result;
    XgRenderTimelineResult boundary_result;
    XgRenderSourceCommitResult retire_result;
} ShutdownPublisherContext;

static struct timespec deadline_after_milliseconds(long milliseconds) {
    struct timespec deadline = {0};
    assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_sec += milliseconds / 1000L;
    deadline.tv_nsec += (milliseconds % 1000L) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void trace_signal(FakeTrace *trace) {
    assert(mtx_lock(&trace->mutex) == thrd_success);
    assert(cnd_broadcast(&trace->changed) == thrd_success);
    assert(mtx_unlock(&trace->mutex) == thrd_success);
}

static bool trace_is_guest_locked(const FakeTrace *trace, thrd_t thread) {
    uint32_t index;
    for (index = 0u; index < trace->guest_thread_count; ++index) {
        if (thrd_equal(trace->guest_threads[index], thread) != 0) return true;
    }
    return false;
}

static void trace_register_guest(FakeTrace *trace) {
    const thrd_t current = thrd_current();
    uint32_t index;

    assert(mtx_lock(&trace->mutex) == thrd_success);
    for (index = 0u; index < trace->guest_thread_count; ++index) {
        if (thrd_equal(trace->guest_threads[index], current) != 0) break;
    }
    if (index == trace->guest_thread_count) {
        assert(trace->guest_thread_count < TEST_GUEST_CAPACITY);
        trace->guest_threads[trace->guest_thread_count++] = current;
    }
    assert(mtx_unlock(&trace->mutex) == thrd_success);
}

/* All driver-facing spies pass here, including the two discard/wait spies. */
static void backend_enter(FakeTrace *trace, bool presenter_owned,
                          bool driver_wait) {
    const thrd_t current = thrd_current();

    assert(mtx_lock(&trace->mutex) == thrd_success);
    if (trace_is_guest_locked(trace, current) &&
        atomic_load_explicit(&trace->guest_publications_active,
                             memory_order_acquire) != 0u) {
        (void)atomic_fetch_add_explicit(&trace->guest_backend_calls, 1u,
                                        memory_order_relaxed);
        if (driver_wait) {
            (void)atomic_fetch_add_explicit(&trace->guest_driver_waits, 1u,
                                            memory_order_relaxed);
        }
    }
    if (presenter_owned) {
        if (!trace->presenter_thread_valid) {
            trace->presenter_thread = current;
            trace->presenter_thread_valid = true;
        } else {
            assert(thrd_equal(trace->presenter_thread, current) != 0);
        }
    }
    assert(mtx_unlock(&trace->mutex) == thrd_success);
}

static void wait_atomic_at_least(FakeTrace *trace, atomic_uint *value,
                                 unsigned int expected) {
    uint32_t step;

    for (step = 0u; step < TEST_WAIT_STEPS; ++step) {
        struct timespec deadline;
        if (atomic_load_explicit(value, memory_order_acquire) >= expected)
            return;
        deadline = deadline_after_milliseconds(TEST_WAIT_STEP_MS);
        assert(mtx_lock(&trace->mutex) == thrd_success);
        if (atomic_load_explicit(value, memory_order_acquire) < expected) {
            const int result = cnd_timedwait(&trace->changed, &trace->mutex,
                                             &deadline);
            assert(result == thrd_success || result == thrd_timedout);
        }
        assert(mtx_unlock(&trace->mutex) == thrd_success);
    }
    assert(atomic_load_explicit(value, memory_order_acquire) >= expected);
}

static void pump_until_atomic(TestHost *test, atomic_uint *value,
                              unsigned int expected) {
    uint32_t step;

    for (step = 0u; step < TEST_WAIT_STEPS; ++step) {
        struct timespec delay = {0, TEST_WAIT_STEP_MS * 1000000L};
        if (atomic_load_explicit(value, memory_order_acquire) >= expected)
            return;
        assert(xg_render_presentation_host_pump(test->host));
        if (atomic_load_explicit(value, memory_order_acquire) >= expected)
            return;
        (void)thrd_sleep(&delay, NULL);
    }
    assert(atomic_load_explicit(value, memory_order_acquire) >= expected);
}

static uint32_t endpoint_index_from_fence(uint64_t fence, uint64_t base) {
    const uint64_t value = fence - base;
    assert(fence > base && value <= TEST_ENDPOINT_CAPACITY);
    return (uint32_t)value - 1u;
}

static bool fake_compile(XgRenderSourceCommitHandle commit,
                         XgRenderCompiledEndpoint *out_endpoint,
                         uint64_t *out_compile_fence, void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    XgRenderSourceCommitHeader header;
    EndpointRecord *record;
    uint32_t index;
    bool result;

    backend_enter(trace, false, false);
    assert(xg_render_source_commit_header_copy(commit, &header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(header.identity.presentation_epoch != 0u);
    assert(header.identity.source_sequence != 0u);
    assert(header.display.width == 320u && header.display.height == 240u);
    assert(header.digest != 0u);

    assert(mtx_lock(&trace->mutex) == thrd_success);
    if (!trace->worker_thread_valid) {
        trace->worker_thread = thrd_current();
        trace->worker_thread_valid = true;
    } else {
        assert(thrd_equal(trace->worker_thread, thrd_current()) != 0);
    }
    assert(header.identity.source_sequence <= TEST_ENDPOINT_CAPACITY);
    assert(trace->compile_by_source[header.identity.source_sequence] == 0u);
    trace->compile_by_source[header.identity.source_sequence] = 1u;
    assert(trace->endpoint_count < TEST_ENDPOINT_CAPACITY);
    index = trace->endpoint_count++;
    record = &trace->endpoints[index];
    record->endpoint = (XgRenderCompiledEndpoint){
        .opaque_handle = ENDPOINT_BASE + index + 1u,
        .identity = header.identity,
        .digest = header.digest,
        .width = header.display.width,
        .height = header.display.height,
        .format = 1u,
        .backend_generation = index + 1u,
    };
    record->compile_fence = COMPILE_FENCE_BASE + index + 1u;
    *out_endpoint = record->endpoint;
    *out_compile_fence = record->compile_fence;
    (void)atomic_fetch_add_explicit(&trace->compile_count, 1u,
                                    memory_order_release);
    (void)atomic_fetch_add_explicit(&trace->abi_validations, 1u,
                                    memory_order_release);
    assert(cnd_broadcast(&trace->changed) == thrd_success);
    if (atomic_load_explicit(&trace->block_compile, memory_order_acquire)) {
        (void)atomic_fetch_add_explicit(&trace->compile_waiters, 1u,
                                        memory_order_release);
        assert(cnd_broadcast(&trace->changed) == thrd_success);
        while (atomic_load_explicit(&trace->block_compile,
                                    memory_order_acquire)) {
            assert(cnd_wait(&trace->changed, &trace->mutex) == thrd_success);
        }
    }
    result = !atomic_exchange_explicit(&trace->fail_next_compile, false,
                                       memory_order_acq_rel);
    assert(mtx_unlock(&trace->mutex) == thrd_success);
    return result;
}

/* Compile fences are checked by the presenter, despite using worker data. */
static XgRenderFenceStatus fake_compile_fence_status(uint64_t fence,
                                                      void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    backend_enter(trace, true, false);
    (void)endpoint_index_from_fence(fence, COMPILE_FENCE_BASE);
    (void)atomic_fetch_add_explicit(&trace->worker_fence_count, 1u,
                                    memory_order_release);
    trace_signal(trace);
    return (XgRenderFenceStatus)atomic_load_explicit(&trace->compile_status,
                                                     memory_order_acquire);
}

static XgRenderFenceStatus fake_completion_fence_status(uint64_t fence,
                                                         void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    backend_enter(trace, true, false);
    (void)endpoint_index_from_fence(fence, COMPLETION_FENCE_BASE);
    (void)atomic_fetch_add_explicit(&trace->completion_fence_count, 1u,
                                    memory_order_release);
    trace_signal(trace);
    return (XgRenderFenceStatus)atomic_load_explicit(
        &trace->completion_status, memory_order_acquire);
}

static bool fake_compose(const XgRenderCompiledEndpoint *endpoint,
                         uint64_t alpha_numerator,
                         uint64_t alpha_denominator,
                         uint64_t *out_completion_fence, void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    EndpointRecord *record;
    uint32_t index;

    backend_enter(trace, true, false);
    if (atomic_exchange_explicit(&trace->reenter_compose_pump, false,
                                 memory_order_acq_rel)) {
        atomic_store_explicit(
            &trace->reentrant_pump_result,
            xg_render_presentation_host_pump(trace->host),
            memory_order_release);
    }
    assert(endpoint != NULL && endpoint->opaque_handle > ENDPOINT_BASE);
    index = (uint32_t)(endpoint->opaque_handle - ENDPOINT_BASE) - 1u;
    assert(index < TEST_ENDPOINT_CAPACITY);
    assert(mtx_lock(&trace->mutex) == thrd_success);
    assert(index < trace->endpoint_count);
    record = &trace->endpoints[index];
    assert(record->endpoint.opaque_handle == endpoint->opaque_handle);
    assert(record->endpoint.digest == endpoint->digest);
    assert(record->endpoint.backend_generation == endpoint->backend_generation);
    assert(record->release_count == 0u && record->compose_count == 0u);
    assert(alpha_numerator == 1u && alpha_denominator == 1u);
    record->compose_count++;
    record->completion_fence = COMPLETION_FENCE_BASE + index + 1u;
    *out_completion_fence = record->completion_fence;
    (void)atomic_fetch_add_explicit(&trace->compose_count, 1u,
                                    memory_order_release);
    assert(cnd_broadcast(&trace->changed) == thrd_success);
    assert(mtx_unlock(&trace->mutex) == thrd_success);
    return atomic_load_explicit(&trace->compose_ok, memory_order_acquire);
}

/* Synthetic stand-in: this call site is reached only from fake_swap. */
static void SDL_GL_SwapWindow(void *window) {
    FakeTrace *trace = (FakeTrace *)window;
    backend_enter(trace, true, false);
    (void)atomic_fetch_add_explicit(&trace->swap_count, 1u,
                                    memory_order_release);
    trace_signal(trace);
}

static void fake_swap(void *user_data) {
    SDL_GL_SwapWindow(user_data);
}

static void record_discard(FakeTrace *trace, uint64_t fence,
                           bool presenter_owned) {
    EndpointRecord *record;
    uint32_t index;

    backend_enter(trace, presenter_owned, true);
    assert(mtx_lock(&trace->mutex) == thrd_success);
    if (fence > COMPILE_FENCE_BASE && fence <=
            COMPILE_FENCE_BASE + TEST_ENDPOINT_CAPACITY) {
        index = endpoint_index_from_fence(fence, COMPILE_FENCE_BASE);
        assert(index < trace->endpoint_count);
        record = &trace->endpoints[index];
        assert(record->compile_fence == fence);
        assert(record->compile_discard_count++ == 0u);
    } else {
        index = endpoint_index_from_fence(fence, COMPLETION_FENCE_BASE);
        assert(index < trace->endpoint_count);
        record = &trace->endpoints[index];
        assert(record->completion_fence == fence);
        assert(record->completion_discard_count++ == 0u);
    }
    (void)atomic_fetch_add_explicit(&trace->discard_count, 1u,
                                    memory_order_release);
    assert(cnd_broadcast(&trace->changed) == thrd_success);
    assert(mtx_unlock(&trace->mutex) == thrd_success);
}

static void fake_worker_discard(uint64_t fence, void *user_data) {
    record_discard((FakeTrace *)user_data, fence, false);
}

static void fake_presenter_discard(uint64_t fence, void *user_data) {
    record_discard((FakeTrace *)user_data, fence, true);
}

static void fake_release_endpoint(const XgRenderCompiledEndpoint *endpoint,
                                  void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    EndpointRecord *record;
    uint32_t index;

    backend_enter(trace, false, false);
    assert(endpoint != NULL && endpoint->opaque_handle > ENDPOINT_BASE);
    index = (uint32_t)(endpoint->opaque_handle - ENDPOINT_BASE) - 1u;
    assert(index < TEST_ENDPOINT_CAPACITY);
    assert(mtx_lock(&trace->mutex) == thrd_success);
    assert(index < trace->endpoint_count);
    record = &trace->endpoints[index];
    assert(record->endpoint.opaque_handle == endpoint->opaque_handle);
    assert(record->endpoint.digest == endpoint->digest);
    assert(record->release_count++ == 0u);
    (void)atomic_fetch_add_explicit(&trace->release_count, 1u,
                                    memory_order_release);
    assert(cnd_broadcast(&trace->changed) == thrd_success);
    assert(mtx_unlock(&trace->mutex) == thrd_success);
}

static void trace_init(FakeTrace *trace) {
    memset(trace, 0, sizeof(*trace));
    assert(mtx_init(&trace->mutex, mtx_plain) == thrd_success);
    assert(cnd_init(&trace->changed) == thrd_success);
    atomic_init(&trace->compile_count, 0u);
    atomic_init(&trace->compose_count, 0u);
    atomic_init(&trace->swap_count, 0u);
    atomic_init(&trace->release_count, 0u);
    atomic_init(&trace->worker_fence_count, 0u);
    atomic_init(&trace->completion_fence_count, 0u);
    atomic_init(&trace->discard_count, 0u);
    atomic_init(&trace->guest_backend_calls, 0u);
    atomic_init(&trace->guest_driver_waits, 0u);
    atomic_init(&trace->guest_publications_active, 0u);
    atomic_init(&trace->abi_validations, 0u);
    atomic_init(&trace->compile_waiters, 0u);
    atomic_init(&trace->block_compile, false);
    atomic_init(&trace->fail_next_compile, false);
    atomic_init(&trace->compose_ok, true);
    atomic_init(&trace->reenter_compose_pump, false);
    atomic_init(&trace->reentrant_pump_result, true);
    atomic_init(&trace->compile_status, XG_RENDER_FENCE_READY);
    atomic_init(&trace->completion_status, XG_RENDER_FENCE_READY);
}

static void trace_destroy(FakeTrace *trace) {
    cnd_destroy(&trace->changed);
    mtx_destroy(&trace->mutex);
}

static void test_host_start(TestHost *test) {
    XgRenderPresentationHostSnapshot snapshot;

    memset(test, 0, sizeof(*test));
    xg_render_semantic_presentation_reset();
    trace_init(&test->trace);
    test->worker = (XgRenderWorkerServices){
        .compile = fake_compile,
        .fence_status = fake_compile_fence_status,
        .discard_fence = fake_worker_discard,
        .release_endpoint = fake_release_endpoint,
        .user_data = &test->trace,
    };
    test->presenter = (XgRenderPresenterServices){
        .fence_status = fake_completion_fence_status,
        .compose = fake_compose,
        .swap_window = fake_swap,
        .discard_fence = fake_presenter_discard,
        .user_data = &test->trace,
        /* The host must replace, not trust, this deliberately foreign token. */
        .owner_token = UINT64_C(0xbad0bad0bad0bad0),
    };
    test->next_boundary = 1u;
    test->host = xg_render_presentation_host_start(&test->worker,
                                                    &test->presenter,
                                                    1000000u);
    assert(test->host != NULL);
    test->trace.host = test->host;
    assert(xg_render_presentation_host_snapshot(test->host, &snapshot));
    assert(snapshot.state == XG_RENDER_PRESENTATION_HOST_RUNNING);
    assert(snapshot.presenter_owner_identity_valid);
    assert(!snapshot.presenter_running);
    assert(snapshot.presentation.publication_open);
    assert(snapshot.presentation.lifecycle_open_count == 1u);
    assert(snapshot.presentation.presenter_owner_token == 0u);
}

static XgRenderSourceCommitHandle make_commit(uint64_t boundary) {
    const XgSemanticSceneIdentity scene = {
        .disc_id = 1u,
        .executable_identity = 2u,
        .primary_overlay_identity = 3u,
        .authored_scene_id = 4u,
        .module = XG_SEMANTIC_MODULE_FIELD,
    };
    const XgSemanticDisplayState display = {
        .width = 320u,
        .height = 240u,
        .aspect_num = 4u,
        .aspect_den = 3u,
    };
    const XgSemanticPassRecord pass = {
        .pass_id = 1u,
        .store = true,
    };
    XgPresentationIdentity identity;
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;

    assert(xg_render_timeline_source_boundary(boundary, boundary * 100u) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_timeline_next_identity(7u, &identity) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_source_commit_begin(&identity, &scene, &display, 1u,
                                         false, true, &builder) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_pass(builder, &pass) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    return commit;
}

/* No condition variable or backend operation occurs in this active region. */
static bool publish_one(TestHost *test) {
    XgRenderSourceCommitHandle commit;
    uint64_t boundary;

    trace_register_guest(&test->trace);
    (void)atomic_fetch_add_explicit(&test->trace.guest_publications_active, 1u,
                                    memory_order_release);
    boundary = test->next_boundary++;
    commit = make_commit(boundary);
    if (xg_render_source_queue_publish(commit) != XG_RENDER_TIMELINE_OK) {
        (void)atomic_fetch_sub_explicit(
            &test->trace.guest_publications_active, 1u, memory_order_release);
        return false;
    }
    xg_render_presentation_host_notify(test->host);
    (void)atomic_fetch_sub_explicit(&test->trace.guest_publications_active, 1u,
                                    memory_order_release);
    return true;
}

static int publisher_main(void *user_data) {
    PublisherContext *context = (PublisherContext *)user_data;
    PublisherGroup *group = context->group;

    trace_register_guest(&group->test->trace);
    assert(mtx_lock(&group->mutex) == thrd_success);
    group->ready++;
    assert(cnd_broadcast(&group->changed) == thrd_success);
    while (!group->start || group->turn != context->turn)
        assert(cnd_wait(&group->changed, &group->mutex) == thrd_success);
    assert(mtx_unlock(&group->mutex) == thrd_success);

    context->published = publish_one(group->test);

    assert(mtx_lock(&group->mutex) == thrd_success);
    group->turn++;
    assert(cnd_broadcast(&group->changed) == thrd_success);
    assert(mtx_unlock(&group->mutex) == thrd_success);
    return context->published ? 0 : 1;
}

static void publish_concurrently(TestHost *test, uint32_t count) {
    PublisherGroup group;
    uint32_t index;

    assert(count != 0u && count <= TEST_GUEST_CAPACITY);
    memset(&group, 0, sizeof(group));
    group.test = test;
    group.count = count;
    assert(mtx_init(&group.mutex, mtx_plain) == thrd_success);
    assert(cnd_init(&group.changed) == thrd_success);
    for (index = 0u; index < count; ++index) {
        group.contexts[index].group = &group;
        group.contexts[index].turn = index;
        assert(thrd_create(&group.threads[index], publisher_main,
                           &group.contexts[index]) == thrd_success);
    }
    assert(mtx_lock(&group.mutex) == thrd_success);
    while (group.ready != count)
        assert(cnd_wait(&group.changed, &group.mutex) == thrd_success);
    group.start = true;
    assert(cnd_broadcast(&group.changed) == thrd_success);
    assert(mtx_unlock(&group.mutex) == thrd_success);
    for (index = 0u; index < count; ++index) {
        int result = -1;
        assert(thrd_join(group.threads[index], &result) == thrd_success);
        assert(result == 0 && group.contexts[index].published);
    }
    cnd_destroy(&group.changed);
    mtx_destroy(&group.mutex);
}

static void unblock_compile(FakeTrace *trace) {
    assert(mtx_lock(&trace->mutex) == thrd_success);
    atomic_store_explicit(&trace->block_compile, false, memory_order_release);
    assert(cnd_broadcast(&trace->changed) == thrd_success);
    assert(mtx_unlock(&trace->mutex) == thrd_success);
}

static void wait_host_capacity(TestHost *test) {
    uint32_t step;
    XgRenderPresentationHostSnapshot snapshot;

    for (step = 0u; step < TEST_WAIT_STEPS; ++step) {
        struct timespec deadline;
        assert(xg_render_presentation_host_snapshot(test->host, &snapshot));
        if (snapshot.worker_capacity_exceeded != 0u) return;
        deadline = deadline_after_milliseconds(TEST_WAIT_STEP_MS);
        assert(mtx_lock(&test->trace.mutex) == thrd_success);
        if (xg_render_presentation_host_snapshot(test->host, &snapshot) &&
            snapshot.worker_capacity_exceeded == 0u) {
            const int result = cnd_timedwait(&test->trace.changed,
                                             &test->trace.mutex, &deadline);
            assert(result == thrd_success || result == thrd_timedout);
        }
        assert(mtx_unlock(&test->trace.mutex) == thrd_success);
    }
    assert(xg_render_presentation_host_snapshot(test->host, &snapshot));
    assert(snapshot.worker_capacity_exceeded != 0u);
}

typedef enum HostCounter {
    HOST_COUNTER_WORKER_STALE = 0,
    HOST_COUNTER_COMPILE_FAILURE,
    HOST_COUNTER_COMPLETION_FAILURE,
} HostCounter;

static uint64_t host_counter_value(
        const XgRenderPresentationHostSnapshot *snapshot,
        HostCounter counter) {
    switch (counter) {
    case HOST_COUNTER_WORKER_STALE:
        return snapshot->worker_stale;
    case HOST_COUNTER_COMPILE_FAILURE:
        return snapshot->worker_compile_failures;
    case HOST_COUNTER_COMPLETION_FAILURE:
        return snapshot->presenter_fence_failures;
    }
    assert(false);
    return 0u;
}

static XgRenderPresentationHostSnapshot wait_host_counter(TestHost *test,
                                                           HostCounter counter) {
    XgRenderPresentationHostSnapshot snapshot;
    uint32_t step;

    for (step = 0u; step < TEST_WAIT_STEPS; ++step) {
        struct timespec deadline;
        assert(xg_render_presentation_host_snapshot(test->host, &snapshot));
        if (host_counter_value(&snapshot, counter) != 0u) return snapshot;
        assert(xg_render_presentation_host_pump(test->host));
        deadline = deadline_after_milliseconds(TEST_WAIT_STEP_MS);
        assert(mtx_lock(&test->trace.mutex) == thrd_success);
        assert(xg_render_presentation_host_snapshot(test->host, &snapshot));
        if (host_counter_value(&snapshot, counter) == 0u) {
            const int result = cnd_timedwait(&test->trace.changed,
                                             &test->trace.mutex, &deadline);
            assert(result == thrd_success || result == thrd_timedout);
        }
        assert(mtx_unlock(&test->trace.mutex) == thrd_success);
    }
    assert(xg_render_presentation_host_snapshot(test->host, &snapshot));
    assert(host_counter_value(&snapshot, counter) != 0u);
    return snapshot;
}

static void assert_no_guest_backend_work(const FakeTrace *trace) {
    assert(atomic_load_explicit(&trace->guest_publications_active,
                                memory_order_acquire) == 0u);
    assert(atomic_load_explicit(&trace->guest_backend_calls,
                                memory_order_acquire) == 0u);
    assert(atomic_load_explicit(&trace->guest_driver_waits,
                                memory_order_acquire) == 0u);
}

static void assert_exact_retirement(FakeTrace *trace) {
    uint32_t index;

    assert(mtx_lock(&trace->mutex) == thrd_success);
    assert(trace->endpoint_count == atomic_load_explicit(
               &trace->compile_count, memory_order_acquire));
    assert(trace->endpoint_count == atomic_load_explicit(
               &trace->release_count, memory_order_acquire));
    for (index = 0u; index < trace->endpoint_count; ++index) {
        const EndpointRecord *record = &trace->endpoints[index];
        uint32_t other;
        assert(record->release_count == 1u);
        assert(record->compile_discard_count == 1u);
        assert(record->compose_count <= 1u);
        assert(record->completion_discard_count == record->compose_count);
        for (other = index + 1u; other < trace->endpoint_count; ++other) {
            assert(record->endpoint.opaque_handle !=
                   trace->endpoints[other].endpoint.opaque_handle);
            assert(record->endpoint.identity.source_sequence !=
                   trace->endpoints[other].endpoint.identity.source_sequence);
        }
    }
    assert(mtx_unlock(&trace->mutex) == thrd_success);
}

static XgRenderPresentationHostSnapshot stop_host(TestHost *test) {
    XgRenderPresentationHostSnapshot snapshot;

    assert(xg_render_presentation_host_join(test->host));
    assert(xg_render_presentation_host_snapshot(test->host, &snapshot));
    assert(snapshot.state == XG_RENDER_PRESENTATION_HOST_JOINED);
    assert(snapshot.shutdown_requested && snapshot.shutdown_complete);
    assert(snapshot.join_complete);
    assert(!snapshot.worker_running && !snapshot.presenter_running);
    assert(!snapshot.presenter_owner_identity_valid);
    assert(!snapshot.presentation.publication_open);
    assert(!snapshot.presentation.source_pending);
    assert(!snapshot.presentation.batch_pending);
    assert(snapshot.presentation.source_queue_depth == 0u);
    assert(snapshot.presentation.batch_queue_depth == 0u);
    assert(snapshot.presentation.retirement_queue_depth == 0u);
    assert_exact_retirement(&test->trace);
    assert_no_guest_backend_work(&test->trace);
    assert(xg_render_presentation_host_destroy(test->host));
    test->host = NULL;
    trace_destroy(&test->trace);
    return snapshot;
}

static void test_worker_presenter_runtime_load_bearing(void) {
    TestHost test;
    XgRenderPresentationHostSnapshot snapshot;

    test_host_start(&test);
    assert(publish_one(&test));
    wait_atomic_at_least(&test.trace, &test.trace.compile_count, 1u);
    assert(atomic_load_explicit(&test.trace.compose_count,
                                memory_order_acquire) == 0u);
    assert(atomic_load_explicit(&test.trace.swap_count,
                                memory_order_acquire) == 0u);
    pump_until_atomic(&test, &test.trace.swap_count, 1u);
    pump_until_atomic(&test, &test.trace.release_count, 1u);
    snapshot = stop_host(&test);
    assert(snapshot.worker_compiled == 1u);
    assert(snapshot.presenter_presented == 1u);
    assert(snapshot.notifications == 1u);
}

static void test_batch_endpoint_abi(void) {
    TestHost test;

    test_host_start(&test);
    assert(publish_one(&test));
    wait_atomic_at_least(&test.trace, &test.trace.abi_validations, 1u);
    pump_until_atomic(&test, &test.trace.release_count, 1u);
    assert(atomic_load_explicit(&test.trace.compose_count,
                                memory_order_acquire) == 1u);
    (void)stop_host(&test);
}

static void test_queue_saturation_and_coalescing(void) {
    TestHost saturated;
    TestHost coalesced;
    XgRenderPresentationHostSnapshot snapshot;
    XgRenderPresentationHostSnapshot blocked_snapshot;
    XgRenderPresentationDiagnostics diagnostics;
    unsigned int fence_checks;
    uint32_t index;

    /* Four pending completion fences occupy every bounded batch slot. */
    test_host_start(&saturated);
    atomic_store_explicit(&saturated.trace.completion_status,
                          XG_RENDER_FENCE_PENDING, memory_order_release);
    for (index = 1u; index <= XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        assert(publish_one(&saturated));
        pump_until_atomic(&saturated, &saturated.trace.swap_count, index);
    }
    assert(publish_one(&saturated));
    wait_host_capacity(&saturated);
    assert(xg_render_presentation_host_snapshot(saturated.host,
                                                &blocked_snapshot));
    assert(blocked_snapshot.worker_capacity_exceeded == 1u);
    assert(blocked_snapshot.presentation.worker_backpressure == 1u);
    fence_checks = atomic_load_explicit(
        &saturated.trace.completion_fence_count, memory_order_acquire);
    pump_until_atomic(&saturated, &saturated.trace.completion_fence_count,
                      fence_checks + 8u);
    assert(xg_render_presentation_host_snapshot(saturated.host, &snapshot));
    assert(snapshot.worker_attempts == blocked_snapshot.worker_attempts);
    assert(snapshot.worker_capacity_exceeded ==
           blocked_snapshot.worker_capacity_exceeded);
    assert(snapshot.presentation.worker_backpressure ==
           blocked_snapshot.presentation.worker_backpressure);
    atomic_store_explicit(&saturated.trace.completion_status,
                          XG_RENDER_FENCE_READY, memory_order_release);
    xg_render_presentation_host_notify(saturated.host);
    pump_until_atomic(&saturated, &saturated.trace.compile_count,
                      XG_RENDER_PRESENTATION_BATCH_CAPACITY + 1u);
    pump_until_atomic(&saturated, &saturated.trace.swap_count,
                      XG_RENDER_PRESENTATION_BATCH_CAPACITY + 1u);
    assert(xg_render_presentation_host_snapshot(saturated.host, &snapshot));
    assert(snapshot.worker_capacity_exceeded >=
           blocked_snapshot.worker_capacity_exceeded);
    assert(XG_RENDER_WORKER_CAPACITY_EXCEEDED != XG_RENDER_WORKER_OK);
    assert(snapshot.presentation.batch_queue_capacity ==
           XG_RENDER_PRESENTATION_BATCH_CAPACITY);
    (void)stop_host(&saturated);

    /* While source 1 compiles, sources 2 and 3 are deterministically replaced
       by source 4 in the one-entry source mailbox. */
    test_host_start(&coalesced);
    atomic_store_explicit(&coalesced.trace.block_compile, true,
                          memory_order_release);
    assert(publish_one(&coalesced));
    wait_atomic_at_least(&coalesced.trace, &coalesced.trace.compile_waiters,
                         1u);
    publish_concurrently(&coalesced, 3u);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.published_commits == 4u);
    assert(diagnostics.source_coalesces == 2u);
    assert(diagnostics.superseded_endpoints == 2u);
    assert(atomic_load_explicit(&coalesced.trace.compile_count,
                                memory_order_acquire) == 1u);
    unblock_compile(&coalesced.trace);
    wait_atomic_at_least(&coalesced.trace, &coalesced.trace.compile_count, 2u);
    pump_until_atomic(&coalesced, &coalesced.trace.release_count, 2u);
    assert(coalesced.trace.compile_by_source[1u] == 1u);
    assert(coalesced.trace.compile_by_source[4u] == 1u);
    assert(coalesced.trace.compile_by_source[2u] == 0u);
    assert(coalesced.trace.compile_by_source[3u] == 0u);
    snapshot = stop_host(&coalesced);
    assert(snapshot.worker_compiled == 2u);
    assert(snapshot.notifications == 4u);
}

static void test_stale_epochs(void) {
    TestHost test;
    XgRenderPresentationHostSnapshot snapshot;
    XgRenderPresentationDiagnostics before;
    uint64_t epoch;

    test_host_start(&test);
    atomic_store_explicit(&test.trace.block_compile, true,
                          memory_order_release);
    assert(publish_one(&test));
    wait_atomic_at_least(&test.trace, &test.trace.compile_waiters, 1u);
    xg_render_semantic_presentation_diagnostics(&before);
    epoch = xg_render_presentation_host_invalidate_or_wake(
        test.host, XG_RENDER_TIMELINE_RESTORE);
    assert(epoch == before.presentation_epoch + 1u);
    unblock_compile(&test.trace);
    wait_atomic_at_least(&test.trace, &test.trace.release_count, 1u);
    snapshot = wait_host_counter(&test, HOST_COUNTER_WORKER_STALE);
    assert(snapshot.worker_stale == 1u);
    assert(XG_RENDER_PRESENTER_STALE != XG_RENDER_PRESENTER_PRESENTED);
    assert(atomic_load_explicit(&test.trace.compose_count,
                                memory_order_acquire) == 0u);
    assert(atomic_load_explicit(&test.trace.swap_count,
                                memory_order_acquire) == 0u);
    snapshot = stop_host(&test);
    assert(snapshot.presenter_stale == 0u);
}

static void test_fence_failure(void) {
    TestHost test;
    XgRenderPresentationHostSnapshot snapshot;

    test_host_start(&test);
    atomic_store_explicit(&test.trace.fail_next_compile, true,
                          memory_order_release);
    assert(publish_one(&test));
    wait_atomic_at_least(&test.trace, &test.trace.release_count, 1u);
    snapshot = wait_host_counter(&test, HOST_COUNTER_COMPILE_FAILURE);

    atomic_store_explicit(&test.trace.completion_status,
                          XG_RENDER_FENCE_FAILED, memory_order_release);
    assert(publish_one(&test));
    pump_until_atomic(&test, &test.trace.swap_count, 1u);
    pump_until_atomic(&test, &test.trace.release_count, 2u);
    snapshot = wait_host_counter(&test, HOST_COUNTER_COMPLETION_FAILURE);
    snapshot = stop_host(&test);
    assert(snapshot.worker_compile_failures == 1u);
    assert(snapshot.presenter_fence_failures == 1u);
    assert(XG_RENDER_PRESENTER_FENCE_FAILED != XG_RENDER_PRESENTER_PRESENTED);
    assert(snapshot.presenter_presented == 1u);
}

static int join_main(void *user_data) {
    JoinContext *context = (JoinContext *)user_data;
    context->result = xg_render_presentation_host_join(context->host);
    (void)atomic_fetch_add_explicit(context->completed, 1u,
                                    memory_order_release);
    trace_signal(context->trace);
    return context->result ? 0 : 1;
}

static int destroy_main(void *user_data) {
    return xg_render_presentation_host_destroy(
               (XgRenderPresentationHost *)user_data)
        ? 0 : 1;
}

static int shutdown_publisher_main(void *user_data) {
    ShutdownPublisherContext *context =
        (ShutdownPublisherContext *)user_data;
    TestHost *test = context->test;
    XgRenderSourceCommitHandle commit;
    uint64_t boundary;

    trace_register_guest(&test->trace);
    (void)atomic_fetch_add_explicit(&test->trace.guest_publications_active, 1u,
                                    memory_order_release);
    boundary = test->next_boundary++;
    commit = make_commit(boundary);

    atomic_store_explicit(&context->stage, 1u, memory_order_release);
    trace_signal(&test->trace);
    assert(mtx_lock(&test->trace.mutex) == thrd_success);
    while (atomic_load_explicit(&context->stage, memory_order_acquire) < 2u)
        assert(cnd_wait(&test->trace.changed, &test->trace.mutex) ==
               thrd_success);
    assert(mtx_unlock(&test->trace.mutex) == thrd_success);

    context->publish_result = xg_render_source_queue_publish(commit);
    xg_render_presentation_host_notify(test->host);
    context->boundary_result = xg_render_timeline_source_boundary(
        boundary + 1u, (boundary + 1u) * 100u);
    context->retire_result = xg_render_source_commit_retire(commit);
    (void)atomic_fetch_sub_explicit(&test->trace.guest_publications_active, 1u,
                                    memory_order_release);
    atomic_store_explicit(&context->stage, 3u, memory_order_release);
    trace_signal(&test->trace);
    return 0;
}

static void test_shutdown_closes_concurrent_publication(void) {
    TestHost test;
    ShutdownPublisherContext publisher;
    JoinContext join;
    XgRenderPresentationHostSnapshot snapshot;
    thrd_t publisher_thread;
    thrd_t join_thread;
    atomic_uint completed;
    int result = -1;

    test_host_start(&test);
    memset(&publisher, 0, sizeof(publisher));
    publisher.test = &test;
    atomic_init(&publisher.stage, 0u);
    assert(thrd_create(&publisher_thread, shutdown_publisher_main,
                       &publisher) == thrd_success);
    wait_atomic_at_least(&test.trace, &publisher.stage, 1u);

    assert(xg_render_presentation_host_snapshot(test.host, &snapshot));
    assert(snapshot.presentation.publication_open);
    assert(snapshot.notifications == 0u);
    xg_render_presentation_host_shutdown(test.host);
    assert(xg_render_presentation_host_snapshot(test.host, &snapshot));
    assert(snapshot.state == XG_RENDER_PRESENTATION_HOST_SHUTTING_DOWN);
    assert(snapshot.shutdown_requested && snapshot.shutdown_complete);
    assert(snapshot.presenter_owner_identity_valid);
    assert(!snapshot.presentation.publication_open);
    assert(snapshot.presentation.lifecycle_close_count == 1u);

    atomic_init(&completed, 0u);
    join = (JoinContext){
        .host = test.host,
        .trace = &test.trace,
        .completed = &completed,
    };
    assert(thrd_create(&join_thread, join_main, &join) == thrd_success);
    wait_atomic_at_least(&test.trace, &completed, 1u);
    result = -1;
    assert(thrd_join(join_thread, &result) == thrd_success);
    assert(result == 1 && !join.result);

    assert(mtx_lock(&test.trace.mutex) == thrd_success);
    atomic_store_explicit(&publisher.stage, 2u, memory_order_release);
    assert(cnd_broadcast(&test.trace.changed) == thrd_success);
    assert(mtx_unlock(&test.trace.mutex) == thrd_success);

    wait_atomic_at_least(&test.trace, &publisher.stage, 3u);
    assert(thrd_join(publisher_thread, &result) == thrd_success);
    assert(result == 0);
    assert(publisher.publish_result == XG_RENDER_TIMELINE_PUBLICATION_CLOSED);
    assert(publisher.boundary_result == XG_RENDER_TIMELINE_PUBLICATION_CLOSED);
    assert(publisher.retire_result == XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_presentation_host_join(test.host));

    assert(xg_render_presentation_host_snapshot(test.host, &snapshot));
    assert(snapshot.state == XG_RENDER_PRESENTATION_HOST_JOINED);
    assert(snapshot.shutdown_complete && snapshot.join_complete);
    assert(!snapshot.presenter_owner_identity_valid);
    assert(snapshot.notifications == 0u);
    assert(!snapshot.presentation.publication_open);
    assert(!snapshot.presentation.source_pending);
    assert(!snapshot.presentation.batch_pending);
    assert_exact_retirement(&test.trace);
    assert_no_guest_backend_work(&test.trace);
    assert(xg_render_presentation_host_destroy(test.host));
    test.host = NULL;
    trace_destroy(&test.trace);
}

static void test_retirement(void) {
    TestHost test;
    XgRenderPresentationHostSnapshot snapshot;
    thrd_t destroy_thread;
    int destroy_result = -1;
    uint32_t index;

    test_host_start(&test);
    atomic_store_explicit(&test.trace.completion_status,
                          XG_RENDER_FENCE_PENDING, memory_order_release);
    for (index = 1u; index <= 3u; ++index) {
        assert(publish_one(&test));
        pump_until_atomic(&test, &test.trace.swap_count, index);
    }
    assert(xg_render_presentation_host_join(test.host));
    assert(xg_render_presentation_host_snapshot(test.host, &snapshot));
    assert(snapshot.state == XG_RENDER_PRESENTATION_HOST_JOINED);
    assert(snapshot.shutdown_complete && snapshot.join_complete);
    assert(!snapshot.presenter_owner_identity_valid);
    assert(!snapshot.presentation.publication_open);
    assert(snapshot.presentation.lifecycle_close_count == 1u);
    assert(snapshot.retirement_drains != 0u);
    assert_exact_retirement(&test.trace);
    assert_no_guest_backend_work(&test.trace);
    assert(thrd_create(&destroy_thread, destroy_main, test.host) ==
           thrd_success);
    assert(thrd_join(destroy_thread, &destroy_result) == thrd_success);
    assert(destroy_result == 0);
    test.host = NULL;
    trace_destroy(&test.trace);
}

static int foreign_presenter_main(void *user_data) {
    TestHost *test = (TestHost *)user_data;
    return xg_render_presentation_host_pump(test->host) ? 1 : 0;
}

static void test_single_sdl_gl_swap_owner(void) {
    TestHost test;
    XgRenderPresentationHostSnapshot snapshot;
    thrd_t foreign;
    int result = -1;

    test_host_start(&test);
    assert(publish_one(&test));
    wait_atomic_at_least(&test.trace, &test.trace.compile_count, 1u);
    {
        const struct timespec delay = {0, TEST_WAIT_STEP_MS * 1000000L};
        (void)thrd_sleep(&delay, NULL);
    }
    assert(thrd_create(&foreign, foreign_presenter_main, &test) ==
           thrd_success);
    assert(thrd_join(foreign, &result) == thrd_success);
    assert(result == 0);
    assert(xg_render_presentation_host_snapshot(test.host, &snapshot));
    assert(snapshot.presenter_owner_rejections == 1u);
    assert(atomic_load_explicit(&test.trace.compose_count,
                                memory_order_acquire) == 0u);
    assert(atomic_load_explicit(&test.trace.swap_count,
                                memory_order_acquire) == 0u);
    assert(atomic_load_explicit(&test.trace.worker_fence_count,
                                memory_order_acquire) == 0u);
    atomic_store_explicit(&test.trace.reenter_compose_pump, true,
                          memory_order_release);
    pump_until_atomic(&test, &test.trace.swap_count, 1u);
    pump_until_atomic(&test, &test.trace.release_count, 1u);
    assert(!atomic_load_explicit(&test.trace.reentrant_pump_result,
                                 memory_order_acquire));
    assert(xg_render_presentation_host_snapshot(test.host, &snapshot));
    assert(snapshot.presenter_owner_rejections == 2u);
    assert(mtx_lock(&test.trace.mutex) == thrd_success);
    assert(test.trace.worker_thread_valid && test.trace.presenter_thread_valid);
    assert(thrd_equal(test.trace.worker_thread,
                      test.trace.presenter_thread) == 0);
    assert(mtx_unlock(&test.trace.mutex) == thrd_success);
    (void)stop_host(&test);
}

static void test_native_guest_callback_zero_swap_wait(void) {
    TestHost test;

    test_host_start(&test);
    publish_concurrently(&test, 2u);
    wait_atomic_at_least(&test.trace, &test.trace.compile_count, 1u);
    pump_until_atomic(&test, &test.trace.swap_count, 1u);
    pump_until_atomic(&test, &test.trace.release_count, 1u);
    assert_no_guest_backend_work(&test.trace);
    (void)stop_host(&test);
}

int main(void) {
    test_worker_presenter_runtime_load_bearing();
    test_batch_endpoint_abi();
    test_queue_saturation_and_coalescing();
    test_stale_epochs();
    test_fence_failure();
    test_shutdown_closes_concurrent_publication();
    test_retirement();
    test_single_sdl_gl_swap_owner();
    test_native_guest_callback_zero_swap_wait();
    return 0;
}
