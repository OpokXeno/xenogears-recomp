#include "xg_render_semantic_presentation.h"

#include <assert.h>
#include <string.h>

#define FAKE_CAPACITY 32u
#define PRESENTER_OWNER UINT64_C(0x50524553454e5445)

typedef enum FakeFenceKind {
    FAKE_COMPILE_FENCE = 0,
    FAKE_COMPLETION_FENCE,
} FakeFenceKind;

typedef enum FakeInvalidationHook {
    FAKE_INVALIDATE_NONE = 0,
    FAKE_INVALIDATE_DURING_COMPILE,
    FAKE_INVALIDATE_DURING_FENCE,
    FAKE_INVALIDATE_DURING_COMPOSE,
    FAKE_INVALIDATE_DURING_SWAP,
    FAKE_INVALIDATE_DURING_RETIREMENT,
} FakeInvalidationHook;

typedef struct FakeFenceRecord {
    uint64_t value;
    FakeFenceKind kind;
    uint32_t discard_count;
} FakeFenceRecord;

typedef struct FakeEndpointRecord {
    XgRenderCompiledEndpoint endpoint;
    XgRenderSourceCommitHandle commit;
    uint32_t compose_count;
    uint32_t release_count;
} FakeEndpointRecord;

typedef struct FakeServices {
    FakeEndpointRecord endpoints[FAKE_CAPACITY];
    FakeFenceRecord fences[FAKE_CAPACITY * 2u];
    uint32_t endpoint_count;
    uint32_t fence_count;
    uint32_t compile_count;
    uint32_t compile_fence_status_count;
    uint32_t completion_fence_status_count;
    uint32_t compose_count;
    uint32_t swap_count;
    uint32_t discard_count;
    uint32_t endpoint_release_count;
    XgRenderFenceStatus compile_status;
    XgRenderFenceStatus completion_status;
    FakeInvalidationHook invalidation_hook;
    uint32_t invalidation_count;
    uint64_t invalidated_epoch;
    bool compile_ok;
    bool compose_ok;
    bool zero_completion_fence;
} FakeServices;

static bool identity_equal(const XgPresentationIdentity *left,
                           const XgPresentationIdentity *right) {
    return left->presentation_epoch == right->presentation_epoch &&
        left->source_sequence == right->source_sequence &&
        left->guest_vblank_sequence == right->guest_vblank_sequence &&
        left->guest_cycle == right->guest_cycle &&
        left->scene_generation == right->scene_generation;
}

static void invalidate_at(FakeServices *fake, FakeInvalidationHook hook) {
    if (fake->invalidation_hook != hook) return;
    fake->invalidation_hook = FAKE_INVALIDATE_NONE;
    fake->invalidation_count++;
    fake->invalidated_epoch = xg_render_timeline_invalidate(
        XG_RENDER_TIMELINE_ARTIFACT_CHANGE);
    assert(fake->invalidated_epoch != 0u);
}

static FakeFenceRecord *find_fence(FakeServices *fake, uint64_t value) {
    uint32_t index;
    for (index = 0u; index < fake->fence_count; ++index) {
        if (fake->fences[index].value == value) return &fake->fences[index];
    }
    return NULL;
}

static void add_fence(FakeServices *fake, uint64_t value,
                      FakeFenceKind kind) {
    assert(value != 0u);
    assert(fake->fence_count < FAKE_CAPACITY * 2u);
    assert(find_fence(fake, value) == NULL);
    fake->fences[fake->fence_count++] = (FakeFenceRecord){
        .value = value,
        .kind = kind,
    };
}

static FakeEndpointRecord *find_endpoint(
        FakeServices *fake, const XgRenderCompiledEndpoint *endpoint) {
    uint32_t index;
    for (index = 0u; index < fake->endpoint_count; ++index) {
        if (fake->endpoints[index].endpoint.opaque_handle ==
                endpoint->opaque_handle)
            return &fake->endpoints[index];
    }
    return NULL;
}

