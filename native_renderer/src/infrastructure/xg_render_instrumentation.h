#ifndef XG_RENDER_INSTRUMENTATION_H
#define XG_RENDER_INSTRUMENTATION_H

#include "xg_render_invalidation_event.h"
#include "xg_render_instrumentation_types.h"

#include <stdbool.h>
#include <stdint.h>

/* Thread-safe telemetry for authentication progress and native IR flushing. */
void xg_render_instrumentation_reset(void);
void xg_render_instrumentation_record_reset(bool scene_boundary);
void xg_render_instrumentation_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_render_instrumentation_record_variant_progress(
    XgRenderRuntimeVariantEvent event, bool exact);
void xg_render_instrumentation_record_completed_proof(void);
void xg_render_instrumentation_record_cold_hook(void);
void xg_render_instrumentation_record_flush_attempt(void);
void xg_render_instrumentation_record_flush_failure(
    uint32_t reason, uint64_t index, uint32_t packet_address, uint32_t status);
void xg_render_instrumentation_snapshot(
    PsxXgRenderAuthInstrumentation *out_instrumentation);

#endif
