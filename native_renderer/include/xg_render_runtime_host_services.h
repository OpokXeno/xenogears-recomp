#ifndef XG_RENDER_RUNTIME_HOST_SERVICES_H
#define XG_RENDER_RUNTIME_HOST_SERVICES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderRuntimeHostServices {
    uint64_t (*frame_count)(void);
    uint32_t (*read_word)(uint32_t address);
    bool (*semantic_module)(uint32_t *out_module);
    bool (*native_text_authorizes_pc)(uint32_t owner_entry);
} XgRenderRuntimeHostServices;

#ifdef __cplusplus
extern "C" {
#endif

bool xg_render_runtime_configure_host_services(
    const XgRenderRuntimeHostServices *services);
bool xg_render_runtime_host_services(
    XgRenderRuntimeHostServices *out_services);

/* Runtime-provided cooperative wait callback (user_data is ignored). Call only
 * on the SDL thread from the running simulation, with no collector/core lock
 * held. Suspends the exact fiber for a bounded host-service interval, keeping
 * its stack and guest-cycle budget. Does NOT read sources, reserve capacity,
 * publish work, execute guest code, or poll commands that can restore state.
 * True means serviced, NOT capacity available: the collector must recheck its
 * FIFO/pool capacity, lane status and epoch before retrying begin/append/publish.
 * A blocked lane needs invalidation, not indefinite capacity retries. False means
 * Native service is unavailable/stopped; do not discard pending FIFO work or
 * continue guest execution as though publication succeeded. */
bool psx_native_render_service_wait(void *user_data);

#ifdef __cplusplus
}
#endif

#endif
