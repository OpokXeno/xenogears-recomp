#ifndef XG_RENDER_RUNTIME_HOST_SERVICES_H
#define XG_RENDER_RUNTIME_HOST_SERVICES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderRuntimeHostServices {
    uint64_t (*frame_count)(void);
    uint32_t (*read_word)(uint32_t address);
} XgRenderRuntimeHostServices;

#ifdef __cplusplus
extern "C" {
#endif

bool xg_render_runtime_configure_host_services(
    const XgRenderRuntimeHostServices *services);
bool xg_render_runtime_host_services(
    XgRenderRuntimeHostServices *out_services);

#ifdef __cplusplus
}
#endif

#endif
