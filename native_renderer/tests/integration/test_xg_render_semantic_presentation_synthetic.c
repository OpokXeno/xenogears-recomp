#include "xg_render_semantic_presentation.h"
#include "xg_render_source_frame.h"
#include "xg_render_surface_graph.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* Static/synthetic ownership fixture only: it does not execute gameplay. */

#define PRESENTER_OWNER UINT64_C(0x5033504841534553)

typedef struct CaptureTrace {
    XgSemanticModuleKind expected_module;
    uint32_t expected_pass_id;
    uint32_t expected_draw_count;
    uint32_t sealed_count;
    uint32_t notified_count;
    XgRenderSourceCommitHandle last_commit;
    bool accept_publication;
    bool expect_discontinuity;
} CaptureTrace;

static CaptureTrace g_trace;
static uint64_t g_vblank;

static XgRenderSourceFrameDescription description_for(
        XgSemanticModuleKind module, uint32_t generation,
        bool discontinuity) {
    return (XgRenderSourceFrameDescription){
        .scene = {
            .disc_id = 1u,
            .executable_identity = UINT64_C(0x534e544845544943),
            .primary_overlay_identity = (uint64_t)module + 1u,
            .authored_scene_id = 1u,
            .module = module,
        },
        .display = {
            .width = 320u,
            .height = 240u,
            .aspect_num = 4u,
            .aspect_den = 3u,
        },
        .scene_generation = generation,
        .source_interval_vblanks = 1u,
        .discontinuity = discontinuity,
        .temporally_eligible = true,
    };
}

static XgSemanticPassRecord pass_for(uint32_t pass_id) {
    return (XgSemanticPassRecord){
        .pass_id = pass_id,
        .load_operation = XG_SEMANTIC_PASS_LOAD,
        .store = true,
    };
}

static XgSemanticDrawRecord movie_companion_draw(uint32_t pass_id) {
    XgSemanticDrawRecord draw = {0};
    draw.order.pass_id = pass_id;
    draw.order.insertion_ordinal = 2u;
    draw.provenance.state_id.scene_epoch = 1u;
    draw.provenance.state_id.state_sequence = 1u;
    draw.provenance.slot_index = 1u;
    draw.has_provenance = true;
    draw.source_primitive_index = 8u;
    draw.primitive.triangle_count = 1u;
    draw.primitive.triangles[0].vertices[0].x = 8;
    return draw;
}

/* Synthetic phase producers have append authority only. */
static XgRenderSourceFrameResult synthetic_phase_emit(
        const XgRenderSourceFrameDescription *description,
        XgSemanticPassRecord *borrowed_pass) {
    const XgRenderSourceFrameFragment fragment = {
        .passes = borrowed_pass,
        .pass_count = 1u,
    };
    return xg_render_source_frame_append_fragment(description, &fragment);
}

static XgRenderSourceFrameResult synthetic_movie_companion_emit(
        const XgRenderSourceFrameDescription *primary,
        XgSemanticDrawRecord *borrowed_draw) {
    const XgRenderSourceFrameFragment fragment = {
        .draws = borrowed_draw,
        .draw_count = 1u,
    };
    return xg_render_source_frame_append_fragment(primary, &fragment);
}

static bool capture_sealed(XgRenderSourceCommitHandle commit,
                           const XgRenderSourceCommitHeader *header,
                           void *user_data) {
    CaptureTrace *trace = (CaptureTrace *)user_data;
    XgSemanticPassRecord copied_pass;
    XgSemanticDrawRecord copied_draw;

    assert(header != NULL);
    assert(header->state == XG_RENDER_SOURCE_SEALED);
    assert(header->scene.module == trace->expected_module);
    assert(header->pass_count == 1u);
    assert(header->draw_count == trace->expected_draw_count);
    assert(header->discontinuity || !trace->expect_discontinuity);
    if (trace->expect_discontinuity)
        assert(!header->temporally_eligible);
    assert(xg_render_source_commit_pass_copy(commit, 0u, &copied_pass) ==
           XG_RENDER_SOURCE_COMMIT_OK);
    assert(copied_pass.pass_id == trace->expected_pass_id);
    if (trace->expected_draw_count != 0u) {
        assert(xg_render_source_commit_draw_copy(commit, 0u, &copied_draw) ==
               XG_RENDER_SOURCE_COMMIT_OK);
        assert(copied_draw.source_primitive_index == 8u);
        assert(copied_draw.primitive.triangles[0].vertices[0].x == 8);
    }
    if (trace->sealed_count != 0u) {
        assert(commit.slot != trace->last_commit.slot ||
               commit.generation != trace->last_commit.generation);
    }
    trace->last_commit = commit;
    trace->sealed_count++;
    return trace->accept_publication;
}

