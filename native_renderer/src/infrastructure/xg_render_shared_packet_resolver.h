#ifndef XG_RENDER_SHARED_PACKET_RESOLVER_H
#define XG_RENDER_SHARED_PACKET_RESOLVER_H

#include "guest_render_types.h"
#include "guest_render_native_stream.h"
#include "gpu.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderSharedPacketGuestReader {
    uint32_t (*read_word)(uint32_t address);
} XgRenderSharedPacketGuestReader;

typedef struct XgRenderSharedPacketEnvironmentServices {
    size_t max_word_count;
    int (*command_word_count)(uint8_t opcode);
    void (*get)(GpuNativeDrawEnvironment *out_environment);
    int (*decode_draw)(
        const uint32_t *words, int word_count,
        const GpuNativeDrawEnvironment *environment,
        GpuRenderSemantic *out_semantic);
    int (*decode_line)(
        const uint32_t *words, size_t word_count,
        const GpuNativeDrawEnvironment *environment,
        GpuRenderSemantic *out_semantic);
} XgRenderSharedPacketEnvironmentServices;

typedef struct XgRenderSharedPacketModeServices {
    GuestRenderRenderMode (*render_mode)(void);
    bool (*packet_bindings_enabled)(void);
} XgRenderSharedPacketModeServices;

typedef struct XgRenderSharedPacketIdentityServices {
    uint64_t (*visual_scene_generation)(void);
    uint64_t (*interpolation_scene_generation)(void);
} XgRenderSharedPacketIdentityServices;

typedef struct XgRenderSharedPacketResolverServices {
    XgRenderSharedPacketGuestReader guest;
    const XgRenderSharedPacketEnvironmentServices *environment;
    XgRenderSharedPacketModeServices mode;
    XgRenderSharedPacketIdentityServices identity;
} XgRenderSharedPacketResolverServices;

const XgRenderSharedPacketEnvironmentServices *
xg_render_shared_packet_gpu_environment_services(void);

bool xg_render_shared_packet_resolve(
    const GuestRenderNativeStreamMissContext *context,
    GpuRenderTransactionId *out_visual_id,
    GpuRenderSemantic *out_semantic,
    const XgRenderSharedPacketResolverServices *services);

#endif
