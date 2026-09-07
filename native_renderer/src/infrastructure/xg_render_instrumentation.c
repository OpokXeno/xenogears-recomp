#include "xg_render_instrumentation.h"

#include <stddef.h>
#include <stdatomic.h>

#ifdef XG_RENDER_INSTRUMENTATION_TESTING
void xg_render_instrumentation_test_seed(
    const PsxXgRenderAuthInstrumentation *seed, uint64_t sequence);
#endif

static PsxXgRenderAuthInstrumentation instrumentation = { .revision = 2u };
static atomic_flag instrumentation_guard = ATOMIC_FLAG_INIT;
static uint64_t next_sequence = 1u;

static bool add_u64_saturating(uint64_t *counter, uint64_t increment) {
    if (increment > UINT64_MAX - *counter) {
        *counter = UINT64_MAX;
        return false;
    }
    *counter += increment;
    return true;
}

static void record_counter_overflow(void) {
    instrumentation.counters_poisoned = true;
    (void)add_u64_saturating(&instrumentation.counter_overflow_events, 1u);
}

static void add_instrumentation_counter(
        uint64_t *counter, uint64_t increment) {
    if (!add_u64_saturating(counter, increment))
        record_counter_overflow();
}

static void lock_instrumentation(void) {
    while (atomic_flag_test_and_set_explicit(
               &instrumentation_guard, memory_order_acquire)) {}
}

static void unlock_instrumentation(void) {
    atomic_flag_clear_explicit(&instrumentation_guard, memory_order_release);
}

static uint64_t allocate_sequence(void) {
    const uint64_t sequence = next_sequence;

    add_instrumentation_counter(&next_sequence, 1u);
    return sequence;
}

void xg_render_instrumentation_reset(void) {
    lock_instrumentation();
    instrumentation = (PsxXgRenderAuthInstrumentation){ .revision = 2u };
    next_sequence = 1u;
    unlock_instrumentation();
}

void xg_render_instrumentation_record_reset(bool scene_boundary) {
    lock_instrumentation();
    if (scene_boundary)
        add_instrumentation_counter(
            &instrumentation.scene_boundary_count, 1u);
    else
        add_instrumentation_counter(&instrumentation.disarm_count, 1u);
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
        add_instrumentation_counter(
            &instrumentation.activation_physical_count, 1u);
        if (exact)
            add_instrumentation_counter(
                &instrumentation.activation_exact_count, 1u);
        break;
    case XG_RENDER_RUNTIME_VARIANT_ENTRY:
        add_instrumentation_counter(
            &instrumentation.entry_physical_count, 1u);
        if (exact)
            add_instrumentation_counter(
                &instrumentation.entry_exact_count, 1u);
        break;
    case XG_RENDER_RUNTIME_VARIANT_CAPTURE:
        add_instrumentation_counter(
            &instrumentation.capture_physical_count, 1u);
        if (exact)
            add_instrumentation_counter(
                &instrumentation.capture_exact_count, 1u);
        break;
    case XG_RENDER_RUNTIME_VARIANT_RETURN:
        add_instrumentation_counter(
            &instrumentation.return_physical_count, 1u);
        if (exact)
            add_instrumentation_counter(
                &instrumentation.return_exact_count, 1u);
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
    add_instrumentation_counter(
        &instrumentation.completed_proof_publication_count, 1u);
    instrumentation.last_publish_sequence = allocate_sequence();
    unlock_instrumentation();
}

void xg_render_instrumentation_record_cold_hook(void) {
    lock_instrumentation();
    add_instrumentation_counter(
        &instrumentation.cold_hook_ingress_count, 1u);
    unlock_instrumentation();
}

void xg_render_instrumentation_record_flush_attempt(void) {
    lock_instrumentation();
    add_instrumentation_counter(
        &instrumentation.native_ir_flush_attempt_count, 1u);
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
    add_instrumentation_counter(
        &instrumentation.native_ir_flush_failure_count, 1u);
    unlock_instrumentation();
}

void xg_render_instrumentation_snapshot(
        PsxXgRenderAuthInstrumentation *out_instrumentation) {
    if (out_instrumentation == NULL) return;
    lock_instrumentation();
    *out_instrumentation = instrumentation;
    unlock_instrumentation();
}

#ifdef XG_RENDER_INSTRUMENTATION_TESTING
void xg_render_instrumentation_test_seed(
        const PsxXgRenderAuthInstrumentation *seed,
        uint64_t sequence) {
    lock_instrumentation();
    instrumentation = *seed;
    next_sequence = sequence;
    unlock_instrumentation();
}
#endif