static void notify_published(void *user_data) {
    CaptureTrace *trace = (CaptureTrace *)user_data;
    trace->notified_count++;
}

static XgRenderSourceFrameResult seal_at_boundary(void) {
    ++g_vblank;
    assert(xg_render_timeline_source_boundary(g_vblank, g_vblank * 100u) ==
           XG_RENDER_TIMELINE_OK);
    return xg_render_source_frame_publish_boundary();
}

static void test_primary_phases_have_one_runtime_commit(void) {
    static const XgSemanticModuleKind modules[] = {
        XG_SEMANTIC_MODULE_FIELD,
        XG_SEMANTIC_MODULE_WORLD,
        XG_SEMANTIC_MODULE_RESIDENT,
        XG_SEMANTIC_MODULE_MENU,
        XG_SEMANTIC_MODULE_BATTLE,
        XG_SEMANTIC_MODULE_BATTLING,
        XG_SEMANTIC_MODULE_MOVIE,
    };

    for (uint32_t index = 0u;
         index < sizeof(modules) / sizeof(modules[0]); ++index) {
        XgRenderSourceFrameDescription description =
            description_for(modules[index], 7u, false);
        XgSemanticPassRecord pass = pass_for(index + 1u);
        const uint32_t before = g_trace.sealed_count;

        g_trace.expected_module = modules[index];
        g_trace.expected_pass_id = index + 1u;
        g_trace.expected_draw_count = 0u;
        g_trace.expect_discontinuity = false;
        g_trace.accept_publication = false;
        assert(synthetic_phase_emit(&description, &pass) ==
               XG_RENDER_SOURCE_FRAME_OK);
        memset(&pass, 0xa5, sizeof(pass));
        assert(xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK);
        assert(seal_at_boundary() == XG_RENDER_SOURCE_FRAME_CAPTURE_FAILED);
        assert(g_trace.sealed_count == before + 1u);
        assert(g_trace.notified_count == 0u);
        assert(xg_render_source_frame_publish_boundary() ==
               XG_RENDER_SOURCE_FRAME_EMPTY);
    }
}

static void test_capacity_rollback_and_stale_generation(void) {
    XgRenderSourceFrameDescription description =
        description_for(XG_SEMANTIC_MODULE_FIELD, 7u, false);
    XgRenderSourceFrameDescription stale = description;
    XgSemanticPassRecord primary = pass_for(1u);
    XgSemanticPassRecord overflow[XG_RENDER_SCENE_PASS_CAPACITY];
    XgRenderSourceFrameFragment fragment;
    XgRenderSourceFrameSnapshot before;
    XgRenderSourceFrameSnapshot after;

    assert(synthetic_phase_emit(&description, &primary) ==
           XG_RENDER_SOURCE_FRAME_OK);
    xg_render_source_frame_snapshot(&before);
    for (uint32_t index = 0u; index < XG_RENDER_SCENE_PASS_CAPACITY; ++index)
        overflow[index] = pass_for(index);
    fragment = (XgRenderSourceFrameFragment){
        .passes = overflow,
        .pass_count = XG_RENDER_SCENE_PASS_CAPACITY,
    };
    assert(xg_render_source_frame_append_fragment(&description, &fragment) ==
           XG_RENDER_SOURCE_FRAME_CAPACITY_EXCEEDED);
    xg_render_source_frame_snapshot(&after);
    assert(after.active == before.active && after.complete == before.complete &&
           after.blocked == before.blocked &&
           after.pass_count == before.pass_count &&
           after.draw_count == before.draw_count);

    stale.scene_generation = 6u;
    fragment = (XgRenderSourceFrameFragment){0};
    assert(xg_render_source_frame_append_fragment(&stale, &fragment) ==
           XG_RENDER_SOURCE_FRAME_REJECTED);
    xg_render_source_frame_snapshot(&after);
    assert(after.scene_generation == 7u && after.pass_count == 1u &&
           !after.complete && !after.blocked);
    xg_render_source_frame_reset();
}

