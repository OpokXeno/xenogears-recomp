#ifndef XG_RENDER_RESIDUAL_H
#define XG_RENDER_RESIDUAL_H

#include "xg_render_invalidation_event.h"

#include "guest_render_types.h"
#include "xg_render_producer_lifecycle.h"

#include <stdint.h>

typedef struct CPUState CPUState;

typedef enum XgRenderResidualCaptureKind {
    XG_RENDER_RESIDUAL_CAPTURE_CLEAR_TILE = 0,
    XG_RENDER_RESIDUAL_CAPTURE_LOGO_SPRITE,
    XG_RENDER_RESIDUAL_CAPTURE_FULLSCREEN_TILE,
    XG_RENDER_RESIDUAL_CAPTURE_FADE_TILES,
    XG_RENDER_RESIDUAL_CAPTURE_TILE_WRITE,
    XG_RENDER_RESIDUAL_CAPTURE_STATIC_GOURAUD,
    XG_RENDER_RESIDUAL_CAPTURE_PROJECTED_GOURAUD,
} XgRenderResidualCaptureKind;

typedef struct XgRenderResidualCaptureRequest {
    XgRenderResidualCaptureKind kind;
    CPUState *cpu;
    uint32_t command_address;
    uint32_t writer_pc;
    uint8_t color;
} XgRenderResidualCaptureRequest;

void xg_render_residual_reset(void);
void xg_render_residual_retain_resident(void);
void xg_render_residual_invalidate(uint32_t address, uint32_t size);
void xg_render_residual_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_render_residual_capture(
    const XgRenderResidualCaptureRequest *request,
    GuestRenderRenderMode render_mode,
    const XgRenderProducerLifecycleServices *services);
bool xg_render_residual_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
    const XgRenderProducerLifecycleServices *services);

#endif