static bool fake_compile(XgRenderSourceCommitHandle commit,
                         XgRenderCompiledEndpoint *out_endpoint,
                         uint64_t *out_compile_fence,
                         void *user_data) {
    FakeServices *fake = (FakeServices *)user_data;
    FakeEndpointRecord *record;
    XgRenderSourceCommitHeader header;
    uint32_t index;

    assert(out_endpoint != NULL && out_compile_fence != NULL);
    assert(xg_render_source_commit_header_copy(commit, &header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(fake->endpoint_count < FAKE_CAPACITY);
    index = fake->endpoint_count++;
    record = &fake->endpoints[index];
    record->commit = commit;
    record->endpoint = (XgRenderCompiledEndpoint){
        .opaque_handle = UINT64_C(0x5847455000000000) + index + 1u,
        .identity = header.identity,
        .digest = header.digest,
        .width = header.display.width,
        .height = header.display.height,
        .format = 1u,
        .backend_generation = (uint64_t)index + 1u,
    };
    *out_endpoint = record->endpoint;
    *out_compile_fence = UINT64_C(0xc000000000000000) + index + 1u;
    add_fence(fake, *out_compile_fence, FAKE_COMPILE_FENCE);
    fake->compile_count++;
    invalidate_at(fake, FAKE_INVALIDATE_DURING_COMPILE);
    return fake->compile_ok;
}

static XgRenderFenceStatus fake_fence_status(uint64_t fence,
                                              void *user_data) {
    FakeServices *fake = (FakeServices *)user_data;
    FakeFenceRecord *record = find_fence(fake, fence);

    assert(record != NULL);
    assert(record->discard_count == 0u);
    if (record->kind == FAKE_COMPILE_FENCE) {
        fake->compile_fence_status_count++;
        invalidate_at(fake, FAKE_INVALIDATE_DURING_FENCE);
        return fake->compile_status;
    }
    fake->completion_fence_status_count++;
    return fake->completion_status;
}

static bool fake_compose(const XgRenderCompiledEndpoint *endpoint,
                         uint64_t alpha_numerator,
                         uint64_t alpha_denominator,
                         uint64_t *out_completion_fence,
                         void *user_data) {
    FakeServices *fake = (FakeServices *)user_data;
    FakeEndpointRecord *record = find_endpoint(fake, endpoint);
    uint32_t index;

    assert(record != NULL);
    assert(record->release_count == 0u);
    assert(record->compose_count == 0u); /* No endpoint may render twice. */
    assert(identity_equal(&endpoint->identity, &record->endpoint.identity));
    assert(endpoint->digest == record->endpoint.digest);
    assert(endpoint->width == record->endpoint.width);
    assert(endpoint->height == record->endpoint.height);
    assert(endpoint->format == record->endpoint.format);
    assert(endpoint->backend_generation ==
           record->endpoint.backend_generation);
    assert(alpha_numerator == 1u && alpha_denominator == 1u);
    assert(out_completion_fence != NULL);

    record->compose_count++;
    fake->compose_count++;
    if (fake->zero_completion_fence) {
        *out_completion_fence = 0u;
    } else {
        index = (uint32_t)(record - fake->endpoints);
        *out_completion_fence = UINT64_C(0xd000000000000000) + index + 1u;
        add_fence(fake, *out_completion_fence, FAKE_COMPLETION_FENCE);
    }
    invalidate_at(fake, FAKE_INVALIDATE_DURING_COMPOSE);
    return fake->compose_ok;
}

static void fake_swap(void *user_data) {
    FakeServices *fake = (FakeServices *)user_data;
    fake->swap_count++;
    invalidate_at(fake, FAKE_INVALIDATE_DURING_SWAP);
}

static void fake_discard(uint64_t fence, void *user_data) {
    FakeServices *fake = (FakeServices *)user_data;
    FakeFenceRecord *record = find_fence(fake, fence);

    assert(record != NULL);
    assert(record->discard_count == 0u); /* Every fence retires exactly once. */
    record->discard_count++;
    fake->discard_count++;
    invalidate_at(fake, FAKE_INVALIDATE_DURING_RETIREMENT);
}

static void fake_release_endpoint(const XgRenderCompiledEndpoint *endpoint,
                                  void *user_data) {
    FakeServices *fake = (FakeServices *)user_data;
    FakeEndpointRecord *record = find_endpoint(fake, endpoint);

    assert(record != NULL);
    assert(record->release_count == 0u); /* Endpoint ownership is exact-once. */
    assert(identity_equal(&endpoint->identity, &record->endpoint.identity));
    assert(endpoint->digest == record->endpoint.digest);
    assert(endpoint->backend_generation ==
           record->endpoint.backend_generation);
    record->release_count++;
    fake->endpoint_release_count++;
}

static XgRenderWorkerServices worker_services(FakeServices *fake) {
    return (XgRenderWorkerServices){
        .compile = fake_compile,
        .fence_status = fake_fence_status,
        .discard_fence = fake_discard,
        .release_endpoint = fake_release_endpoint,
        .user_data = fake,
    };
}

static XgRenderPresenterServices presenter_services(FakeServices *fake) {
    return (XgRenderPresenterServices){
        .fence_status = fake_fence_status,
        .compose = fake_compose,
        .swap_window = fake_swap,
        .discard_fence = fake_discard,
        .user_data = fake,
        .owner_token = PRESENTER_OWNER,
    };
}

static void begin_fixture(FakeServices *fake) {
    XgRenderPresentationDiagnostics diagnostics;

    xg_render_semantic_presentation_reset();
    assert(xg_render_presentation_lifecycle_claim_open(PRESENTER_OWNER) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    memset(fake, 0, sizeof(*fake));
    fake->compile_ok = true;
    fake->compose_ok = true;
    fake->compile_status = XG_RENDER_FENCE_READY;
    fake->completion_status = XG_RENDER_FENCE_PENDING;
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.publication_open);
    assert(diagnostics.presenter_owner_token == PRESENTER_OWNER);
    assert(diagnostics.lifecycle_open_count == 1u);
}

static XgRenderSourceCommitHandle make_commit(uint32_t source_index) {
    const XgSemanticSceneIdentity scene = {
        .disc_id = 1u,
        .executable_identity = 10u,
        .primary_overlay_identity = 20u,
        .authored_scene_id = 30u,
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
    XgSemanticDrawRecord draw = {0};
    XgRenderSourceBuilder builder;
    XgRenderSourceCommitHandle commit;

    assert(xg_render_timeline_next_identity(7u, &identity) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_source_commit_begin(&identity, &scene, &display, 1u,
                                         false, true, &builder) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_append_pass(builder, &pass) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    draw.order.pass_id = 1u;
    draw.order.insertion_ordinal = source_index;
    draw.has_provenance = true;
    draw.provenance.state_id.scene_epoch = 1u;
    draw.provenance.state_id.state_sequence = source_index;
    draw.provenance.slot_index = source_index;
    draw.source_primitive_index = source_index;
    draw.primitive.triangle_count = 1u;
    assert(xg_render_source_commit_append_draw(builder, &draw) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_source_commit_seal(builder, &commit) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    return commit;
}

static XgRenderSourceCommitHandle publish_commit(uint32_t source_index) {
    XgRenderSourceCommitHandle commit;
    assert(xg_render_timeline_source_boundary(source_index,
                                               source_index * 100u) ==
           XG_RENDER_TIMELINE_OK);
    commit = make_commit(source_index);
    assert(xg_render_source_queue_publish(commit) == XG_RENDER_TIMELINE_OK);
    return commit;
}

static void assert_commit_retired(XgRenderSourceCommitHandle commit) {
    assert(xg_render_source_commit_header_copy(
               commit, &(XgRenderSourceCommitHeader){0}) ==
           XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT);
    assert(xg_render_source_commit_retire(commit) ==
           XG_RENDER_SOURCE_COMMIT_INVALID_TRANSITION);
}

static void assert_trace_retired(const FakeServices *fake) {
    uint32_t index;
    assert(fake->endpoint_release_count == fake->endpoint_count);
    assert(fake->discard_count == fake->fence_count);
    for (index = 0u; index < fake->endpoint_count; ++index) {
        assert(fake->endpoints[index].release_count == 1u);
        assert_commit_retired(fake->endpoints[index].commit);
    }
    for (index = 0u; index < fake->fence_count; ++index)
        assert(fake->fences[index].discard_count == 1u);
}

static void finish_fixture(FakeServices *fake) {
    XgRenderPresenterServices presenter = presenter_services(fake);
    XgRenderPresentationDiagnostics diagnostics;
    uint64_t closed_epoch = 0u;

    fake->invalidation_hook = FAKE_INVALIDATE_NONE;
    assert(xg_render_presentation_lifecycle_close(
               PRESENTER_OWNER, XG_RENDER_TIMELINE_RESET, &closed_epoch) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(closed_epoch != 0u);
    assert(xg_render_presenter_drain_retirements(&presenter));
    assert_trace_retired(fake);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(!diagnostics.publication_open);
    assert(diagnostics.presenter_owner_token == PRESENTER_OWNER);
    assert(diagnostics.lifecycle_close_count == 1u);
}

static void test_endpoint_once_pending_ready_and_exact_retirement(void) {
    FakeServices fake;
    XgRenderSourceCommitHandle commit;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    uint32_t discarded;
    uint32_t released;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    commit = publish_commit(1u);
    fake.compile_status = XG_RENDER_FENCE_PENDING;

    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(fake.compile_count == 1u && fake.endpoint_count == 1u);
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_FENCE_PENDING);
    assert(fake.compose_count == 0u && fake.swap_count == 0u);

    fake.compile_status = XG_RENDER_FENCE_READY;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_PRESENTED);
    assert(fake.compile_count == 1u);
    assert(fake.compose_count == 1u && fake.swap_count == 1u);
    assert(fake.endpoints[0].compose_count == 1u);

    /* Re-polling completion cannot compile, compose, or swap the endpoint. */
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_FENCE_PENDING);
    assert(fake.compile_count == 1u && fake.compose_count == 1u &&
           fake.swap_count == 1u);
    fake.completion_status = XG_RENDER_FENCE_READY;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_EMPTY);
    assert_commit_retired(commit);
    assert_trace_retired(&fake);

    discarded = fake.discard_count;
    released = fake.endpoint_release_count;
    finish_fixture(&fake);
    assert(fake.discard_count == discarded);
    assert(fake.endpoint_release_count == released);
    assert_trace_retired(&fake);
}

static void test_fence_failures(void) {
    FakeServices fake;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    XgRenderPresentationDiagnostics diagnostics;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    (void)publish_commit(1u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    fake.compile_status = XG_RENDER_FENCE_FAILED;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_FENCE_FAILED);
    assert(fake.compose_count == 0u && fake.swap_count == 0u);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.fence_failures == 1u);
    assert_trace_retired(&fake);
    finish_fixture(&fake);

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    (void)publish_commit(1u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_PRESENTED);
    fake.completion_status = XG_RENDER_FENCE_FAILED;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_FENCE_FAILED);
    assert(fake.compose_count == 1u && fake.swap_count == 1u);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.fence_failures == 1u);
    assert_trace_retired(&fake);
    finish_fixture(&fake);
}

