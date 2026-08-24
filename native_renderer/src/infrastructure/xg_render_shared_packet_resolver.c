#include "xg_render_shared_packet_resolver.h"

#include <limits.h>
#include <stddef.h>

static bool services_are_complete(
        const XgRenderSharedPacketResolverServices *services) {
    const XgRenderSharedPacketEnvironmentServices *environment =
        services != NULL ? services->environment : NULL;

    return services != NULL && services->guest.read_word != NULL &&
        environment != NULL && environment->max_word_count != 0u &&
        environment->command_word_count != NULL && environment->get != NULL &&
        environment->decode_draw != NULL && environment->decode_line != NULL &&
        services->mode.render_mode != NULL &&
        services->mode.packet_bindings_enabled != NULL &&
        services->identity.visual_scene_generation != NULL &&
        services->identity.interpolation_scene_generation != NULL;
}

static bool source_and_container_are_valid(
        const GuestRenderNativeStreamMissContext *context) {
    if (context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_MMIO ||
        context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK ||
        context->source_kind == GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BURST)
        return true;
    return context->source_kind ==
               GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST &&
        context->command_id >= 4u && context->container_id <= UINT32_MAX &&
        (((uint32_t)context->container_id & UINT32_C(0x1fffffff)) ==
         (((uint32_t)context->command_id - 4u) & UINT32_C(0x1fffffff)));
}

bool xg_render_shared_packet_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderTransactionId *out_visual_id,
        GpuRenderSemantic *out_semantic,
        const XgRenderSharedPacketResolverServices *services) {
    GpuNativeDrawEnvironment environment;
    uint32_t words[GPU_GP0_RING_MAX_WORDS];
    uint64_t visual_scene;
    uint64_t interpolation_scene;
    int expected_words;
    int decoded;

    if (!services_are_complete(services) || context == NULL ||
        out_visual_id == NULL || out_semantic == NULL ||
        services->mode.render_mode() != GUEST_RENDER_RENDER_NATIVE ||
        !services->mode.packet_bindings_enabled() ||
        !source_and_container_are_valid(context) ||
        context->command_id > UINT32_MAX ||
        (context->command_id & 3u) != 0u || context->opcode < 0x20u ||
        context->opcode > 0x7fu || context->word_count == 0u ||
        context->word_count > services->environment->max_word_count ||
        context->word_count > GPU_GP0_RING_MAX_WORDS)
        return false;

    visual_scene = services->identity.visual_scene_generation();
    interpolation_scene = services->identity.interpolation_scene_generation();
    if (visual_scene == UINT64_MAX || interpolation_scene == 0u ||
        (context->visual_id.scene_epoch != 0u &&
         context->visual_id.scene_epoch != visual_scene + 1u))
        return false;

    expected_words = services->environment->command_word_count(context->opcode);
    if (expected_words == 0 ||
        (expected_words > 0 &&
         (size_t)expected_words != context->word_count))
        return false;

    for (size_t index = 0u; index < context->word_count; ++index)
        words[index] = services->guest.read_word(
            (uint32_t)context->command_id + (uint32_t)index * 4u);
    if ((uint8_t)(words[0] >> 24u) != context->opcode) return false;

    services->environment->get(&environment);
    decoded = context->opcode >= 0x40u && context->opcode <= 0x5fu
        ? services->environment->decode_line(
              words, context->word_count, &environment, out_semantic)
        : services->environment->decode_draw(
              words, (int)context->word_count, &environment, out_semantic);
    if (decoded != 1 ||
        (out_semantic->interpolation_identity.scene_id != 0u &&
         out_semantic->interpolation_identity.scene_id != interpolation_scene))
        return false;

    *out_visual_id = context->visual_id.scene_epoch != 0u
        ? context->visual_id
        : (GpuRenderTransactionId){ visual_scene + 1u, 0u };
    return true;
}
