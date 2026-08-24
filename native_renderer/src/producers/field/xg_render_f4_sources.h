#ifndef XG_RENDER_F4_SOURCES_H
#define XG_RENDER_F4_SOURCES_H

#include "xg_render_invalidation_event.h"

#include "guest_render_types.h"
#include "guest_render_native_stream.h"
#include "xg_host_3d.h"
#include "xg_render_producer_lifecycle.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

typedef struct XgRenderF4GuestMemoryCallbacks {
    bool (*data_range_is_valid)(
        uint32_t address, uint32_t size, uint32_t alignment,
        bool allow_scratchpad);
    bool (*word_address_is_valid)(uint32_t address);
    bool (*stack_address_is_valid)(uint32_t address);
    bool (*vector_address_is_valid)(uint32_t address);
    void (*capture_projection)(
        const CPUState *cpu, XgHost3dProjection *projection);
} XgRenderF4GuestMemoryCallbacks;

typedef struct XgRenderF4InterpolationCallbacks {
    uint64_t (*scene_generation)(void);
} XgRenderF4InterpolationCallbacks;

typedef struct XgRenderF4ResourceCallbacks {
    void (*watch)(uint32_t address, uint32_t size);
} XgRenderF4ResourceCallbacks;

typedef struct XgRenderF4SourceServices {
    const XgRenderProducerLifecycleServices *lifecycle;
    XgRenderF4GuestMemoryCallbacks guest_memory;
    XgRenderF4InterpolationCallbacks interpolation;
    XgRenderF4ResourceCallbacks resources;
} XgRenderF4SourceServices;

void xg_render_f4_sources_clear(void);
void xg_render_f4_sources_reset(void);
bool xg_render_f4_sources_writer_is_authorized(
    const GuestRenderNativeStreamMissContext *context);
uint32_t xg_render_f4_sources_count(void);
void xg_render_f4_sources_invalidate_overlapping(
    uint32_t address, uint32_t size);
void xg_render_f4_sources_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

bool xg_render_f4_sources_capture_battle_fader(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderF4SourceServices *services);
bool xg_render_f4_sources_capture_fixed_2a(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderF4SourceServices *services);
bool xg_render_f4_sources_capture_projected_2a(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderF4SourceServices *services);
void xg_render_f4_sources_observe_2a_ot(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderF4SourceServices *services);
bool xg_render_f4_sources_capture_field_f4(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    const XgRenderF4SourceServices *services);

bool xg_render_f4_sources_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
    const XgRenderF4SourceServices *services);

#endif
