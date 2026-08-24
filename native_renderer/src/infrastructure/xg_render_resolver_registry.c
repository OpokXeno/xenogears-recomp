#include "xg_render_resolver_registry.h"

#include "xg_render_shared_packet_resolver.h"

#include <stddef.h>
#include <string.h>

typedef struct XgNativeResolveHint {
    uint32_t command_id;
    uint32_t word_count;
    uint64_t container_id;
    uint64_t resource_generation;
    uint8_t family;
    uint8_t opcode;
    uint8_t source_kind;
    bool valid;
} XgNativeResolveHint;

#define XG_NATIVE_RESOLVE_HINT_CAPACITY 1024u

static XgRenderResolverRegistryServices registry_services;
static XgNativeResolveHint native_resolve_hints[
    XG_NATIVE_RESOLVE_HINT_CAPACITY];

static bool services_are_complete(
        const XgRenderResolverRegistryServices *services) {
    return services != NULL && services->model_ft4 != NULL &&
        services->model_ft3 != NULL && services->zoom != NULL &&
        services->field_sprite != NULL &&
        services->field_polyline != NULL && services->residual != NULL &&
        services->f4 != NULL && services->shared_packet != NULL &&
        services->scene_generation != NULL &&
        services->resource_generation != NULL &&
        services->f4_source_count != NULL;
}

static bool services_equal(const XgRenderResolverRegistryServices *left,
                           const XgRenderResolverRegistryServices *right) {
    return left != NULL && right != NULL &&
        left->model_ft4 == right->model_ft4 &&
        left->model_ft3 == right->model_ft3 && left->zoom == right->zoom &&
        left->field_sprite == right->field_sprite &&
        left->field_polyline == right->field_polyline &&
        left->residual == right->residual && left->f4 == right->f4 &&
        left->shared_packet == right->shared_packet &&
        left->scene_generation == right->scene_generation &&
        left->resource_generation == right->resource_generation &&
        left->f4_source_count == right->f4_source_count;
}

bool xg_render_resolver_registry_register(
        const XgRenderResolverRegistryServices *services) {
    if (!services_are_complete(services) ||
        services_are_complete(&registry_services))
        return false;
    registry_services = *services;
    return true;
}

bool xg_render_resolver_registry_unregister(
        const XgRenderResolverRegistryServices *services) {
    if (!services_equal(&registry_services, services)) return false;
    memset(&registry_services, 0, sizeof(registry_services));
    memset(native_resolve_hints, 0, sizeof(native_resolve_hints));
    return true;
}

void xg_render_resolver_registry_reset(void) {
    memset(native_resolve_hints, 0, sizeof(native_resolve_hints));
}

static uint32_t native_resolve_hint_index(uint32_t command_id) {
    uint32_t hash = command_id * UINT32_C(2654435761);

    hash ^= hash >> 16u;
    return hash & (XG_NATIVE_RESOLVE_HINT_CAPACITY - 1u);
}

static XgNativeResolveFamily native_resolve_hint_find(
        const GuestRenderNativeStreamMissContext *context) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    const uint32_t key = (uint32_t)command_id & UINT32_C(0x1fffffff);
    const XgNativeResolveHint *hint = &native_resolve_hints[
        native_resolve_hint_index(key)];

    if (command_id > UINT32_MAX || !hint->valid || hint->command_id != key)
        return XG_NATIVE_RESOLVE_NONE;
    if (hint->family == XG_NATIVE_RESOLVE_NONSHARED_MISS &&
        (hint->resource_generation !=
             registry_services.resource_generation() ||
         context == NULL || hint->container_id != context->container_id ||
         hint->word_count != context->word_count ||
         hint->opcode != context->opcode ||
         hint->source_kind != (uint8_t)context->source_kind))
        return XG_NATIVE_RESOLVE_NONE;
    return (XgNativeResolveFamily)hint->family;
}

void xg_render_resolver_registry_hint_put(
        uint64_t command_id, XgNativeResolveFamily family) {
    const uint32_t key = (uint32_t)command_id & UINT32_C(0x1fffffff);
    XgNativeResolveHint *hint;

    if (command_id > UINT32_MAX || family == XG_NATIVE_RESOLVE_NONE ||
        registry_services.resource_generation == NULL)
        return;
    hint = &native_resolve_hints[native_resolve_hint_index(key)];
    *hint = (XgNativeResolveHint){
        .command_id = key,
        .resource_generation = registry_services.resource_generation(),
        .family = (uint8_t)family,
        .valid = true,
    };
}

static void native_resolve_hint_put_nonshared_miss(
        const GuestRenderNativeStreamMissContext *context) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    const uint32_t key = (uint32_t)command_id & UINT32_C(0x1fffffff);
    XgNativeResolveHint *hint;

    if (context == NULL || command_id > UINT32_MAX)
        return;
    hint = &native_resolve_hints[native_resolve_hint_index(key)];
    *hint = (XgNativeResolveHint){
        .command_id = key,
        .word_count = (uint32_t)context->word_count,
        .container_id = context->container_id,
        .resource_generation = registry_services.resource_generation(),
        .family = XG_NATIVE_RESOLVE_NONSHARED_MISS,
        .opcode = context->opcode,
        .source_kind = (uint8_t)context->source_kind,
        .valid = true,
    };
}