static void test_field_movie_companion_is_one_discontinuous_publication(void) {
    XgRenderSourceFrameDescription field =
        description_for(XG_SEMANTIC_MODULE_FIELD, 7u, true);
    XgSemanticPassRecord pass = pass_for(5u);
    XgSemanticDrawRecord movie = movie_companion_draw(5u);
    XgRenderSourceFrameSnapshot snapshot;
    XgRenderPresentationDiagnostics before;
    XgRenderPresentationDiagnostics after;
    const uint32_t sealed_before = g_trace.sealed_count;

    g_trace.expected_module = XG_SEMANTIC_MODULE_FIELD;
    g_trace.expected_pass_id = 5u;
    g_trace.expected_draw_count = 1u;
    g_trace.expect_discontinuity = true;
    g_trace.accept_publication = true;
    assert(synthetic_phase_emit(&field, &pass) == XG_RENDER_SOURCE_FRAME_OK);
    assert(synthetic_movie_companion_emit(&field, &movie) ==
           XG_RENDER_SOURCE_FRAME_OK);
    memset(&pass, 0xa5, sizeof(pass));
    memset(&movie, 0xa5, sizeof(movie));
    xg_render_source_frame_snapshot(&snapshot);
    assert(snapshot.active && !snapshot.complete && !snapshot.blocked);
    assert(snapshot.pass_count == 1u && snapshot.draw_count == 1u);
    assert(xg_render_source_frame_complete() == XG_RENDER_SOURCE_FRAME_OK);

    xg_render_semantic_presentation_diagnostics(&before);
    assert(seal_at_boundary() == XG_RENDER_SOURCE_FRAME_OK);
    xg_render_semantic_presentation_diagnostics(&after);
    assert(g_trace.sealed_count == sealed_before + 1u);
    assert(g_trace.notified_count == 1u);
    assert(after.published_commits == before.published_commits + 1u);
    assert(after.source_queue_depth == before.source_queue_depth + 1u);

    /* The consumed runtime frame cannot seal or publish a duplicate commit. */
    assert(xg_render_source_frame_publish_boundary() ==
           XG_RENDER_SOURCE_FRAME_EMPTY);
    xg_render_semantic_presentation_diagnostics(&before);
    assert(before.published_commits == after.published_commits);
    assert(before.source_queue_depth == after.source_queue_depth);
    assert(g_trace.sealed_count == sealed_before + 1u);
    assert(g_trace.notified_count == 1u);
}

int main(void) {
    const XgRenderSourceFrameHostCallbacks callbacks = {
        .sealed_capture = capture_sealed,
        .published_notify = notify_published,
        .user_data = &g_trace,
    };
    uint64_t closed_epoch = 0u;

    xg_render_semantic_presentation_reset();
    xg_render_source_frame_reset();
    xg_render_surface_graph_reset();
    assert(xg_render_presentation_lifecycle_claim_open(PRESENTER_OWNER) ==
           XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(xg_render_source_frame_configure_host_callbacks(&callbacks));

    test_primary_phases_have_one_runtime_commit();
    test_capacity_rollback_and_stale_generation();
    test_field_movie_companion_is_one_discontinuous_publication();
    assert(xg_render_presentation_lifecycle_close(
               PRESENTER_OWNER, XG_RENDER_TIMELINE_RESET,
               &closed_epoch) == XG_RENDER_PRESENTATION_LIFECYCLE_OK);
    assert(closed_epoch != 0u);
    return 0;
}
