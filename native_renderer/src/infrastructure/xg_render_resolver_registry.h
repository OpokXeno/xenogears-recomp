#ifndef XG_RENDER_RESOLVER_REGISTRY_H
#define XG_RENDER_RESOLVER_REGISTRY_H

#include "guest_render_native_stream.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderSharedPacketResolverServices
    XgRenderSharedPacketResolverServices;

typedef enum XgNativeResolveFamily {
    XG_NATIVE_RESOLVE_NONE = 0,
    XG_NATIVE_RESOLVE_MODEL_FT4,
    XG_NATIVE_RESOLVE_MODEL_FT3,
    XG_NATIVE_RESOLVE_ZOOM,
    XG_NATIVE_RESOLVE_FIELD_SPRITE,
    XG_NATIVE_RESOLVE_FIELD_POLYLINE,
    XG_NATIVE_RESOLVE_RESIDUAL,
    XG_NATIVE_RESOLVE_F4,
    XG_NATIVE_RESOLVE_SHARED,
    XG_NATIVE_RESOLVE_NONSHARED_MISS,
} XgNativeResolveFamily;

typedef bool (*XgRenderNativeProducerResolver)(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderSemantic *out_semantic);
typedef struct XgRenderResolverRegistryServices {
    XgRenderNativeProducerResolver model_ft4;
    XgRenderNativeProducerResolver model_ft3;
    XgRenderNativeProducerResolver zoom;
    XgRenderNativeProducerResolver field_sprite;
    XgRenderNativeProducerResolver field_polyline;
    XgRenderNativeProducerResolver residual;
    XgRenderNativeProducerResolver f4;
    const XgRenderSharedPacketResolverServices *shared_packet;
    uint64_t (*scene_generation)(void);
    uint64_t (*resource_generation)(void);
    uint32_t (*f4_source_count)(void);
} XgRenderResolverRegistryServices;

bool xg_render_resolver_registry_register(
    const XgRenderResolverRegistryServices *services);
bool xg_render_resolver_registry_unregister(
    const XgRenderResolverRegistryServices *services);
void xg_render_resolver_registry_reset(void);
void xg_render_resolver_registry_hint_put(
    uint64_t command_id, XgNativeResolveFamily family);
bool xg_render_resolver_registry_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderTransactionId *out_visual_id,
    GpuRenderSemantic *out_semantic);

#endif
