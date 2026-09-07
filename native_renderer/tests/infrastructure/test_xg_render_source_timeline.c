#include "xg_render_semantic_presentation.h"
#include "xg_render_movie_publisher.h"
#include "xg_render_source_frame.h"
#include "xg_render_surface_graph.h"

#include <assert.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>

typedef enum FakePresentationMode {
    FAKE_RENDERED = 0,
    FAKE_HEADLESS,
    FAKE_BLANK,
    FAKE_TURBO,
    FAKE_PRESENT_SKIPPED,
    FAKE_MODE_COUNT,
} FakePresentationMode;

#define FAKE_PRESENTER_OWNER UINT64_C(0x503250524553454e)

typedef struct RetirementRace {
    atomic_bool callback_entered;
    atomic_bool allow_callback_return;
} RetirementRace;

typedef struct FakeTrace {
    XgRenderCompiledEndpoint compiled_endpoints[8];
    bool endpoint_released[8];
    uint64_t commit_digests[8];
    XgPresentationIdentity identities[8];
    uint32_t commit_count;
    uint32_t endpoint_release_count;
    uint32_t swap_count;
    uint32_t discard_count;
    bool compile_ok;
    bool compose_ok;
    bool last_discontinuity;
    uint32_t last_resource_count;
    uint32_t last_surface_edge_count;
    XgRenderSourceCommitHandle last_commit;
    XgRenderTimelineInvalidationReason reentry_reason;
    bool guest_invalidation_active;
    bool discard_reenters;
    bool invalidate_during_compose;
    bool invalidate_during_swap;
    bool reset_during_compile;
    bool reset_during_fence;
    bool reset_during_compose;
    bool reset_during_swap;
    bool reset_during_discard;
    bool inspect_pending_during_swap;
    bool pending_during_swap;
    RetirementRace *retirement_race;
} FakeTrace;

static const XgRenderSourceFrameDescription k_frame_description = {
    .scene = {
        .disc_id = 1u,
        .executable_identity = 10u,
        .primary_overlay_identity = 20u,
        .authored_scene_id = 30u,
        .module = XG_SEMANTIC_MODULE_FIELD,
    },
    .display = {
        .width = 320u,
        .height = 240u,
        .aspect_num = 4u,
        .aspect_den = 3u,
    },
    .scene_generation = 7u,
    .temporally_eligible = true,
};

