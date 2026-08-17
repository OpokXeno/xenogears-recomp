#include "native_render_baseline.h"
#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "gpu_render.h"

#include <string.h>

uint64_t s_frame_count = 0;

void gl_renderer_native_midpoint_diag(
        GlRendererNativeMidpointDiagnostics *out_diagnostics) {
    if (out_diagnostics != NULL)
        memset(out_diagnostics, 0, sizeof(*out_diagnostics));
}

uint64_t gl_renderer_pres_total(void) { return 0u; }

int gl_renderer_pres_get(uint64_t sequence, GlPresEvent *out_event) {
    (void)sequence;
    (void)out_event;
    return 0;
}

size_t gl_renderer_retired_failure_events(
        GlRendererRetiredFailureEvent *out_events, size_t capacity) {
    (void)out_events;
    (void)capacity;
    return 0u;
}

uint64_t gl_renderer_retired_failure_event_total(void) { return 0u; }

uint64_t gl_renderer_retired_failure_event_overflow(void) { return 0u; }

void native_render_baseline_runtime_reset(void) {}
void native_render_baseline_runtime_arm(void) {}
NativeRenderBaselineReason native_render_baseline_runtime_observe(
        NativeRenderBaselineSnapshot *snapshot) {
    (void)snapshot;
    return NATIVE_RENDER_BASELINE_COMPLETE;
}

void gte_attribution_set_enabled(bool enabled) { (void)enabled; }
void gte_attribution_reset(void) {}
GteAttributionResult gte_attribution_producer_begin(
    const GteAttributionProducerContext *context) {
    (void)context;
    return GTE_ATTRIBUTION_OK;
}
GteAttributionResult gte_attribution_producer_end(void) {
    return GTE_ATTRIBUTION_OK;
}

GteAttributionResult gte_attribution_snapshot(
        GteAttributionSnapshot *out_snapshot,
        GteAttributionContextCounter *context_counters,
        size_t context_capacity,
        GteAttributionSiteCounter *site_counters,
        size_t site_capacity) {
    (void)context_counters;
    (void)context_capacity;
    (void)site_counters;
    (void)site_capacity;
    if (out_snapshot == NULL) return GTE_ATTRIBUTION_INVALID_ARGUMENT;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    return GTE_ATTRIBUTION_OK;
}

void gpu_get_draw_state(GpuDrawState *out) {
    if (out != NULL) memset(out, 0, sizeof(*out));
}

void gpu_native_environment_get(GpuNativeDrawEnvironment *out) {
    if (out != NULL) memset(out, 0, sizeof(*out));
}

void gpu_native_interpolation_scene_boundary(uint64_t scene_id) {
    (void)scene_id;
}

int psx_ws_x_margin(void) { return 0; }
uint32_t psx_ws_xclip_bound(uint32_t vanilla) { return vanilla; }

GpuRenderTransactionStatus gr_transaction_begin(
    GpuRenderTransactionId transaction_id,
    uint64_t vram_mutation_serial) {
    (void)transaction_id;
    (void)vram_mutation_serial;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_ordering_barrier(
    GpuRenderTransactionId transaction_id) {
    (void)transaction_id;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_draw_semantic(
    GpuRenderTransactionId transaction_id,
    const GpuRenderSemantic *semantic) {
    (void)transaction_id;
    (void)semantic;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_record_interpolation_anchors(
        const GpuRenderInterpolationVertexAnchor *anchors, size_t count) {
    (void)anchors;
    (void)count;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_draw_semantic_temporal_candidate(
        const GpuRenderSemantic *semantic,
        const GpuRenderTemporalCullPolicy *policy) {
    (void)semantic;
    (void)policy;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_commit_validate(
    GpuRenderTransactionId transaction_id,
    uint64_t current_vram_mutation_serial,
    const GpuRenderPresent *present) {
    (void)transaction_id;
    (void)current_vram_mutation_serial;
    (void)present;
    return GPU_RENDER_TRANSACTION_READY;
}

GpuRenderTransactionStatus gr_rollback(
    GpuRenderTransactionId transaction_id) {
    (void)transaction_id;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_deferred_candidate_capture(
    GpuRenderTransactionId transaction_id,
    GpuRenderDeferredCandidateToken *out_candidate_token) {
    (void)transaction_id;
    if (out_candidate_token != NULL)
        *out_candidate_token = GPU_RENDER_DEFERRED_CANDIDATE_NONE;
    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
}

GpuRenderTransactionStatus gr_deferred_candidate_discard(
    GpuRenderDeferredCandidateToken candidate_token) {
    (void)candidate_token;
    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
}

GpuRenderTransactionStatus gr_deferred_transaction_begin(
    GpuRenderTransactionId transaction_id,
    uint64_t vram_mutation_serial,
    GpuRenderDeferredCandidateToken candidate_token) {
    (void)transaction_id;
    (void)vram_mutation_serial;
    (void)candidate_token;
    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
}
