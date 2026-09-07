#include "xg_render_instrumentation.h"

#include <stdint.h>
#include <stdio.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1;                                                            \
    }                                                                        \
} while (0)

void xg_render_instrumentation_test_seed(
    const PsxXgRenderAuthInstrumentation *seed, uint64_t sequence);

int main(void) {
    PsxXgRenderAuthInstrumentation seed;
    PsxXgRenderAuthInstrumentation snapshot;

    xg_render_instrumentation_reset();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.revision == 2u);
    CHECK(snapshot.counter_overflow_events == 0u);
    CHECK(!snapshot.counters_poisoned);

    seed = (PsxXgRenderAuthInstrumentation){
        .revision = 2u,
        .cold_hook_ingress_count = UINT64_MAX - 1u,
    };
    xg_render_instrumentation_test_seed(&seed, 1u);
    xg_render_instrumentation_record_cold_hook();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.cold_hook_ingress_count == UINT64_MAX);
    CHECK(snapshot.counter_overflow_events == 0u);
    CHECK(!snapshot.counters_poisoned);

    xg_render_instrumentation_record_cold_hook();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.cold_hook_ingress_count == UINT64_MAX);
    CHECK(snapshot.counter_overflow_events == 1u);
    CHECK(snapshot.counters_poisoned);

    xg_render_instrumentation_record_cold_hook();
    xg_render_instrumentation_record_flush_attempt();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.cold_hook_ingress_count == UINT64_MAX);
    CHECK(snapshot.native_ir_flush_attempt_count == 1u);
    CHECK(snapshot.counter_overflow_events == 2u);
    CHECK(snapshot.counters_poisoned);

    xg_render_instrumentation_reset();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.revision == 2u);
    CHECK(snapshot.cold_hook_ingress_count == 0u);
    CHECK(snapshot.last_progress_sequence == 0u);
    CHECK(snapshot.scene_boundary_count == 0u);
    CHECK(snapshot.counter_overflow_events == 0u);
    CHECK(!snapshot.counters_poisoned);

    seed = (PsxXgRenderAuthInstrumentation){ .revision = 2u };
    xg_render_instrumentation_test_seed(&seed, UINT64_MAX);
    xg_render_instrumentation_record_completed_proof();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.completed_proof_publication_count == 1u);
    CHECK(snapshot.last_publish_sequence == UINT64_MAX);
    CHECK(snapshot.counter_overflow_events == 1u);
    CHECK(snapshot.counters_poisoned);

    xg_render_instrumentation_record_completed_proof();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.completed_proof_publication_count == 2u);
    CHECK(snapshot.last_publish_sequence == UINT64_MAX);
    CHECK(snapshot.counter_overflow_events == 2u);
    CHECK(snapshot.counters_poisoned);

    seed = (PsxXgRenderAuthInstrumentation){
        .revision = 2u,
        .cold_hook_ingress_count = UINT64_MAX,
        .counter_overflow_events = UINT64_MAX - 1u,
    };
    xg_render_instrumentation_test_seed(&seed, 1u);
    xg_render_instrumentation_record_cold_hook();
    xg_render_instrumentation_record_cold_hook();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.cold_hook_ingress_count == UINT64_MAX);
    CHECK(snapshot.counter_overflow_events == UINT64_MAX);
    CHECK(snapshot.counters_poisoned);

    xg_render_instrumentation_reset();
    xg_render_instrumentation_snapshot(&snapshot);
    CHECK(snapshot.counter_overflow_events == 0u);
    CHECK(!snapshot.counters_poisoned);

    puts("xg_render_instrumentation: all tests passed");
    return 0;
}
