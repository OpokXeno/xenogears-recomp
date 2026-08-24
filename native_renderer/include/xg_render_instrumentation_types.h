#ifndef XG_RENDER_INSTRUMENTATION_TYPES_H
#define XG_RENDER_INSTRUMENTATION_TYPES_H

#include <stdint.h>

typedef enum XgRenderRuntimeVariantEvent {
    XG_RENDER_RUNTIME_VARIANT_IGNORE = 0,
    XG_RENDER_RUNTIME_VARIANT_ACTIVATED,
    XG_RENDER_RUNTIME_VARIANT_ENTRY,
    XG_RENDER_RUNTIME_VARIANT_CAPTURE,
    XG_RENDER_RUNTIME_VARIANT_RETURN,
    XG_RENDER_RUNTIME_VARIANT_CONSUMED,
    XG_RENDER_RUNTIME_VARIANT_REJECT,
} XgRenderRuntimeVariantEvent;

typedef struct PsxXgRenderAuthInstrumentation {
    uint32_t revision;
    uint64_t cold_hook_ingress_count;
    uint64_t activation_physical_count;
    uint64_t activation_exact_count;
    uint64_t entry_physical_count;
    uint64_t entry_exact_count;
    uint64_t capture_physical_count;
    uint64_t capture_exact_count;
    uint64_t return_physical_count;
    uint64_t return_exact_count;
    uint64_t last_progress_sequence;
    uint64_t last_reset_sequence;
    uint64_t last_publish_sequence;
    uint64_t scene_boundary_count;
    uint64_t disarm_count;
    uint64_t completed_proof_publication_count;
    uint64_t native_ir_flush_attempt_count;
    uint64_t native_ir_flush_failure_count;
    uint64_t first_native_ir_flush_failure_index;
    uint32_t first_native_ir_flush_failure_reason;
    uint32_t first_native_ir_flush_failure_packet;
    uint32_t first_native_ir_flush_failure_status;
} PsxXgRenderAuthInstrumentation;

#endif
