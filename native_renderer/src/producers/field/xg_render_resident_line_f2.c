#include "xg_render_resident_line_f2.h"

#include "gpu.h"

#include <limits.h>
#include <stddef.h>

enum {
    CAPACITY = 6u,
};

static const uint32_t CAPTURE_PC = UINT32_C(0x8007fbe0);
static const uint32_t CAPTURE_INSTRUCTION = UINT32_C(0x00a04021);
static const uint32_t STAGE_PC = UINT32_C(0x80073b64);
static const uint32_t STAGE_INSTRUCTION = UINT32_C(0x3c03800d);
static const uint32_t INTERPOLATION_PRODUCER_ID = UINT32_C(0x80073b64);

typedef struct XgRenderResidentLineF2Source {
    uint32_t packet_addresses[CAPACITY];
    uint32_t xy[CAPACITY][2];
    CPUState *owner_cpu;
    uint32_t heap_address;
    uint8_t buffer_index;
    uint8_t source_count;
    uint8_t publish_count;
    bool valid;
} XgRenderResidentLineF2Source;

static XgRenderResidentLineF2Source source;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static int16_t low_s16(uint32_t value) {
    return (int16_t)(value & UINT32_C(0xffff));
}

static void apply_draw_state(
        GpuRenderMaterial *material, const GpuDrawState *draw) {
    material->draw_area_left = draw->left;
    material->draw_area_top = draw->top;
    material->draw_area_right = draw->right;
    material->draw_area_bottom = draw->bottom;
    material->draw_offset_x = draw->offset_x;
    material->draw_offset_y = draw->offset_y;
    material->texture_window_mask_x = draw->texture_window_mask_x;
    material->texture_window_mask_y = draw->texture_window_mask_y;
    material->texture_window_offset_x = draw->texture_window_offset_x;
    material->texture_window_offset_y = draw->texture_window_offset_y;
    material->dither = draw->dither;
    material->mask_set = draw->mask_set;
    material->mask_check = draw->mask_check;
}

void xg_render_resident_line_f2_clear(void) {
    source = (XgRenderResidentLineF2Source){0};
}

void xg_render_resident_line_f2_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
        event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
        event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_render_resident_line_f2_clear();
}

bool xg_render_resident_line_f2_capture(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderResidentLineF2Services *services) {
    XgRenderResidentLineF2Source captured = {0};
    uint32_t heap_address;
    uint32_t table_address;
    uint8_t level;
    uint8_t publish_count;
    uint8_t buffer_index;

    xg_render_resident_line_f2_clear();
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        services->word_address_is_valid == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    level = (uint8_t)cpu->gpr[4];
    publish_count = (uint8_t)cpu->gpr[5];
    if (level == publish_count) publish_count = (uint8_t)(publish_count - 1u);
    captured.source_count = level > 0u ? (uint8_t)(level - 1u) : 0u;
    if (captured.source_count > CAPACITY ||
        publish_count > captured.source_count)
        return false;
    heap_address = cpu->read_word(UINT32_C(0x800c3ea4));
    buffer_index = cpu->read_byte(UINT32_C(0x800ccb34));
    if (buffer_index > 1u ||
        !services->word_address_is_valid(heap_address))
        return false;
    table_address = UINT32_C(0x800c3202) + (uint32_t)level * 6u;
    if (captured.source_count != 0u &&
        !services->guest_data_range_is_valid(
            table_address, captured.source_count, 1u, false))
        return false;
    for (uint32_t index = 0u; index < captured.source_count; ++index) {
        const uint32_t packet = heap_address + 0x908u +
            ((uint32_t)buffer_index + index * 2u) * 0x10u;
        const uint16_t y =
            (uint16_t)(cpu->read_byte(table_address + index) + 0x5eu);

        if (packet < heap_address ||
            !services->guest_data_range_is_valid(
                packet, 0x10u, 4u, false))
            return false;
        captured.packet_addresses[index] = packet;
        captured.xy[index][0] =
            UINT32_C(0x0000000c) | ((uint32_t)y << 16u);
        captured.xy[index][1] =
            UINT32_C(0x00000012) | ((uint32_t)y << 16u);
    }
    captured.owner_cpu = cpu;
    captured.heap_address = heap_address;
    captured.buffer_index = buffer_index;
    captured.publish_count = publish_count;
    captured.valid = true;
    source = captured;
    return true;
}

