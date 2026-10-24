#include "xg_render_semantic_presentation.h"

#undef NDEBUG
#include <assert.h>

static uint64_t clock_ns(void *unused) {
    (void)unused;
    return UINT64_C(1000000000);
}

static uint64_t lag(const XgRenderPresenterServices *presenter,
                    uint64_t elapsed, uint64_t debt, bool rebase) {
    assert(xg_render_presenter_sync_source_clock(presenter,
        elapsed * UINT64_C(33868800) / UINT64_C(1000000000),
        UINT64_C(1000000000) + elapsed, debt, true, rebase));
    XgRenderPresentationDiagnostics diagnostics;
    xg_render_semantic_presentation_diagnostics(&diagnostics);
    return diagnostics.visual_lag_ns;
}

/* UI boundaries are one vblank apart, while a backend-approved motion
 * interval spans three vblanks. The next source must retain its entire phase
 * window even though the visible UI also supplies intervening endpoints. */
static uint64_t deadlines[3];
static unsigned compiled, released;

static XgRenderCompileResult compile_motion(XgRenderSourceCommitHandle commit,
        XgRenderCompiledEndpoint *endpoint, uint64_t *fence, void *unused) {
    (void)unused;
    XgRenderSourceCommitHeader header;
    assert(compiled < 3u);
    assert(xg_render_source_commit_header_copy(commit, &header) == XG_RENDER_SOURCE_COMMIT_OK);
    assert(xg_render_worker_source_deadline(commit, &deadlines[compiled]));
    *endpoint = (XgRenderCompiledEndpoint){
        .opaque_handle = compiled + 1u, .identity = header.identity,
        .digest = header.digest, .width = header.display.width,
        .height = header.display.height, .format = 1u, .backend_generation = 1u,
        .temporal_phase_count = 11u, .temporal_interval_ns = UINT64_C(50000000),
        .pixel_digest = compiled + 1u, .temporal_previous_pixel_digest = 1u,
    };
    *fence = ++compiled;
    return XG_RENDER_COMPILE_ENDPOINT_READY;
}
static XgRenderFenceStatus ready(uint64_t fence, void *unused) {
    (void)unused; assert(fence); return XG_RENDER_FENCE_READY;
}
static void discard(uint64_t fence, void *unused) { (void)unused; assert(fence); }
static void release(const XgRenderCompiledEndpoint *endpoint, void *unused) {
    (void)unused; assert(endpoint->opaque_handle); ++released;
}
static void test_authored_interval_budget(void) {
    const XgRenderWorkerServices worker = {
        .compile = compile_motion, .fence_status = ready,
        .discard_fence = discard, .release_endpoint = release,
    };
    const XgRenderPresenterServices presenter = {
        .owner_token = UINT64_C(0x534f55524345434c), .clock_ns = clock_ns,
    };
    const XgSemanticSceneIdentity scene = {.module = XG_SEMANTIC_MODULE_FIELD};
    const XgSemanticDisplayState display = {
        .width = 320u, .height = 216u, .temporal_hz = 240u,
        .aspect_num = 16u, .aspect_den = 9u,
    };
    xg_render_semantic_presentation_reset();
    assert(xg_render_presentation_lifecycle_claim_open(presenter.owner_token) == XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(lag(&presenter, 0u, 0u, true) == 0u);
    for (uint64_t index = 1u; index <= 3u; ++index) {
        XgPresentationIdentity identity;
        XgRenderSourceBuilder builder;
        XgRenderSourceCommitHandle commit;
        assert(xg_render_timeline_source_boundary(index, index * UINT64_C(564480)) == XG_RENDER_TIMELINE_OK);
        assert(xg_render_timeline_next_work_identity(1u, index * UINT64_C(564480), &identity) == XG_RENDER_TIMELINE_OK);
        assert(xg_render_source_commit_begin(&identity, &scene, &display, 1u, false, true, &builder) == XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_commit_set_native_work(builder, true) == XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_commit_seal(builder, &commit) == XG_RENDER_SOURCE_COMMIT_OK);
        assert(xg_render_source_queue_publish(commit) == XG_RENDER_TIMELINE_OK);
        for (unsigned attempt = 0u; attempt < 4u; ++attempt) {
            assert(xg_render_worker_compile_next(&worker) == XG_RENDER_WORKER_OK);
            XgRenderPresentationDiagnostics diagnostics;
            xg_render_semantic_presentation_diagnostics(&diagnostics);
            if (!diagnostics.source_queue_depth) break;
        }
        assert(compiled == index);
    }
    const uint64_t source_time = UINT64_C(1050000000);
    const uint64_t tick = UINT64_C(4166667);
    assert(deadlines[2] >= source_time + UINT64_C(100000000) + tick);
    assert(deadlines[2] <= source_time + UINT64_C(150000000) + tick);
    uint64_t epoch;
    assert(xg_render_presentation_lifecycle_close(presenter.owner_token, XG_RENDER_TIMELINE_RESET, &epoch) == XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(xg_render_presenter_drain_retirements(&presenter));
    assert(released == compiled);
    xg_render_semantic_presentation_reset();
}

int main(void) {
    const XgRenderPresenterServices presenter = {
        .owner_token = UINT64_C(0x534f55524345434c),
        .clock_ns = clock_ns,
    };
    xg_render_semantic_presentation_reset();
    assert(xg_render_presentation_lifecycle_claim_open(presenter.owner_token) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);

    /* Ordinary scheduling noise must not compress each source interval by
     * half a 240 Hz tick. Exercise repeated late/on-time guest quanta. */
    uint64_t previous = lag(&presenter, 0u, UINT64_C(6000000), true);
    for (uint64_t frame = 1u; frame <= 240u; ++frame) {
        const uint64_t current = lag(&presenter,
            frame * UINT64_C(16666667),
            frame & 1u ? 0u : UINT64_C(6000000), false);
        const uint64_t change = previous > current ? previous-current : current-previous;
        assert(change < UINT64_C(100000));
        previous = current;
    }

    /* A real stall still attacks immediately and sheds most of its debt at
     * the original rate; the slow noise band must not retain 250 ms. */
    assert(lag(&presenter, 0u, UINT64_C(250000000), true) == UINT64_C(250000000));
    const uint64_t recovered = lag(&presenter, UINT64_C(1000000000), 0u, false);
    assert(recovered > UINT64_C(150000000));
    assert(recovered < UINT64_C(200000000));

    /* A pause/debt rebase clears the envelope rather than carrying old lag
     * into the new source clock correspondence. */
    assert(lag(&presenter, UINT64_C(2000000000), 0u, true) == 0u);
    xg_render_semantic_presentation_reset();
    test_authored_interval_budget();
    return 0;
}
