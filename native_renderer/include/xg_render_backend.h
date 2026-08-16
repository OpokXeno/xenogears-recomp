#ifndef XG_RENDER_BACKEND_H
#define XG_RENDER_BACKEND_H

#include "gpu_render.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XG_RENDER_BACKEND_NO_FINAL_ORDINAL UINT32_MAX

typedef enum XgRenderBackendStatus {
    XG_RENDER_BACKEND_OK = 0,
    XG_RENDER_BACKEND_INVALID_ARGUMENT = 1,
    XG_RENDER_BACKEND_IR_UNAVAILABLE = 2,
    XG_RENDER_BACKEND_IR_NOT_FINALIZED = 3,
    XG_RENDER_BACKEND_STALE_VRAM_SERIAL = 4,
    XG_RENDER_BACKEND_MISSING_NATIVE_ITEMS = 5,
    XG_RENDER_BACKEND_MISSING_COMPATIBILITY_CALLBACK = 6,
    XG_RENDER_BACKEND_INVALID_ORDER = 7,
    XG_RENDER_BACKEND_INVALID_ITEM = 8,
    XG_RENDER_BACKEND_INVALID_MATERIAL = 9,
    XG_RENDER_BACKEND_UNSUPPORTED_MATERIAL = 10,
    XG_RENDER_BACKEND_FIXED_POINT_CONVERSION_FAILED = 11,
    XG_RENDER_BACKEND_TRANSACTION_BEGIN_FAILED = 12,
    XG_RENDER_BACKEND_ORDERING_BARRIER_FAILED = 13,
    XG_RENDER_BACKEND_COMPATIBILITY_FAILED = 14,
    XG_RENDER_BACKEND_NATIVE_DRAW_FAILED = 15,
    XG_RENDER_BACKEND_COMMIT_FAILED = 16,
    XG_RENDER_BACKEND_ROLLBACK_FAILED = 17,
} XgRenderBackendStatus;

typedef enum XgRenderBackendFallbackReason {
    XG_RENDER_BACKEND_FALLBACK_NONE = 0,
    XG_RENDER_BACKEND_FALLBACK_INVALID_REQUEST = 1,
    XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE = 2,
    XG_RENDER_BACKEND_FALLBACK_STALE_VRAM_SERIAL = 3,
    XG_RENDER_BACKEND_FALLBACK_MISSING_NATIVE_ITEMS = 4,
    XG_RENDER_BACKEND_FALLBACK_MISSING_COMPATIBILITY = 5,
    XG_RENDER_BACKEND_FALLBACK_MATERIAL = 6,
    XG_RENDER_BACKEND_FALLBACK_FIXED_POINT = 7,
    XG_RENDER_BACKEND_FALLBACK_TRANSACTION = 8,
} XgRenderBackendFallbackReason;

typedef bool (*XgRenderBackendCompatibilityCallback)(
    const XgRenderIrCompatibilityItem *item,
    void *user_data);

typedef struct XgRenderBackendSubmitInfo {
    uint64_t current_vram_mutation_serial;
    const GpuRenderPresent *present;
    /* Called in final-ordinal order while the generic transaction is open. */
    XgRenderBackendCompatibilityCallback compatibility_callback;
    void *compatibility_user_data;
} XgRenderBackendSubmitInfo;

typedef struct XgRenderBackendResult {
    XgRenderBackendStatus status;
    XgRenderBackendFallbackReason fallback_reason;
    GpuRenderTransactionStatus transaction_status;
    size_t items_preflighted;
    size_t compatibility_items_submitted;
    size_t native_items_submitted;
    size_t semantic_primitives_submitted;
    uint32_t failed_final_ordinal;
    bool transaction_began;
    bool rollback_attempted;
    bool rollback_succeeded;
} XgRenderBackendResult;

XgRenderBackendStatus xg_render_backend_translate_primitive(
    const XgRenderIrNativePrimitive *primitive,
    GpuRenderSemantic *out_semantic);
XgRenderBackendStatus xg_render_backend_translate_anchor(
    const XgRenderIrMaterialState *material,
    const XgRenderIrVertex *vertex,
    uint64_t scene_id,
    uint32_t producer_id,
    GpuRenderInterpolationVertexAnchor *out_anchor);
XgRenderBackendResult xg_render_backend_submit(
    const XgRenderIr *ir,
    const XgRenderBackendSubmitInfo *submit_info);

#ifdef __cplusplus
}
#endif

#endif
