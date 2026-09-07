#include "xg_render_semantic_presentation.h"
#include "xg_render_temporal_submission.h"

#include <assert.h>
#include <stdint.h>

typedef struct FakeBackend {
    uint32_t draw_count;
    uint32_t primitive_ids[4];
    GpuRenderTransactionStatus status;
} FakeBackend;

static FakeBackend g_backend;

GpuRenderTransactionStatus gr_draw_semantic_temporal_candidate(
        const GpuRenderSemantic *semantic,
        const GpuRenderTemporalCullPolicy *policy) {
    assert(semantic != NULL && policy != NULL);
    if (g_backend.status != GPU_RENDER_TRANSACTION_OK)
        return g_backend.status;
    assert(g_backend.draw_count < 4u);
    g_backend.primitive_ids[g_backend.draw_count++] =
        semantic->interpolation_identity.primitive_id;
    return GPU_RENDER_TRANSACTION_OK;
}

static GpuRenderSemantic semantic(uint32_t primitive_id) {
    GpuRenderSemantic value = {0};
    value.interpolation_identity.scene_id = 7u;
    value.interpolation_identity.producer_id = 8u;
    value.interpolation_identity.primitive_id = primitive_id;
    value.interpolation_identity.valid = 1u;
    return value;
}

static void reset_fixture(void) {
    g_backend = (FakeBackend){ .status = GPU_RENDER_TRANSACTION_OK };
    xg_render_semantic_presentation_reset();
    xg_render_temporal_submission_reset();
}

static void test_flush_filter_and_reset(void) {
    const GpuRenderTemporalCullPolicy policy = {0};
    GpuRenderSemantic first = semantic(1u);
    GpuRenderSemantic second = semantic(2u);

    reset_fixture();
    assert(xg_render_temporal_submission_stage(&first, &policy));
    assert(xg_render_temporal_submission_stage(&second, &policy));
    assert(xg_render_temporal_submission_cover_current(&second));
    assert(xg_render_temporal_submission_flush());
    assert(g_backend.draw_count == 1u);
    assert(g_backend.primitive_ids[0] == 1u);

    assert(xg_render_temporal_submission_stage(&second, &policy));
    xg_render_temporal_submission_reset();
    assert(xg_render_temporal_submission_flush());
    assert(g_backend.draw_count == 1u);
}

static void test_capacity_and_blocked_flush(void) {
    const GpuRenderTemporalCullPolicy policy = {0};
    GpuRenderSemantic first = semantic(1u);
    GpuRenderSemantic second = semantic(2u);
    GpuRenderSemantic third = semantic(3u);

    reset_fixture();
    assert(xg_render_temporal_submission_stage(&first, &policy));
    assert(xg_render_temporal_submission_stage(&second, &policy));
    assert(!xg_render_temporal_submission_stage(&third, &policy));
    assert(!xg_render_temporal_submission_flush());
    assert(g_backend.draw_count == 0u);

    xg_render_temporal_submission_reset();
    assert(xg_render_temporal_submission_stage(&third, &policy));
    assert(xg_render_temporal_submission_flush());
    assert(g_backend.draw_count == 1u);
    assert(g_backend.primitive_ids[0] == 3u);
}

static void test_backend_failure_is_fail_closed(void) {
    const GpuRenderTemporalCullPolicy policy = {0};
    GpuRenderSemantic first = semantic(1u);

    reset_fixture();
    assert(xg_render_temporal_submission_stage(&first, &policy));
    g_backend.status = GPU_RENDER_TRANSACTION_BACKEND_ERROR;
    assert(!xg_render_temporal_submission_flush());
    assert(g_backend.draw_count == 0u);
    xg_render_temporal_submission_reset();
    g_backend.status = GPU_RENDER_TRANSACTION_OK;
    assert(xg_render_temporal_submission_flush());
    assert(g_backend.draw_count == 0u);
}

static void test_epoch_invalidation_discards_candidates(void) {
    const XgRenderTimelineInvalidationReason reasons[] = {
        XG_RENDER_TIMELINE_RESTORE,
        XG_RENDER_TIMELINE_ROLLBACK,
        XG_RENDER_TIMELINE_DISC_CHANGE,
        XG_RENDER_TIMELINE_RESET,
    };
    const GpuRenderTemporalCullPolicy policy = {0};

    reset_fixture();
    for (uint32_t index = 0u;
         index < sizeof(reasons) / sizeof(reasons[0]); ++index) {
        GpuRenderSemantic candidate = semantic(10u + index);
        assert(xg_render_temporal_submission_stage(&candidate, &policy));
        (void)xg_render_timeline_invalidate(reasons[index]);
        assert(xg_render_temporal_submission_flush());
        assert(g_backend.draw_count == 0u);
    }
}

int main(void) {
    test_flush_filter_and_reset();
    test_capacity_and_blocked_flush();
    test_backend_failure_is_fail_closed();
    test_epoch_invalidation_discards_candidates();
    reset_fixture();
    return 0;
}
