#ifndef XG_NATIVE_RENDER_BASELINE_H
#define XG_NATIVE_RENDER_BASELINE_H

#include "game_identity.h"
#include "native_render_baseline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_NATIVE_RENDER_BASELINE_ACTOR_COUNT_CAP = 256,
};

typedef enum XgNativeRenderBaselineReason {
    XG_NATIVE_RENDER_BASELINE_OK = 0,
    XG_NATIVE_RENDER_BASELINE_INVALID_ARGUMENT = 1,
    XG_NATIVE_RENDER_BASELINE_INVALID_IDENTITY = 2,
    XG_NATIVE_RENDER_BASELINE_INVALID_VBLANK_BOUND = 3,
    XG_NATIVE_RENDER_BASELINE_INVALID_ACTOR_COUNT = 4,
    XG_NATIVE_RENDER_BASELINE_INVALID_ACTOR_POINTER = 5,
    XG_NATIVE_RENDER_BASELINE_ACTOR_RANGE_OVERFLOW = 6,
} XgNativeRenderBaselineReason;

typedef struct XgNativeRenderBaselineResult {
    bool success;
    XgNativeRenderBaselineReason reason;
    uint64_t digest;
} XgNativeRenderBaselineResult;

XgNativeRenderBaselineResult xg_native_render_baseline_configure(
    const PsxGameIdentity *runtime_identity,
    uint32_t max_vblanks,
    NativeRenderBaselineConfig *out_config);
XgNativeRenderBaselineResult xg_native_render_baseline_sample(
    const uint8_t *guest_ram,
    size_t guest_ram_size);

#ifdef __cplusplus
}
#endif

#endif
