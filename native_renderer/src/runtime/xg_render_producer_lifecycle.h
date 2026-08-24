#ifndef XG_RENDER_PRODUCER_LIFECYCLE_H
#define XG_RENDER_PRODUCER_LIFECYCLE_H

#include "guest_render_native_stream.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderProducerLifecycle {
    uint64_t artifact_generation;
    uint64_t resource_generation;
    uint64_t scene_generation;
    uint32_t producer_pc;
    /* 0: resident, 1: authenticated artifact, 2: no-gates scene,
     * 3: local producer authority. */
    uint8_t scene_resource;
} XgRenderProducerLifecycle;

typedef struct XgRenderProducerLifecycleServices {
    bool (*begin)(uint32_t producer_pc,
                  XgRenderProducerLifecycle *out_lifecycle);
    bool (*matches)(const XgRenderProducerLifecycle *lifecycle);
    bool (*matches_replay)(
        const XgRenderProducerLifecycle *lifecycle,
        const GuestRenderNativeStreamMissContext *context);
    bool (*replay_container_matches_command)(
        const GuestRenderNativeStreamMissContext *context);
    bool (*guest_data_range_is_valid)(
        uint32_t address, uint32_t size, uint32_t alignment,
        bool allow_scratchpad);
} XgRenderProducerLifecycleServices;

#endif