static bool fake_compile(XgRenderSourceCommitHandle commit,
                         XgRenderCompiledEndpoint *out_endpoint,
                         uint64_t *out_fence,
                         void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    XgRenderSourceCommitHeader header;

    assert(xg_render_source_commit_header_copy(commit, &header) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(trace->commit_count < 8u);
    trace->commit_digests[trace->commit_count] = header.digest;
    trace->identities[trace->commit_count] = header.identity;
    *out_endpoint = (XgRenderCompiledEndpoint){
        .opaque_handle = UINT64_C(0x5032454e44500000) +
            trace->commit_count + 1u,
        .identity = header.identity,
        .digest = header.digest,
        .width = header.display.width,
        .height = header.display.height,
        .format = 1u,
        .backend_generation = (uint64_t)trace->commit_count + 1u,
    };
    trace->compiled_endpoints[trace->commit_count] = *out_endpoint;
    trace->last_commit = commit;
    trace->commit_count++;
    trace->last_discontinuity = header.discontinuity;
    trace->last_resource_count = header.resource_count;
    trace->last_surface_edge_count = header.surface_edge_count;
    *out_fence = header.digest;
    if (trace->reset_during_compile) {
        trace->reset_during_compile = false;
        xg_render_semantic_presentation_reset();
    }
    return trace->compile_ok;
}

static XgRenderFenceStatus fake_fence_status(uint64_t fence, void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    assert(fence != 0u);
    if (trace->reset_during_fence) {
        trace->reset_during_fence = false;
        xg_render_semantic_presentation_reset();
    }
    return XG_RENDER_FENCE_READY;
}

static bool fake_compose(const XgRenderCompiledEndpoint *endpoint,
                         uint64_t alpha_numerator,
                         uint64_t alpha_denominator,
                         uint64_t *out_completion_fence,
                         void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    uint32_t index;

    assert(endpoint != NULL && endpoint->opaque_handle != 0u);
    for (index = 0u; index < trace->commit_count; ++index) {
        if (trace->compiled_endpoints[index].opaque_handle ==
                endpoint->opaque_handle)
            break;
    }
    assert(index < trace->commit_count);
    assert(!trace->endpoint_released[index]);
    assert(endpoint->identity.presentation_epoch ==
           trace->identities[index].presentation_epoch);
    assert(endpoint->identity.source_sequence ==
           trace->identities[index].source_sequence);
    assert(endpoint->identity.guest_vblank_sequence ==
           trace->identities[index].guest_vblank_sequence);
    assert(endpoint->identity.guest_cycle ==
           trace->identities[index].guest_cycle);
    assert(endpoint->identity.scene_generation ==
           trace->identities[index].scene_generation);
    assert(endpoint->digest == trace->commit_digests[index]);
    assert(endpoint->width == trace->compiled_endpoints[index].width);
    assert(endpoint->height == trace->compiled_endpoints[index].height);
    assert(endpoint->format == trace->compiled_endpoints[index].format);
    assert(endpoint->backend_generation ==
           trace->compiled_endpoints[index].backend_generation);
    assert(alpha_numerator == 1u && alpha_denominator == 1u);
    *out_completion_fence = endpoint->opaque_handle ^
        UINT64_C(0x4000000000000000);
    assert(*out_completion_fence != 0u);
    if (trace->invalidate_during_compose) {
        trace->invalidate_during_compose = false;
        (void)xg_render_timeline_invalidate(trace->reentry_reason);
    }
    if (trace->reset_during_compose) {
        trace->reset_during_compose = false;
        xg_render_semantic_presentation_reset();
    }
    return trace->compose_ok;
}

static void fake_release_endpoint(const XgRenderCompiledEndpoint *endpoint,
                                  void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    uint32_t index;

    assert(endpoint != NULL && endpoint->opaque_handle != 0u);
    for (index = 0u; index < trace->commit_count; ++index) {
        if (trace->compiled_endpoints[index].opaque_handle ==
                endpoint->opaque_handle)
            break;
    }
    assert(index < trace->commit_count);
    assert(!trace->endpoint_released[index]);
    assert(endpoint->digest == trace->commit_digests[index]);
    assert(endpoint->backend_generation ==
           trace->compiled_endpoints[index].backend_generation);
    trace->endpoint_released[index] = true;
    trace->endpoint_release_count++;
}

static void fake_swap(void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    trace->swap_count++;
    if (trace->inspect_pending_during_swap) {
        XgRenderPresentationDiagnostics diagnostics;
        xg_render_semantic_presentation_diagnostics(&diagnostics);
        trace->pending_during_swap = diagnostics.batch_pending;
    }
    if (trace->invalidate_during_swap) {
        trace->invalidate_during_swap = false;
        (void)xg_render_timeline_invalidate(trace->reentry_reason);
    }
    if (trace->reset_during_swap) {
        trace->reset_during_swap = false;
        xg_render_semantic_presentation_reset();
    }
}

static void fake_discard(uint64_t fence, void *user_data) {
    FakeTrace *trace = (FakeTrace *)user_data;
    assert(fence != 0u);
    assert(!trace->guest_invalidation_active);
    trace->discard_count++;
    if (trace->reset_during_discard) {
        trace->reset_during_discard = false;
        xg_render_semantic_presentation_reset();
    }
    if (trace->retirement_race != NULL) {
        atomic_store_explicit(&trace->retirement_race->callback_entered, true,
                              memory_order_release);
        while (!atomic_load_explicit(
                   &trace->retirement_race->allow_callback_return,
                   memory_order_acquire))
            thrd_yield();
    }
    if (trace->discard_reenters) {
        trace->discard_reenters = false;
        (void)xg_render_timeline_invalidate(trace->reentry_reason);
    }
}

static XgRenderWorkerServices worker_services(FakeTrace *trace) {
    return (XgRenderWorkerServices){
        .compile = fake_compile,
        .fence_status = fake_fence_status,
        .discard_fence = fake_discard,
        .release_endpoint = fake_release_endpoint,
        .user_data = trace,
    };
}

static XgRenderPresenterServices presenter_services(FakeTrace *trace) {
    return (XgRenderPresenterServices){
        .fence_status = fake_fence_status,
        .compose = fake_compose,
        .swap_window = fake_swap,
        .discard_fence = fake_discard,
        .user_data = trace,
        .owner_token = FAKE_PRESENTER_OWNER,
    };
}

typedef struct DrainThreadContext {
    XgRenderPresenterServices services;
    atomic_bool started;
    atomic_bool finished;
    bool result;
} DrainThreadContext;

static int drain_retirements_thread(void *user_data) {
    DrainThreadContext *context = (DrainThreadContext *)user_data;
    atomic_store_explicit(&context->started, true, memory_order_release);
    context->result = xg_render_presenter_drain_retirements(
        &context->services);
    atomic_store_explicit(&context->finished, true, memory_order_release);
    return 0;
}

static void reset_source_lifecycle(void) {
    xg_render_semantic_presentation_reset();
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
    assert(xg_render_presentation_lifecycle_claim_open(
               FAKE_PRESENTER_OWNER) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
}

static void claim_open_source_lifecycle(void) {
    assert(xg_render_presentation_lifecycle_claim_open(
               FAKE_PRESENTER_OWNER) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
}

static void close_source_lifecycle(
        const XgRenderPresenterServices *presenter) {
    XgRenderPresentationDiagnostics diagnostics;
    uint64_t closed_epoch = 0u;

    assert(xg_render_presentation_lifecycle_close(
               FAKE_PRESENTER_OWNER, XG_RENDER_TIMELINE_RESET,
               &closed_epoch) == XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(closed_epoch != 0u);
    if (presenter != NULL)
        assert(xg_render_presenter_drain_retirements(presenter));
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(!diagnostics.publication_open);
    assert(!diagnostics.source_pending && !diagnostics.batch_pending);
    assert(diagnostics.retirement_queue_depth == 0u);
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    xg_render_resource_repository_reset();
}

static void assert_all_endpoints_released(const FakeTrace *trace) {
    assert(trace->endpoint_release_count == trace->commit_count);
    for (uint32_t index = 0u; index < trace->commit_count; ++index)
        assert(trace->endpoint_released[index]);
}

static XgRenderResourceIdentity surface_identity(uint8_t seed) {
    XgRenderResourceIdentity identity;
    for (uint32_t index = 0u; index < sizeof(identity.bytes); ++index)
        identity.bytes[index] = (uint8_t)(seed + index);
    return identity;
}

static void build_frame(uint32_t source_index) {
    const XgSemanticPassRecord pass = {
        .pass_id = 1u,
        .store = true,
    };
    XgSemanticDrawRecord draw = {0};

    draw.order.pass_id = 1u;
    draw.order.insertion_ordinal = source_index;
    draw.has_provenance = true;
    draw.provenance.state_id.scene_epoch = 3u;
    draw.provenance.state_id.state_sequence = source_index;
    draw.provenance.slot_index = source_index;
    draw.source_primitive_index = source_index;
    draw.primitive.triangle_count = 1u;
    draw.primitive.triangles[0].vertices[0].x = (int32_t)source_index;

    assert(xg_render_source_frame_begin(&k_frame_description) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_pass(&pass) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_draw(&draw) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK);
}

static XgRenderSourceFrameResult source_boundary(uint64_t vblank,
                                                  uint64_t cycle) {
    XgRenderSourceFrameResult frame_result;

    assert(xg_render_timeline_source_boundary(vblank, cycle) ==
           XG_RENDER_TIMELINE_OK);
    frame_result = xg_render_source_frame_publish_boundary();
    if (frame_result == XG_RENDER_SOURCE_FRAME_EMPTY)
        xg_render_timeline_note_hold();
    else if (frame_result != XG_RENDER_SOURCE_FRAME_OK)
        xg_render_timeline_note_rejection();
    return frame_result;
}

static uint64_t trace_digest(const FakeTrace *trace) {
    uint64_t digest = UINT64_C(1469598103934665603);
    const uint64_t first_epoch = trace->commit_count == 0u
        ? 0u : trace->identities[0].presentation_epoch;

    for (uint32_t index = 0u; index < trace->commit_count; ++index) {
        const uint64_t values[] = {
            trace->identities[index].presentation_epoch - first_epoch + 1u,
            trace->identities[index].source_sequence,
            trace->identities[index].guest_vblank_sequence,
            trace->identities[index].guest_cycle,
            trace->identities[index].scene_generation,
        };
        for (size_t value = 0u; value < sizeof(values) / sizeof(values[0]);
             ++value) {
            uint64_t word = values[value];
            for (uint32_t byte = 0u; byte < 8u; ++byte) {
                digest ^= (uint8_t)(word >> (byte * 8u));
                digest *= UINT64_C(1099511628211);
            }
        }
    }
    return digest;
}

static uint64_t run_mode(FakePresentationMode mode, FakeTrace *out_trace) {
    FakeTrace trace = {
        .compile_ok = true,
        .compose_ok = true,
    };
    XgRenderPresentationDiagnostics diagnostics;
    XgRenderWorkerServices worker = worker_services(&trace);
    XgRenderPresenterServices presenter = presenter_services(&trace);

    reset_source_lifecycle();
    assert(source_boundary(1u, 100u) == XG_RENDER_SOURCE_FRAME_EMPTY);
    build_frame(11u);
    assert(source_boundary(2u, 200u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    if (mode == FAKE_RENDERED)
        assert(xg_render_presenter_present_next(&presenter) ==
               XG_RENDER_PRESENTER_PRESENTED);

    assert(source_boundary(3u, 300u) == XG_RENDER_SOURCE_FRAME_EMPTY);
    build_frame(22u);
    assert(source_boundary(4u, 400u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    if (mode == FAKE_RENDERED)
        assert(xg_render_presenter_present_next(&presenter) ==
               XG_RENDER_PRESENTER_PRESENTED);

    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.boundary_count == 4u);
    assert(diagnostics.published_commits == 2u);
    assert(diagnostics.presentation_holds == 2u);
    assert(diagnostics.source_sequence == 2u);
    assert(trace.commit_count == 2u);
    assert(trace.swap_count == (mode == FAKE_RENDERED ? 2u : 0u));
    /* Skipped modes retain a fake batch; retire it while its services live. */
    close_source_lifecycle(&presenter);
    assert_all_endpoints_released(&trace);
    *out_trace = trace;
    return trace_digest(out_trace);
}

static void test_mode_equivalence(void) {
    FakeTrace traces[FAKE_MODE_COUNT];
    uint64_t digests[FAKE_MODE_COUNT];

    for (uint32_t mode = 0u; mode < FAKE_MODE_COUNT; ++mode)
        digests[mode] = run_mode((FakePresentationMode)mode, &traces[mode]);
    assert(digests[0] == UINT64_C(0xd67a01a1d7ae4ff9));
    for (uint32_t mode = 1u; mode < FAKE_MODE_COUNT; ++mode) {
        assert(digests[mode] == digests[0]);
        for (uint32_t index = 0u; index < 2u; ++index) {
            const XgPresentationIdentity *actual =
                &traces[mode].identities[index];
            const XgPresentationIdentity *expected =
                &traces[0].identities[index];
            assert(traces[mode].commit_digests[index] != 0u);
            assert(traces[0].commit_digests[index] != 0u);
            assert(actual->presentation_epoch ==
                   traces[mode].identities[0].presentation_epoch);
            assert(expected->presentation_epoch ==
                   traces[0].identities[0].presentation_epoch);
            assert(actual->source_sequence == expected->source_sequence);
            assert(actual->guest_vblank_sequence ==
                   expected->guest_vblank_sequence);
            assert(actual->guest_cycle == expected->guest_cycle);
            assert(actual->scene_generation == expected->scene_generation);
        }
    }
}

static void test_boundary_diagnostics(void) {
    XgRenderPresentationDiagnostics diagnostics;

    reset_source_lifecycle();
    assert(xg_render_timeline_source_boundary(1u, 100u) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_timeline_source_boundary(1u, 101u) ==
           XG_RENDER_TIMELINE_DUPLICATE_BOUNDARY);
    assert(xg_render_timeline_source_boundary(4u, 400u) ==
           XG_RENDER_TIMELINE_OK);
    assert(xg_render_timeline_source_boundary(3u, 300u) ==
           XG_RENDER_TIMELINE_DUPLICATE_BOUNDARY);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.boundary_count == 2u);
    assert(diagnostics.duplicate_boundaries == 2u);
    assert(diagnostics.missed_boundaries == 2u);
    assert(diagnostics.guest_vblank_sequence == 4u);
    assert(diagnostics.guest_cycle == 400u);
    close_source_lifecycle(NULL);
}

static void test_epoch_invalidations(void) {
    const XgRenderTimelineInvalidationReason reasons[] = {
        XG_RENDER_TIMELINE_RESTORE,
        XG_RENDER_TIMELINE_ROLLBACK,
        XG_RENDER_TIMELINE_DISC_CHANGE,
        XG_RENDER_TIMELINE_RESET,
    };
    FakeTrace trace = {
        .compile_ok = true,
        .compose_ok = true,
    };
    XgRenderWorkerServices worker = worker_services(&trace);
    XgRenderPresenterServices presenter = presenter_services(&trace);
    XgRenderPresentationDiagnostics diagnostics;
    XgRenderSourceFrameSnapshot frame_snapshot;
    uint64_t expected_epoch;
    uint64_t initial_invalidation_count;
    uint64_t initial_invalidations_by_reason[
        XG_RENDER_TIMELINE_INVALIDATION_REASON_COUNT];

    reset_source_lifecycle();
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    expected_epoch = diagnostics.presentation_epoch;
    initial_invalidation_count = diagnostics.invalidation_count;
    memcpy(initial_invalidations_by_reason,
           diagnostics.invalidations_by_reason,
           sizeof(initial_invalidations_by_reason));
    for (uint32_t index = 0u;
         index < sizeof(reasons) / sizeof(reasons[0]); ++index) {
        build_frame(40u + index);
        assert(source_boundary(1u, 100u + index) ==
               XG_RENDER_SOURCE_FRAME_OK);
        assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
        assert(trace.last_discontinuity);

        build_frame(50u + index);
        expected_epoch++;
        assert(xg_render_timeline_invalidate(reasons[index]) == expected_epoch);
        xg_render_source_frame_snapshot(&frame_snapshot);
        assert(!frame_snapshot.active);
        assert(frame_snapshot.presentation_epoch == expected_epoch);

        xg_render_semantic_presentation_diagnostics(&diagnostics);
        assert(diagnostics.presentation_epoch == expected_epoch);
        assert(diagnostics.source_sequence == 0u);
        assert(diagnostics.guest_vblank_sequence == 0u);
        assert(!diagnostics.source_pending && !diagnostics.batch_pending);
        assert(diagnostics.invalidation_count ==
               initial_invalidation_count + (uint64_t)index + 1u);
        if (initial_invalidations_by_reason[reasons[index]] == 0u) {
            assert(diagnostics.invalidations_by_reason[reasons[index]] == 1u);
        } else {
            assert(diagnostics.invalidations_by_reason[reasons[index]] ==
                   initial_invalidations_by_reason[reasons[index]] + 1u);
        }
        assert(diagnostics.last_invalidation_reason == reasons[index]);
        assert(xg_render_presenter_drain_retirements(&presenter));
        assert_all_endpoints_released(&trace);
    }
    close_source_lifecycle(&presenter);
}

static void test_nonblocking_invalidation_and_stale_before_swap(void) {
    FakeTrace trace = {
        .compile_ok = true,
        .compose_ok = true,
        .reentry_reason = XG_RENDER_TIMELINE_SCENE_CHANGE,
    };
    XgRenderWorkerServices worker = worker_services(&trace);
    XgRenderPresenterServices presenter = presenter_services(&trace);
    XgRenderPresenterServices foreign_presenter = presenter;
    XgRenderPresentationDiagnostics diagnostics;
    uint64_t initial_epoch;

    reset_source_lifecycle();
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    initial_epoch = diagnostics.presentation_epoch;
    build_frame(70u);
    assert(source_boundary(1u, 100u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);

    trace.guest_invalidation_active = true;
    assert(xg_render_timeline_invalidate(
               XG_RENDER_TIMELINE_ARTIFACT_CHANGE) == initial_epoch + 1u);
    trace.guest_invalidation_active = false;
    assert(trace.discard_count == 0u);

    trace.discard_reenters = true;
    assert(xg_render_presenter_drain_retirements(&presenter));
    assert(trace.discard_count == 1u);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.presentation_epoch == initial_epoch + 2u);
    assert(diagnostics.invalidations_by_reason[
               XG_RENDER_TIMELINE_ARTIFACT_CHANGE] == 1u);
    assert(diagnostics.invalidations_by_reason[
               XG_RENDER_TIMELINE_SCENE_CHANGE] == 1u);
    assert(diagnostics.last_invalidation_reason ==
           XG_RENDER_TIMELINE_SCENE_CHANGE);
    assert_all_endpoints_released(&trace);

    foreign_presenter.owner_token++;
    assert(!xg_render_presenter_drain_retirements(&foreign_presenter));

    reset_source_lifecycle();
    memset(&trace, 0, sizeof(trace));
    trace.compile_ok = true;
    trace.compose_ok = true;
    trace.invalidate_during_compose = true;
    trace.reentry_reason = XG_RENDER_TIMELINE_ARTIFACT_CHANGE;
    worker = worker_services(&trace);
    presenter = presenter_services(&trace);
    build_frame(71u);
    assert(source_boundary(1u, 200u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_STALE);
    assert(trace.swap_count == 0u);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.presented_endpoints == 0u);
    assert(diagnostics.invalidations_by_reason[
               XG_RENDER_TIMELINE_ARTIFACT_CHANGE] == 1u);
    assert_all_endpoints_released(&trace);

    reset_source_lifecycle();
    memset(&trace, 0, sizeof(trace));
    trace.compile_ok = true;
    trace.compose_ok = true;
    trace.invalidate_during_swap = true;
    trace.inspect_pending_during_swap = true;
    trace.reentry_reason = XG_RENDER_TIMELINE_ARTIFACT_CHANGE;
    worker = worker_services(&trace);
    presenter = presenter_services(&trace);
    build_frame(72u);
    assert(source_boundary(1u, 300u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_PRESENTED);
    assert(trace.swap_count == 1u);
    assert(!trace.pending_during_swap);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.presented_endpoints == 1u);
    assert(diagnostics.stale_batches == 1u);
    assert(!diagnostics.batch_pending);
    assert(diagnostics.invalidations_by_reason[
                XG_RENDER_TIMELINE_ARTIFACT_CHANGE] == 1u);
    assert_all_endpoints_released(&trace);
    close_source_lifecycle(&presenter);
}

static void test_reentrant_reset_callbacks_and_retirement_quiescence(void) {
    FakeTrace trace = { .compile_ok = true, .compose_ok = true };
    XgRenderWorkerServices worker;
    XgRenderPresenterServices presenter;
    XgRenderPresentationDiagnostics diagnostics;
    RetirementRace retirement_race;
    DrainThreadContext drain_context;
    DrainThreadContext foreign_context;
    thrd_t drain_thread;
    thrd_t foreign_thread;
    uint64_t expected_epoch;

    reset_source_lifecycle();
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    expected_epoch = diagnostics.presentation_epoch + 1u;
    worker = worker_services(&trace);
    build_frame(80u);
    assert(source_boundary(1u, 100u) == XG_RENDER_SOURCE_FRAME_OK);
    trace.reset_during_compile = true;
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_STALE);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.presentation_epoch == expected_epoch);
    assert(diagnostics.boundary_count == 0u && !diagnostics.batch_pending);
    assert_all_endpoints_released(&trace);
    claim_open_source_lifecycle();

    reset_source_lifecycle();
    memset(&trace, 0, sizeof(trace));
    trace.compile_ok = true;
    trace.compose_ok = true;
    worker = worker_services(&trace);
    presenter = presenter_services(&trace);
    build_frame(81u);
    assert(source_boundary(1u, 200u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    trace.reset_during_fence = true;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_STALE);
    assert(trace.swap_count == 0u);
    assert_all_endpoints_released(&trace);
    claim_open_source_lifecycle();

    reset_source_lifecycle();
    memset(&trace, 0, sizeof(trace));
    trace.compile_ok = true;
    trace.compose_ok = true;
    worker = worker_services(&trace);
    presenter = presenter_services(&trace);
    build_frame(82u);
    assert(source_boundary(1u, 300u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    trace.reset_during_compose = true;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_STALE);
    assert(trace.swap_count == 0u);
    assert_all_endpoints_released(&trace);
    claim_open_source_lifecycle();

    reset_source_lifecycle();
    memset(&trace, 0, sizeof(trace));
    trace.compile_ok = true;
    trace.compose_ok = true;
    worker = worker_services(&trace);
    presenter = presenter_services(&trace);
    build_frame(83u);
    assert(source_boundary(1u, 400u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    expected_epoch = diagnostics.presentation_epoch + 1u;
    trace.reset_during_swap = true;
    assert(xg_render_presenter_present_next(&presenter) ==
           XG_RENDER_PRESENTER_PRESENTED);
    assert(trace.swap_count == 1u);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.presentation_epoch == expected_epoch);
    assert(diagnostics.presented_endpoints == 1u);
    assert_all_endpoints_released(&trace);
    claim_open_source_lifecycle();

    reset_source_lifecycle();
    memset(&trace, 0, sizeof(trace));
    trace.compile_ok = true;
    trace.compose_ok = true;
    worker = worker_services(&trace);
    presenter = presenter_services(&trace);
    build_frame(84u);
    assert(source_boundary(1u, 500u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    expected_epoch = diagnostics.presentation_epoch + 1u;
    assert(xg_render_timeline_invalidate(XG_RENDER_TIMELINE_RESTORE) ==
           expected_epoch);
    atomic_init(&retirement_race.callback_entered, false);
    atomic_init(&retirement_race.allow_callback_return, false);
    trace.retirement_race = &retirement_race;
    trace.reset_during_discard = true;
    drain_context = (DrainThreadContext){ .services = presenter };
    atomic_init(&drain_context.started, false);
    atomic_init(&drain_context.finished, false);
    assert(thrd_create(&drain_thread, drain_retirements_thread,
                       &drain_context) == thrd_success);
    while (!atomic_load_explicit(&retirement_race.callback_entered,
                                 memory_order_acquire))
        thrd_yield();

    foreign_context = (DrainThreadContext){ .services = presenter };
    foreign_context.services.owner_token++;
    atomic_init(&foreign_context.started, false);
    atomic_init(&foreign_context.finished, false);
    assert(thrd_create(&foreign_thread, drain_retirements_thread,
                       &foreign_context) == thrd_success);
    while (!atomic_load_explicit(&foreign_context.started,
                                 memory_order_acquire))
        thrd_yield();
    for (uint32_t index = 0u; index < 1000u; ++index) thrd_yield();
    assert(!atomic_load_explicit(&drain_context.finished,
                                  memory_order_acquire));
    assert(atomic_load_explicit(&foreign_context.finished,
                                memory_order_acquire));
    assert(xg_render_source_commit_header_copy(
               trace.last_commit, &(XgRenderSourceCommitHeader){0}) ==
           XG_RENDER_SOURCE_COMMIT_OK);

    atomic_store_explicit(&retirement_race.allow_callback_return, true,
                          memory_order_release);
    assert(thrd_join(drain_thread, NULL) == thrd_success);
    assert(thrd_join(foreign_thread, NULL) == thrd_success);
    assert(drain_context.result);
    assert(!foreign_context.result);
    assert(trace.discard_count == 1u);
    assert_all_endpoints_released(&trace);
    assert(xg_render_source_commit_header_copy(
               trace.last_commit, &(XgRenderSourceCommitHeader){0}) ==
           XG_RENDER_SOURCE_COMMIT_INVALID_ARGUMENT);
    claim_open_source_lifecycle();
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.presentation_epoch == expected_epoch + 1u);
    assert(!diagnostics.source_pending && !diagnostics.batch_pending);
    close_source_lifecycle(&presenter);
}

static void test_capacity_blocked_backend_and_reset(void) {
    FakeTrace trace = {
        .compile_ok = false,
        .compose_ok = true,
    };
    XgRenderWorkerServices worker = worker_services(&trace);
    XgRenderPresentationDiagnostics diagnostics;
    XgRenderSourceFrameSnapshot frame_snapshot;
    uint64_t expected_epoch;
    const XgSemanticPassRecord pass = { .pass_id = 1u };
    const XgSemanticSurfaceEdge edge = {
        .source_surface_id = 1u,
        .source_generation = 1u,
        .target_surface_id = 2u,
        .target_generation = 1u,
        .width = 1u,
        .height = 1u,
    };

    reset_source_lifecycle();
    assert(xg_render_source_frame_begin(&k_frame_description) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_pass(&pass) ==
           XG_RENDER_SOURCE_FRAME_OK);
    for (uint32_t index = 0u;
         index < XG_RENDER_SCENE_SURFACE_EDGE_CAPACITY; ++index)
        assert(xg_render_source_frame_append_surface_edge(&edge) ==
               XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_append_surface_edge(&edge) ==
           XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED);
    assert(xg_render_source_frame_complete() ==
           XG_RENDER_SOURCE_FRAME_INCOMPLETE);
    assert(source_boundary(1u, 100u) == XG_RENDER_SOURCE_FRAME_INCOMPLETE);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.rejected_commits == 1u);

    build_frame(60u);
    assert(source_boundary(2u, 200u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) ==
           XG_RENDER_WORKER_COMPILE_FAILED);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    assert(diagnostics.compile_failures == 1u);
    assert(!diagnostics.source_pending && !diagnostics.batch_pending);

    build_frame(61u);
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    expected_epoch = diagnostics.presentation_epoch + 1u;
    xg_render_semantic_presentation_reset();
    xg_render_source_frame_reset();
    claim_open_source_lifecycle();
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    xg_render_source_frame_snapshot(&frame_snapshot);
    assert(diagnostics.presentation_epoch == expected_epoch);
    assert(diagnostics.boundary_count == 0u);
    assert(!diagnostics.source_pending && !diagnostics.batch_pending);
    assert(!frame_snapshot.active && !frame_snapshot.blocked);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_EMPTY);
    assert_all_endpoints_released(&trace);
    {
        XgRenderPresenterServices presenter = presenter_services(&trace);
        close_source_lifecycle(&presenter);
    }
}

static void test_persistent_surface_graph_attaches_to_source_frame(void) {
    const uint8_t generated_bytes[16] = {
        1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
        9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u,
    };
    const uint8_t framebuffer_bytes[8] = {
        20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u,
    };
    XgRenderSurfacePublicationDescription generated_description = {
        .kind = XG_RENDER_RESOURCE_GENERATED_SURFACE,
        .format = XG_RENDER_SURFACE_RGBA8,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .owner_generation = k_frame_description.scene_generation,
        .width = 2u,
        .height = 2u,
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGBA8,
            .width = 2u,
            .height = 2u,
            .row_pitch = 8u,
        },
        .bytes = generated_bytes,
        .byte_count = sizeof(generated_bytes),
    };
    XgRenderSurfacePublicationDescription framebuffer_description = {
        .kind = XG_RENDER_RESOURCE_FRAMEBUFFER,
        .format = XG_RENDER_SURFACE_VRAM16,
        .provenance = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_NONE,
            .synthetic = true,
        },
        .owner_generation = k_frame_description.scene_generation,
        .width = 2u,
        .height = 2u,
        .descriptor = {
            .version = XG_RENDER_RESOURCE_DESCRIPTOR_VERSION,
            .pixel_format = XG_RENDER_RESOURCE_PIXEL_FORMAT_RGB555,
            .width = 2u,
            .height = 2u,
            .row_pitch = 4u,
        },
        .bytes = framebuffer_bytes,
        .byte_count = sizeof(framebuffer_bytes),
    };
    XgRenderSurfacePublication generated;
    XgRenderSurfacePublication framebuffer;
    XgRenderSurfacePublication movie_surface;
    XgSemanticSurfaceEdge edge;
    XgSemanticSurfaceEdge movie_edge;
    XgRenderMovieFrameDescription movie_description;
    XgRenderMovieFrameHandle movie_frame;
    XgRenderMovieFramePublication movie_publication;
    XgRenderSurfaceAttachmentDescription movie_attachment;
    XgRenderResourceView movie_view;
    XgSemanticPassRecord pass;
    XgRenderSourceFrameSnapshot snapshot;
    FakeTrace trace = { .compile_ok = true, .compose_ok = true };
    XgRenderWorkerServices worker = worker_services(&trace);

    reset_source_lifecycle();
    xg_render_movie_publisher_reset();
    generated_description.identity = surface_identity(1u);
    framebuffer_description.identity = surface_identity(91u);
    assert(xg_render_surface_graph_publish(
               &generated_description, &generated) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(xg_render_surface_graph_publish(
               &framebuffer_description, &framebuffer) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    edge = (XgSemanticSurfaceEdge){
        .source_surface_id = generated.handle.resource_id,
        .source_generation = generated.handle.generation,
        .target_surface_id = framebuffer.handle.resource_id,
        .target_generation = framebuffer.handle.generation,
        .kind = XG_SEMANTIC_SURFACE_SAMPLE,
        .width = 2u,
        .height = 2u,
    };
    assert(xg_render_surface_graph_append_edge(&edge) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    movie_description = (XgRenderMovieFrameDescription){
        .movie_id = 700u,
        .owner_kind = XG_RENDER_MOVIE_OWNER_STANDALONE,
        .owner_receipt = 103u,
        .owner_generation = k_frame_description.scene_generation,
        .guest_cycle = 100u,
        .width = 2u,
        .height = 2u,
        .expected_strips = 1u,
        .byte_count = sizeof(framebuffer_bytes),
    };
    {
        XgRenderResourceCapabilityMetadata metadata = {
            .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
            .receipt = movie_description.owner_receipt,
            .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
            .owner_kind = XG_RENDER_RESOURCE_OWNER_SOURCE,
            .owner_generation = movie_description.owner_generation,
            .source = {
                .source_class = XG_RENDER_RESOURCE_SOURCE_MOVIE_OWNER,
                .range_size = movie_description.byte_count,
                .range_content_digest = movie_description.owner_receipt,
            },
        };
        XgRenderResourceProvenance provenance;

        memcpy(metadata.source.identity.bytes, &movie_description.movie_id,
               sizeof(movie_description.movie_id));
        assert(xg_render_resource_capability_register(
                   &metadata, &provenance) ==
               XG_RENDER_RESOURCE_CAPABILITY_OK);
        movie_description.owner_capability = provenance.capability;
    }
    assert(xg_render_movie_frame_begin(&movie_description, &movie_frame) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_write_strip(
               movie_frame, 0u, 0u, framebuffer_bytes,
               sizeof(framebuffer_bytes)) == XG_RENDER_MOVIE_OK);
    assert(xg_render_movie_frame_publish(movie_frame, &movie_publication) ==
           XG_RENDER_MOVIE_OK);
    assert(xg_render_resource_view(movie_publication.surface, &movie_view) ==
           XG_RENDER_RESOURCE_OK);
    movie_attachment = (XgRenderSurfaceAttachmentDescription){
        .handle = movie_publication.surface,
        .kind = movie_view.kind,
        .owner_kind = movie_view.owner_kind,
        .format = XG_RENDER_SURFACE_VRAM16,
        .provenance = movie_view.provenance,
        .owner_generation = movie_view.owner_generation,
        .content_digest = movie_view.content_digest,
        .width = movie_description.width,
        .height = movie_description.height,
        .byte_count = movie_view.byte_count,
    };
    movie_edge = (XgSemanticSurfaceEdge){
        .source_surface_id = framebuffer.handle.resource_id,
        .source_generation = framebuffer.handle.generation,
        .target_surface_id = movie_publication.surface.resource_id,
        .target_generation = movie_publication.surface.generation,
        .kind = XG_SEMANTIC_SURFACE_MOVIE,
        .width = 2u,
        .height = 2u,
    };
    assert(xg_render_surface_graph_attach_resource_with_edge(
               &movie_attachment, &movie_edge, &movie_surface) ==
           XG_RENDER_SURFACE_GRAPH_OK);
    assert(movie_surface.handle.generation ==
           movie_publication.surface.generation);
    assert(xg_render_resource_view(movie_publication.surface, &movie_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(movie_view.retain_count == 0u);

    assert(xg_render_source_frame_begin(&k_frame_description) ==
           XG_RENDER_SOURCE_FRAME_OK);
    xg_render_source_frame_snapshot(&snapshot);
    assert(snapshot.resource_count == 0u);
    assert(snapshot.surface_edge_count == 0u);
    assert(xg_render_resource_view(movie_publication.surface, &movie_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(movie_view.retain_count == 0u);
    pass = (XgSemanticPassRecord){
        .pass_id = 1u,
        .target_surface_id = framebuffer.handle.resource_id,
        .target_generation = framebuffer.handle.generation,
        .store = true,
    };
    assert(xg_render_source_frame_append_pass(&pass) ==
           XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK);
    assert(source_boundary(1u, 100u) == XG_RENDER_SOURCE_FRAME_OK);
    assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
    assert(xg_render_resource_view(movie_publication.surface, &movie_view) ==
           XG_RENDER_RESOURCE_OK);
    assert(movie_view.retain_count == 1u);
    assert(trace.last_resource_count == 3u);
    assert(trace.last_surface_edge_count == 2u);
    xg_render_movie_publisher_reset();
    {
        XgRenderPresenterServices presenter = presenter_services(&trace);
        close_source_lifecycle(&presenter);
    }
    assert_all_endpoints_released(&trace);
}

int main(void) {
    test_mode_equivalence();
    test_boundary_diagnostics();
    test_epoch_invalidations();
    test_nonblocking_invalidation_and_stale_before_swap();
    test_reentrant_reset_callbacks_and_retirement_quiescence();
    test_capacity_blocked_backend_and_reset();
    test_persistent_surface_graph_attaches_to_source_frame();
    reset_source_lifecycle();
    close_source_lifecycle(NULL);
    return 0;
}
