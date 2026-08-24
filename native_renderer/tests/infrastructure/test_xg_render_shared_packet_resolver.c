#include "xg_render_shared_packet_resolver.h"

#include <assert.h>
#include <string.h>

static uint32_t packet_words[4];
static GuestRenderRenderMode render_mode;
static bool bindings_enabled;
static uint64_t visual_scene;
static uint64_t interpolation_scene;
static uint32_t read_count;
static uint32_t draw_decode_count;
static uint32_t line_decode_count;
static int decode_result;
static uint64_t decoded_interpolation_scene;

static uint32_t read_word(uint32_t address) {
    ++read_count;
    return packet_words[(address - UINT32_C(0x100)) / 4u];
}

static int command_word_count(uint8_t opcode) {
    return opcode == 0x20u || opcode == 0x40u ? 3 : 0;
}

static void get_environment(GpuNativeDrawEnvironment *out_environment) {
    memset(out_environment, 0, sizeof(*out_environment));
}

static int decode_draw(
        const uint32_t *words, int word_count,
        const GpuNativeDrawEnvironment *environment,
        GpuRenderSemantic *out_semantic) {
    (void)words;
    (void)word_count;
    (void)environment;
    ++draw_decode_count;
    memset(out_semantic, 0, sizeof(*out_semantic));
    out_semantic->interpolation_identity.scene_id =
        decoded_interpolation_scene;
    return decode_result;
}

static int decode_line(
        const uint32_t *words, size_t word_count,
        const GpuNativeDrawEnvironment *environment,
        GpuRenderSemantic *out_semantic) {
    ++line_decode_count;
    return decode_draw(
        words, (int)word_count, environment, out_semantic);
}

static GuestRenderRenderMode get_render_mode(void) {
    return render_mode;
}

static bool get_bindings_enabled(void) {
    return bindings_enabled;
}

static uint64_t get_visual_scene(void) {
    return visual_scene;
}

static uint64_t get_interpolation_scene(void) {
    return interpolation_scene;
}

static const XgRenderSharedPacketEnvironmentServices environment_services = {
    .max_word_count = 4u,
    .command_word_count = command_word_count,
    .get = get_environment,
    .decode_draw = decode_draw,
    .decode_line = decode_line,
};

static const XgRenderSharedPacketResolverServices services = {
    .guest.read_word = read_word,
    .environment = &environment_services,
    .mode = {
        .render_mode = get_render_mode,
        .packet_bindings_enabled = get_bindings_enabled,
    },
    .identity = {
        .visual_scene_generation = get_visual_scene,
        .interpolation_scene_generation = get_interpolation_scene,
    },
};

static GuestRenderNativeStreamMissContext valid_context(uint8_t opcode) {
    packet_words[0] = (uint32_t)opcode << 24u;
    packet_words[1] = 1u;
    packet_words[2] = 2u;
    render_mode = GUEST_RENDER_RENDER_NATIVE;
    bindings_enabled = true;
    visual_scene = 7u;
    interpolation_scene = 11u;
    read_count = 0u;
    draw_decode_count = 0u;
    line_decode_count = 0u;
    decode_result = 1;
    decoded_interpolation_scene = 0u;
    return (GuestRenderNativeStreamMissContext){
        .visual_id = { 8u, 3u },
        .command_id = UINT32_C(0x100),
        .container_id = UINT32_C(0x80),
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_BLOCK,
        .opcode = opcode,
        .word_count = 3u,
    };
}

static bool resolve(const GuestRenderNativeStreamMissContext *context) {
    GpuRenderTransactionId visual_id = {0};
    GpuRenderSemantic semantic;

    return xg_render_shared_packet_resolve(
        context, &visual_id, &semantic, &services);
}

int main(void) {
    GuestRenderNativeStreamMissContext context = valid_context(0x20u);
    GpuRenderTransactionId visual_id = {0};
    GpuRenderSemantic semantic;

    assert(xg_render_shared_packet_resolve(
        &context, &visual_id, &semantic, &services));
    assert(visual_id.scene_epoch == 8u && visual_id.state_sequence == 3u);
    assert(read_count == 3u && draw_decode_count == 1u);

    context = valid_context(0x40u);
    assert(resolve(&context));
    assert(line_decode_count == 1u && draw_decode_count == 1u);

    context = valid_context(0x20u);
    context.visual_id = (GpuRenderTransactionId){0};
    assert(xg_render_shared_packet_resolve(
        &context, &visual_id, &semantic, &services));
    assert(visual_id.scene_epoch == 8u && visual_id.state_sequence == 0u);

    context = valid_context(0x20u);
    context.source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST;
    context.container_id = UINT32_C(0x0fc);
    assert(resolve(&context));
    context.container_id += 4u;
    assert(!resolve(&context));

    context = valid_context(0x20u);
    context.word_count = 2u;
    assert(!resolve(&context));
    context = valid_context(0x20u);
    packet_words[0] = UINT32_C(0x21000000);
    assert(!resolve(&context));
    context = valid_context(0x20u);
    context.visual_id.scene_epoch = 9u;
    assert(!resolve(&context));
    context = valid_context(0x20u);
    context.source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_UNKNOWN;
    assert(!resolve(&context));

    context = valid_context(0x20u);
    render_mode = GUEST_RENDER_RENDER_SHADOW;
    assert(!resolve(&context));
    context = valid_context(0x20u);
    bindings_enabled = false;
    assert(!resolve(&context));
    context = valid_context(0x20u);
    decode_result = -1;
    assert(!resolve(&context));
    context = valid_context(0x20u);
    decoded_interpolation_scene = 12u;
    assert(!resolve(&context));

    return 0;
}