bool xg_render_resident_line_f2_stage(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderResidentLineF2Services *services) {
    GpuRenderSemantic semantics[CAPACITY] = {0};
    GpuDrawState draw = {0};
    uint32_t state_address;
    uint32_t heap_address;
    uint8_t count;
    uint8_t buffer_index;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    state_address = cpu->read_word(UINT32_C(0x800d2d28));
    heap_address = cpu->read_word(UINT32_C(0x800c3ea4));
    if (state_address > UINT32_MAX - 0x99u ||
        !services->guest_data_range_is_valid(
            state_address + 0x97u, 2u, 1u, false))
        return false;
    count = cpu->read_byte(state_address + 0x97u);
    buffer_index = cpu->read_byte(state_address + 0x98u);
    if (count == 0u) return true;
    if (!source.valid || source.owner_cpu != cpu ||
        source.heap_address != heap_address ||
        source.buffer_index != buffer_index ||
        source.publish_count != count || count > source.source_count)
        return false;

    gpu_get_draw_state(&draw);
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t packet = heap_address + 0x908u +
            ((uint32_t)buffer_index + index * 2u) * 0x10u;
        GpuRenderSemantic *semantic = &semantics[index];

        if (packet != source.packet_addresses[index] ||
            !services->guest_data_range_is_valid(
                packet, 0x10u, 4u, false) ||
            (cpu->read_word(packet) >> 24u) != 3u ||
            cpu->read_word(packet + 4u) != UINT32_C(0x40ffffff) ||
            cpu->read_word(packet + 8u) != source.xy[index][0] ||
            cpu->read_word(packet + 12u) != source.xy[index][1])
            return false;
        semantic->topology = GPU_RENDER_SEMANTIC_LINES;
        semantic->line_count = 1u;
        semantic->material.shading = GPU_RENDER_SHADING_FLAT;
        apply_draw_state(&semantic->material, &draw);
        for (uint32_t vertex = 0u; vertex < 2u; ++vertex) {
            const uint32_t xy = source.xy[index][vertex];
            GpuRenderSemanticVertex *destination =
                &semantic->lines[0].vertices[vertex];

            destination->x = (int32_t)low_s16(xy) * INT32_C(65536);
            destination->y =
                (int32_t)low_s16(xy >> 16u) * INT32_C(65536);
            destination->r = UINT8_C(0xff);
            destination->g = UINT8_C(0xff);
            destination->b = UINT8_C(0xff);
        }
    }
    if (services->stage_semantic == NULL || services->abort_submission == NULL)
        return false;
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t packet = source.packet_addresses[index];

        if (!services->stage_semantic(
                &semantics[index], packet,
                UINT32_C(0x40000000) |
                    (packet & UINT32_C(0x001ffffc)),
                INTERPOLATION_PRODUCER_ID, index)) {
            services->abort_submission();
            return false;
        }
    }
    return true;
}

XgRenderResidentLineF2Observation xg_render_resident_line_f2_observe(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        GuestRenderRenderMode render_mode,
        const XgRenderResidentLineF2Services *services) {
    if (physical_address_equals(pc, CAPTURE_PC) &&
        instruction_word == CAPTURE_INSTRUCTION) {
        (void)xg_render_resident_line_f2_capture(cpu, render_mode, services);
        return XG_RENDER_RESIDENT_LINE_F2_OBSERVATION_CAPTURE;
    }
    if (physical_address_equals(pc, STAGE_PC) &&
        instruction_word == STAGE_INSTRUCTION) {
        (void)xg_render_resident_line_f2_stage(cpu, render_mode, services);
        return XG_RENDER_RESIDENT_LINE_F2_OBSERVATION_STAGE;
    }
    return XG_RENDER_RESIDENT_LINE_F2_OBSERVATION_NONE;
}
