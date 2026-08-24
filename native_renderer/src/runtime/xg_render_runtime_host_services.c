#include "xg_render_runtime_host_services.h"

#include <stddef.h>

static XgRenderRuntimeHostServices host_services;
static bool configured;

bool xg_render_runtime_configure_host_services(
        const XgRenderRuntimeHostServices *services) {
    if (services == NULL || services->frame_count == NULL ||
        services->read_word == NULL)
        return false;

    host_services = *services;
    configured = true;
    return true;
}

bool xg_render_runtime_host_services(
        XgRenderRuntimeHostServices *out_services) {
    if (!configured || out_services == NULL) return false;
    *out_services = host_services;
    return true;
}
