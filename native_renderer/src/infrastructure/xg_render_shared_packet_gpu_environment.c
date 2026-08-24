#include "xg_render_shared_packet_resolver.h"

#include "gpu.h"

const XgRenderSharedPacketEnvironmentServices *
xg_render_shared_packet_gpu_environment_services(void) {
    static const XgRenderSharedPacketEnvironmentServices services = {
        .max_word_count = GPU_GP0_RING_MAX_WORDS,
        .command_word_count = gpu_gp0_command_word_count,
        .get = gpu_native_environment_get,
        .decode_draw = gpu_native_semantic_from_gp0,
        .decode_line = gpu_native_line_semantic_from_gp0,
    };

    return &services;
}