static void test_compose_failure_and_zero_fence_fail_closed(void) {
    FakeServices fake;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    fake.compose_ok = false;
    (void)publish_commit(1u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_COMPOSE_FAILED);
    assert(fake.compose_count == 1u && fake.swap_count == 0u);
    assert(fake.fence_count == 2u);
    assert_trace_retired(&fake);
    finish_fixture(&fake);

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    fake.zero_completion_fence = true;
    (void)publish_commit(1u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_COMPOSE_FAILED);
    assert(fake.compose_ok && fake.compose_count == 1u);
    assert(fake.swap_count == 0u);
    assert(fake.fence_count == 1u); /* Zero is never accepted or discarded. */
    assert_trace_retired(&fake);
    finish_fixture(&fake);
}

static void test_stale_commit_published_after_invalidation(void) {
    FakeServices fake;
    XgRenderSourceCommitHandle stale;
    XgRenderPresentationDiagnostics diagnostics;

    begin_fixture(&fake);
    assert(xg_render_timeline_source_boundary(1u, 100u) ==
           XG_RENDER_TIMELINE_OK);
    stale = make_commit(1u);
    (void)xg_render_timeline_invalidate(XG_RENDER_TIMELINE_RESTORE);
    assert(xg_render_source_queue_publish(stale) ==
           XG_RENDER_TIMELINE_STALE_COMMIT);
    assert(xg_render_source_commit_header_copy(
               stale, &(XgRenderSourceCommitHeader){0}) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.stale_commits == 1u);
    assert(!diagnostics.source_pending);
    assert(xg_render_source_commit_retire(stale) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert_commit_retired(stale);
    finish_fixture(&fake);
}

static void test_invalidation_hook(FakeInvalidationHook hook) {
    FakeServices fake;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    XgRenderPresentationDiagnostics diagnostics;
    XgRenderWorkerResult worker_result;
    XgRenderPresenterResult presenter_result;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    (void)publish_commit(1u);
    fake.invalidation_hook = hook;
    worker_result = xg_render_worker_compile_next(&worker);

    if (hook == FAKE_INVALIDATE_DURING_COMPILE) {
        assert(worker_result == XG_RENDER_WORKER_STALE);
    } else {
        assert(worker_result == XG_RENDER_WORKER_OK);
        presenter_result = xg_render_presenter_present_next(&presenter);
        if (hook == FAKE_INVALIDATE_DURING_SWAP) {
            assert(presenter_result == XG_RENDER_PRESENTER_PRESENTED);
            assert(fake.swap_count == 1u);
        } else {
            assert(presenter_result == XG_RENDER_PRESENTER_STALE);
            assert(fake.swap_count == 0u);
        }
    }

    assert(fake.invalidation_count == 1u);
    assert(fake.invalidation_hook == FAKE_INVALIDATE_NONE);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.presentation_epoch == fake.invalidated_epoch);
    assert(diagnostics.invalidations_by_reason[
               XG_RENDER_TIMELINE_ARTIFACT_CHANGE] == 1u);
    assert(!diagnostics.source_pending && !diagnostics.batch_pending);
    assert_trace_retired(&fake);
    finish_fixture(&fake);
}