static bool native_resolve_family(
        XgNativeResolveFamily family,
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    switch (family) {
    case XG_NATIVE_RESOLVE_MODEL_FT4:
        return registry_services.model_ft4(context, out_semantic);
    case XG_NATIVE_RESOLVE_MODEL_FT3:
        return registry_services.model_ft3(context, out_semantic);
    case XG_NATIVE_RESOLVE_ZOOM:
        return registry_services.zoom(context, out_semantic);
    case XG_NATIVE_RESOLVE_FIELD_SPRITE:
        return registry_services.field_sprite(context, out_semantic);
    case XG_NATIVE_RESOLVE_FIELD_POLYLINE:
        return registry_services.field_polyline(context, out_semantic);
    case XG_NATIVE_RESOLVE_RESIDUAL:
        return registry_services.residual(context, out_semantic);
    case XG_NATIVE_RESOLVE_F4:
        return registry_services.f4(context, out_semantic);
    case XG_NATIVE_RESOLVE_SHARED:
        return xg_render_shared_packet_resolve(
            context, out_visual_id, out_semantic,
            registry_services.shared_packet);
    case XG_NATIVE_RESOLVE_NONSHARED_MISS:
    case XG_NATIVE_RESOLVE_NONE:
    default:
        return false;
    }
}

bool xg_render_resolver_registry_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic) {
    GuestRenderNativeStreamMissContext resolved;
    XgNativeResolveFamily hinted_family;
    bool hinted_resolved;
    uint64_t scene_generation;

    if (!services_are_complete(&registry_services) || context == NULL ||
        out_visual_id == NULL ||
        context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN ||
        context->word_count == 0u || context->opcode < 0x20u ||
        context->opcode > 0x7fu)
        return false;
    scene_generation = registry_services.scene_generation();
    *out_visual_id = (GpuRenderTransactionId){
        scene_generation + 1u, registry_services.f4_source_count(),
    };
    resolved = *context;
    if (resolved.visual_id.scene_epoch == 0u) {
        resolved.visual_id.scene_epoch = scene_generation + 1u;
        resolved.visual_id.state_sequence = 0u;
    }
    hinted_family = native_resolve_hint_find(&resolved);
    hinted_resolved = native_resolve_family(
        hinted_family, &resolved, out_visual_id, out_semantic);
    if (hinted_resolved) {
        *out_visual_id = resolved.visual_id;
        return true;
    }
    if (hinted_family != XG_NATIVE_RESOLVE_NONSHARED_MISS)
        hinted_family = XG_NATIVE_RESOLVE_NONE;
    if (hinted_family != XG_NATIVE_RESOLVE_NONSHARED_MISS) {
        if (registry_services.model_ft4(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_MODEL_FT4;
        else if (registry_services.model_ft3(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_MODEL_FT3;
        else if (registry_services.zoom(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_ZOOM;
        else if (registry_services.field_sprite(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_FIELD_SPRITE;
        else if (registry_services.field_polyline(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_FIELD_POLYLINE;
        else if (registry_services.residual(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_RESIDUAL;
        else if (registry_services.f4(&resolved, out_semantic))
            hinted_family = XG_NATIVE_RESOLVE_F4;
    }
    if (hinted_family != XG_NATIVE_RESOLVE_MODEL_FT4 &&
        hinted_family != XG_NATIVE_RESOLVE_MODEL_FT3 &&
        hinted_family != XG_NATIVE_RESOLVE_ZOOM &&
        hinted_family != XG_NATIVE_RESOLVE_FIELD_SPRITE &&
        hinted_family != XG_NATIVE_RESOLVE_FIELD_POLYLINE &&
        hinted_family != XG_NATIVE_RESOLVE_RESIDUAL &&
        hinted_family != XG_NATIVE_RESOLVE_F4 &&
        xg_render_shared_packet_resolve(
            &resolved, out_visual_id, out_semantic,
            registry_services.shared_packet))
        hinted_family = XG_NATIVE_RESOLVE_SHARED;
    if (hinted_family != XG_NATIVE_RESOLVE_MODEL_FT4 &&
        hinted_family != XG_NATIVE_RESOLVE_MODEL_FT3 &&
        hinted_family != XG_NATIVE_RESOLVE_ZOOM &&
        hinted_family != XG_NATIVE_RESOLVE_FIELD_SPRITE &&
        hinted_family != XG_NATIVE_RESOLVE_FIELD_POLYLINE &&
        hinted_family != XG_NATIVE_RESOLVE_RESIDUAL &&
        hinted_family != XG_NATIVE_RESOLVE_F4 &&
        hinted_family != XG_NATIVE_RESOLVE_SHARED) {
        native_resolve_hint_put_nonshared_miss(&resolved);
        return false;
    }
    xg_render_resolver_registry_hint_put(
        resolved.command_id, hinted_family);
    *out_visual_id = resolved.visual_id;
    return true;
}
