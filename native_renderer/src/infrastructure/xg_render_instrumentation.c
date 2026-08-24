#include "xg_render_instrumentation.h"

#include <stddef.h>
#include <stdatomic.h>

static PsxXgRenderAuthInstrumentation instrumentation = { .revision = 1u };
static atomic_flag instrumentation_guard = ATOMIC_FLAG_INIT;
static uint64_t next_sequence = 1u;

static void lock_instrumentation(void) {
    while (atomic_flag_test_and_set_explicit(
               &instrumentation_guard, memory_order_acquire)) {}
}

static void unlock_instrumentation(void) {
    atomic_flag_clear_explicit(&instrumentation_guard, memory_order_release);
}

static uint64_t allocate_sequence(void) {
    return next_sequence++;
}

void xg_render_instrumentation_reset(void) {
    lock_instrumentation();
    instrumentation = (PsxXgRenderAuthInstrumentation){ .revision = 1u };
    next_sequence = 1u;
    unlock_instrumentation();
}

void xg_render_instrumentation_record_reset(bool scene_boundary) {
    lock_instrumentation();
    if (scene_boundary)
        ++instrumentation.scene_boundary_count;
    else
        ++instrumentation.disarm_count;
    instrumentation.last_reset_sequence = allocate_sequence();
    unlock_instrumentation();
}

void xg_render_instrumentation_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY)
        xg_render_instrumentation_record_reset(true);
    else if (event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST)
        xg_render_instrumentation_record_reset(false);
    else if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_instrumentation_reset();
}

void xg_render_instrumentation_record_variant_progress(
        XgRenderRuntimeVariantEvent event, bool exact) {
    lock_instrumentation();
    switch (event) {
    case XG_RENDER_RUNTIME_VARIANT_ACTIVATED:
        ++instrumentation.activation_physical_count;
        if (exact) ++instrumentation.activation_exact_count;
        break;
    case XG_RENDER_RUNTIME_VARIANT_ENTRY:
        ++instrumentation.entry_physical_count;
        if (exact) ++instrumentation.entry_exact_count;
        break;
    case XG_RENDER_RUNTIME_VARIANT_CAPTURE:
        ++instrumentation.capture_physical_count;
        if (exact) ++instrumentation.capture_exact_count;
        break;
    case XG_RENDER_RUNTIME_VARIANT_RETURN:
        ++instrumentation.return_physical_count;
        if (exact) ++instrumentation.return_exact_count;
        break;
    default:
        unlock_instrumentation();
        return;
    }
    instrumentation.last_progress_sequence = allocate_sequence();
    unlock_instrumentation();
}

void xg_render_instrumentation_record_completed_proof(void) {
    lock_instrumentation();
    ++instrumentation.completed_proof_publication_count;
    instrumentation.last_publish_sequence = allocate_sequence();
    unlock_instrumentation();
}

void xg_render_instrumentation_record_cold_hook(void) {
    lock_instrumentation();
    ++instrumentation.cold_hook_ingress_count;
    unlock_instrumentation();
}

void xg_render_instrumentation_record_flush_attempt(void) {
    lock_instrumentation();
    ++instrumentation.native_ir_flush_attempt_count;
    unlock_instrumentation();
}

void xg_render_instrumentation_record_flush_failure(
        uint32_t reason, uint64_t index,
        uint32_t packet_address, uint32_t status) {
    lock_instrumentation();
    if (instrumentation.native_ir_flush_failure_count == 0u) {
        instrumentation.first_native_ir_flush_failure_index = index;
        instrumentation.first_native_ir_flush_failure_reason = reason;
        instrumentation.first_native_ir_flush_failure_packet = packet_address;
        instrumentation.first_native_ir_flush_failure_status = status;
    }
    ++instrumentation.native_ir_flush_failure_count;
    unlock_instrumentation();
}

void xg_render_instrumentation_snapshot(
        PsxXgRenderAuthInstrumentation *out_instrumentation) {
    if (out_instrumentation == NULL) return;
    lock_instrumentation();
    *out_instrumentation = instrumentation;
    unlock_instrumentation();
}
