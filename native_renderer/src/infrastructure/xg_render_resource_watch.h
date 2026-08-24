#ifndef XG_RENDER_RESOURCE_WATCH_H
#define XG_RENDER_RESOURCE_WATCH_H

#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef void (*XgRenderResourceWatchCallback)(
    uint32_t physical_address, uint32_t size);

/* Tracks guest RAM ranges whose mutation invalidates captured render data. */
void xg_render_resource_watch_register(XgRenderResourceWatchCallback callback);
void xg_render_resource_watch_add(uint32_t address, uint32_t size);
void xg_render_resource_watch_add_model_ft3_descriptor(
    uint32_t address, uint32_t size);
bool xg_render_resource_watch_model_ft3_descriptor_overlaps(
    uint32_t address, uint32_t size);
bool xg_render_resource_watch_overlaps(uint32_t address, uint32_t size);
bool xg_render_resource_watch_needs_invalidation(
    uint32_t address, uint32_t size);
void xg_render_resource_watch_mark_invalidated(
    uint32_t address, uint32_t size);
void xg_render_resource_watch_reset(void);
void xg_render_resource_watch_handle_invalidation(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);
void xg_render_resource_watch_reset_model_ft3_descriptors(void);

#endif