static void test_invalidation_during_all_endpoint_stages(void) {
    test_invalidation_hook(FAKE_INVALIDATE_DURING_COMPILE);
    test_invalidation_hook(FAKE_INVALIDATE_DURING_FENCE);
    test_invalidation_hook(FAKE_INVALIDATE_DURING_COMPOSE);
    test_invalidation_hook(FAKE_INVALIDATE_DURING_SWAP);
    test_invalidation_hook(FAKE_INVALIDATE_DURING_RETIREMENT);
}

static void test_source_and_batch_coalescing(void) {
    FakeServices fake;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    XgRenderPresentationDiagnostics diagnostics;
    XgRenderSourceCommitHandle first;
    XgRenderSourceCommitHandle second;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    first = publish_commit(1u);
    second = publish_commit(2u);
    assert_commit_retired(first);
    assert(xg_render_source_commit_header_copy(
               second, &(XgRenderSourceCommitHeader){0}) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.source_coalesces == 1u);
    assert(diagnostics.source_capacity_drops == 1u);
    assert(diagnostics.superseded_endpoints == 1u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(fake.compile_count == 1u);
    assert(fake.endpoints[0].endpoint.identity.source_sequence == 2u);
    finish_fixture(&fake);

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    first = publish_commit(1u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    second = publish_commit(2u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(fake.compile_count == 2u);
    assert(fake.endpoints[0].release_count == 1u);
    assert(fake.fences[0].discard_count == 1u);
    assert_commit_retired(first);
    assert(xg_render_source_commit_header_copy(
               second, &(XgRenderSourceCommitHeader){0}) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.batch_coalesces == 1u);
    assert(diagnostics.batch_capacity_drops == 1u);
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_PRESENTED);
    assert(fake.endpoints[0].compose_count == 0u);
    assert(fake.endpoints[1].compose_count == 1u);
    finish_fixture(&fake);
}

static void test_saturation_preserves_latest_source(void) {
    FakeServices fake;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    XgRenderPresentationDiagnostics diagnostics;
    XgRenderSourceCommitHandle latest;
    XgRenderSourceCommitHeader latest_header;
    uint32_t index;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    for (index = 0u; index < XG_RENDER_PRESENTATION_BATCH_CAPACITY; ++index) {
        (void)publish_commit(index + 1u);
        assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
        assert(xg_render_presenter_present_next(&presenter) ==
               XG_RENDER_PRESENTER_PRESENTED);
    }
    assert(fake.endpoint_count == XG_RENDER_PRESENTATION_BATCH_CAPACITY);
    assert(fake.endpoint_release_count == 0u);

    latest = publish_commit(XG_RENDER_PRESENTATION_BATCH_CAPACITY + 1u);
    assert(xg_render_worker_compile_next(&worker) ==
           XG_RENDER_WORKER_CAPACITY_EXCEEDED);
    assert(fake.compile_count == XG_RENDER_PRESENTATION_BATCH_CAPACITY);
    assert(xg_render_source_commit_header_copy(latest, &latest_header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(latest_header.state == XG_RENDER_SOURCE_QUEUED);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.worker_backpressure == 1u);
    assert(diagnostics.source_pending);
    assert(diagnostics.source_queue_depth == 1u);
    assert(diagnostics.batch_queue_depth ==
           XG_RENDER_PRESENTATION_BATCH_CAPACITY);

    /* Freeing an older completion must not retire or claim the mailbox. */
    fake.completion_status = XG_RENDER_FENCE_READY;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_EMPTY);
    assert(xg_render_source_commit_header_copy(latest, &latest_header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(latest_header.state == XG_RENDER_SOURCE_QUEUED);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.source_pending);

    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(fake.compile_count == XG_RENDER_PRESENTATION_BATCH_CAPACITY + 1u);
    finish_fixture(&fake);
}

static void test_pending_completion_retirement(void) {
    FakeServices fake;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    uint32_t index;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    for (index = 1u; index <= 2u; ++index) {
        (void)publish_commit(index);
        assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
        assert(xg_render_presenter_present_next(&presenter) ==
               XG_RENDER_PRESENTER_PRESENTED);
    }
    assert(fake.compose_count == 2u && fake.swap_count == 2u);
    finish_fixture(&fake);
    assert(fake.discard_count == 4u);
    assert_trace_retired(&fake);
}

static void test_owner_rejection(void) {
    FakeServices fake;
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    XgRenderPresenterServices foreign;
    uint32_t fence_status_count;

    begin_fixture(&fake);
    worker = worker_services(&fake);
    presenter = presenter_services(&fake);
    foreign = presenter;
    foreign.owner_token++;
    assert(xg_render_presenter_drain_retirements(&presenter));
    assert(!xg_render_presenter_drain_retirements(&foreign));

    (void)publish_commit(1u);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    fence_status_count = fake.compile_fence_status_count;
    assert(xg_render_presenter_present_next(&foreign) ==
           XG_RENDER_PRESENTER_OWNER_REJECTED);
    assert(fake.compile_fence_status_count == fence_status_count);
    assert(fake.compose_count == 0u && fake.swap_count == 0u);

    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_PRESENTED);
    fake.completion_status = XG_RENDER_FENCE_READY;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_EMPTY);
    assert_trace_retired(&fake);
    finish_fixture(&fake);
}

int main(void) {
    test_endpoint_once_pending_ready_and_exact_retirement();
    test_fence_failures();
    test_compose_failure_and_zero_fence_fail_closed();
    test_stale_commit_published_after_invalidation();
    test_invalidation_during_all_endpoint_stages();
    test_source_and_batch_coalescing();
    test_saturation_preserves_latest_source();
    test_pending_completion_retirement();
    test_owner_rejection();
    return 0;
}
