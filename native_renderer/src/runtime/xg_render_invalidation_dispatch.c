#include "xg_render_invalidation_dispatch.h"

#include <stddef.h>

enum { XG_RENDER_INVALIDATION_MODULE_CAPACITY = 32u };

static XgRenderInvalidationModule
    modules[XG_RENDER_INVALIDATION_MODULE_CAPACITY];
static uint32_t module_count;

void xg_render_invalidation_clear_modules(void) {
    module_count = 0u;
}

bool xg_render_invalidation_register_module(
        const XgRenderInvalidationModule *module) {
    if (module == NULL || module->handle == NULL ||
        module_count >= XG_RENDER_INVALIDATION_MODULE_CAPACITY)
        return false;
    modules[module_count++] = *module;
    return true;
}

void xg_render_invalidation_dispatch(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    if (event == NULL || services == NULL) return;
    for (uint32_t index = 0u; index < module_count; ++index)
        modules[index].handle(event, services);
}
