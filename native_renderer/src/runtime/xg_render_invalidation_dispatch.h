#ifndef XG_RENDER_INVALIDATION_DISPATCH_H
#define XG_RENDER_INVALIDATION_DISPATCH_H

#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

void xg_render_invalidation_clear_modules(void);
bool xg_render_invalidation_register_module(
    const XgRenderInvalidationModule *module);
void xg_render_invalidation_dispatch(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

#endif
