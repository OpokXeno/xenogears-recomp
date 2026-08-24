#include "gpu_gl_renderer.h"

#include <stdint.h>
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
