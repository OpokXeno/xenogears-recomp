#include "xg_render_overlay_ft4.h"

#include "gpu.h"
#include "xg_field_render_services.h"
#include "xg_render_field_sprite.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_quad_builder.h"

#include <stddef.h>

#define TEMPLATE_CAPACITY XG_RENDER_IR_ITEM_CAPACITY

static XgRenderOverlayFt4Template templates[TEMPLATE_CAPACITY];
static uint32_t template_count;
static PsxXgRenderOverlayFt4Snapshot overlay_snapshot;
static bool projected_2e_descriptor_scope;
typedef struct XgRenderOverlayFt4LocalProducerPending {
    uint64_t generation;
    uint32_t entry_sp;
    uint32_t return_address;
    uint32_t packet_address;
    uint8_t expected_opcode;
    bool valid;
} XgRenderOverlayFt4LocalProducerPending;

static XgRenderOverlayFt4LocalProducerPending local_producer_pending;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool normalized_ranges_overlap(
        uint32_t left_start, uint32_t left_size,
        uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & UINT32_C(0x1fffffff);
    const uint64_t left_end = left_begin + left_size;
    const uint64_t right_begin = right_start & UINT32_C(0x1fffffff);
    const uint64_t right_end = right_begin + right_size;

    return left_size != 0u && right_size != 0u &&
        left_begin < right_end && right_begin < left_end;
}

static XgRenderOverlayFt4Template *overlay_ft4_find(
        uint32_t packet_address) {
    for (uint32_t index = 0u; index < template_count; ++index) {
        XgRenderOverlayFt4Template *record = &templates[index];

        if (record->valid && physical_address_equals(
                record->packet_address, packet_address))
            return record;
    }
    return NULL;
}

bool xg_render_overlay_ft4_lookup(
        uint32_t packet_address, XgRenderOverlayFt4Template *out_template) {
    const XgRenderOverlayFt4Template *record =
        overlay_ft4_find(packet_address);

    if (record == NULL) return false;
    if (out_template != NULL) *out_template = *record;
    return true;
}

static XgRenderOverlayFt4Template *overlay_ft4_upsert(
        uint32_t packet_address, uint32_t producer_pc,
        const XgRenderOverlayFt4Services *services,
        uint32_t *failure_detail) {
    XgRenderOverlayFt4Template *record =
        overlay_ft4_find(packet_address);

    if (failure_detail != NULL) *failure_detail = 0u;
    if (services == NULL || services->lifecycle == NULL ||
        services->lifecycle->begin == NULL ||
        services->lifecycle->matches == NULL) {
        if (failure_detail != NULL) *failure_detail = 3u;
        return NULL;
    }
    if (record != NULL) {
        if (services->lifecycle->matches(&record->lifecycle)) return record;
        if (services->local_producer_generation != NULL &&
            services->local_producer_generation(producer_pc) != 0u) {
            *record = (XgRenderOverlayFt4Template){
                .packet_address = packet_address,
            };
            if (services->lifecycle->begin(
                    producer_pc, &record->lifecycle)) {
                if (services->watch_resource != NULL)
                    services->watch_resource(packet_address, 0x28u);
                return record;
            }
        }
        if (failure_detail != NULL) *failure_detail = 1u;
        return NULL;
    }
    if (template_count == TEMPLATE_CAPACITY) {
        if (failure_detail != NULL) *failure_detail = 2u;
        return NULL;
    }
    record = &templates[template_count++];
    *record = (XgRenderOverlayFt4Template){
        .packet_address = packet_address,
    };
    if (!services->lifecycle->begin(producer_pc, &record->lifecycle)) {
        if (failure_detail != NULL) *failure_detail = 3u;
        *record = (XgRenderOverlayFt4Template){0};
        --template_count;
        return NULL;
    }
    if (services->watch_resource != NULL)
        services->watch_resource(packet_address, 0x28u);
    return record;
}

bool xg_render_overlay_ft4_publish_field_sprite(
        const XgRenderFieldSpriteOverlayPublication *publication,
        const XgRenderOverlayFt4Services *services,
        uint32_t *failure_detail) {
    XgRenderOverlayFt4Template *target;

    if (failure_detail != NULL) *failure_detail = 0u;
    if (publication == NULL || services == NULL ||
        services->lifecycle == NULL || services->lifecycle->matches == NULL ||
        !services->lifecycle->matches(&publication->lifecycle)) {
        if (failure_detail != NULL) *failure_detail = 3u;
        return false;
    }
    target = overlay_ft4_find(publication->packet_address);
    if (target == NULL) {
        if (template_count == TEMPLATE_CAPACITY) {
            if (failure_detail != NULL) *failure_detail = 2u;
            return false;
        }
        target = &templates[template_count++];
    }
    *target = (XgRenderOverlayFt4Template){
        .primitive = publication->primitive,
        .lifecycle = publication->lifecycle,
        .material = publication->primitive.material,
        .packet_address = publication->packet_address,
        .source_primitive_index = publication->source_primitive_index,
        .interpolation_producer_id = publication->interpolation_producer_id,
        .interpolation_primitive_id = publication->interpolation_primitive_id,
        .family = publication->family,
        .interpolation_identity_valid =
            publication->interpolation_identity_valid,
        .material_ready = publication->material_ready,
        .valid = true,
    };
    if (services->watch_resource != NULL)
        services->watch_resource(publication->packet_address, 0x28u);
    if (publication->kind == XG_RENDER_FIELD_SPRITE_OVERLAY_PROJECTED_2E) {
        ++overlay_snapshot.projected_2e_material_count;
    } else {
        ++overlay_snapshot.field_source_template_count;
        if (publication->family == 4u)
            ++overlay_snapshot.field_base_template_count;
        else
            ++overlay_snapshot.field_offset_template_count;
    }
    return true;
}

bool xg_render_overlay_ft4_capture_initialized_packet(
        CPUState *cpu, uint32_t packet_address, uint32_t producer_pc,
        uint8_t expected_opcode,
        const XgRenderOverlayFt4Services *services) {
    GpuDrawState draw = {0};
    XgRenderQuadSource source = {0};
    XgRenderOverlayFt4Template *record;
    uint32_t words[10];

    if (cpu == NULL || cpu->read_word == NULL || services == NULL ||
        services->guest_data_range_is_valid == NULL ||
        (expected_opcode != 0x2cu && expected_opcode != 0x2eu) ||
        !services->guest_data_range_is_valid(
            packet_address, 0x28u, 4u, false))
        return false;
    for (uint32_t word = 0u; word < 10u; ++word)
        words[word] = cpu->read_word(packet_address + word * 4u);
    if ((uint8_t)(words[0] >> 24u) != 9u ||
        (uint8_t)(words[1] >> 24u) != expected_opcode)
        return false;
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&source.material, &draw);
    source.material.tpage = (uint16_t)(words[5] >> 16u);
    source.material.texture_page_x = source.material.tpage & 0x0fu;
    source.material.texture_page_y = (source.material.tpage >> 4u) & 1u;
    source.material.texture_depth = (XgRenderIrTextureDepth)(
        (source.material.tpage >> 7u) & 3u);
    source.material.blend_mode = (XgRenderIrBlendMode)(
        (source.material.tpage >> 5u) & 3u);
    source.material.clut_x = ((uint16_t)(words[3] >> 16u) & 0x3fu) << 4u;
    source.material.clut_y = (uint16_t)(words[3] >> 16u) >> 6u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = (expected_opcode & 1u) != 0u;
    source.material.semi_transparent = (expected_opcode & 2u) != 0u;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t xy = words[2u + vertex * 2u];
        const uint16_t uv = (uint16_t)words[3u + vertex * 2u];

        source.vertices[vertex] = (XgRenderQuadSourceVertex){
            .x = xg_render_runtime_low_s16(xy),
            .y = xg_render_runtime_low_s16(xy >> 16u),
            .u = (uint8_t)uv,
            .v = (uint8_t)(uv >> 8u),
            .red = (uint8_t)words[1],
            .green = (uint8_t)(words[1] >> 8u),
            .blue = (uint8_t)(words[1] >> 16u),
        };
    }
    record = overlay_ft4_upsert(
        packet_address, producer_pc, services, NULL);
    if (record == NULL ||
        xg_render_quad_build_primitive(&source, &record->primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
        return false;
    record->material = source.material;
    record->source_primitive_index =
        ((uint32_t)expected_opcode << 24u) |
        (packet_address & UINT32_C(0x00ffffff));
    record->interpolation_producer_id =
        producer_pc & UINT32_C(0x1fffffff);
    record->interpolation_primitive_id =
        packet_address & UINT32_C(0x1fffffff);
    record->family = expected_opcode == 0x2cu ? 11u : 12u;
    record->interpolation_identity_valid = true;
    record->material_ready = true;
    record->valid = true;
    ++overlay_snapshot.field_source_template_count;
    return true;
}

bool xg_render_overlay_ft4_local_producer_preflight(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        uint8_t expected_opcode, const XgRenderOverlayFt4Services *services) {
    uint32_t packet_address;

    if (cpu == NULL || services == NULL ||
        services->guest_data_range_is_valid == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        (expected_opcode != 0x2cu && expected_opcode != 0x2eu) ||
        cpu->gpr[4] > UINT32_MAX - 0x48u)
        return false;
    packet_address = cpu->gpr[4] + 0x48u;
    return services->guest_data_range_is_valid(
        packet_address, 0x28u, 4u, false);
}

bool xg_render_overlay_ft4_local_producer_begin(
        CPUState *cpu, GuestRenderRenderMode render_mode, uint64_t generation,
        uint8_t expected_opcode, const XgRenderOverlayFt4Services *services) {
    const uint32_t packet_address = cpu != NULL &&
        cpu->gpr[4] <= UINT32_MAX - 0x48u ? cpu->gpr[4] + 0x48u : 0u;

    local_producer_pending = (XgRenderOverlayFt4LocalProducerPending){0};
    if (generation == 0u || !xg_render_overlay_ft4_local_producer_preflight(
            cpu, render_mode, expected_opcode, services))
        return false;
    local_producer_pending.generation = generation;
    local_producer_pending.entry_sp = cpu->gpr[29];
    local_producer_pending.return_address = cpu->gpr[31];
    local_producer_pending.packet_address = packet_address;
    local_producer_pending.expected_opcode = expected_opcode;
    local_producer_pending.valid = true;
    return true;
}

bool xg_render_overlay_ft4_local_producer_writer(
        CPUState *cpu, uint32_t writer_index, uint8_t expected_opcode) {
    const uint32_t frame_size = expected_opcode == 0x2cu ? 0xd0u : 0x20u;

    return cpu != NULL && local_producer_pending.valid && writer_index < 2u &&
        local_producer_pending.expected_opcode == expected_opcode &&
        local_producer_pending.entry_sp >= frame_size &&
        cpu->gpr[29] == local_producer_pending.entry_sp - frame_size &&
        physical_address_equals(
            cpu->gpr[6], local_producer_pending.packet_address +
                writer_index * 0x10u);
}

bool xg_render_overlay_ft4_local_producer_commit(
        CPUState *cpu, uint64_t generation, uint32_t producer_pc,
        uint8_t expected_opcode, const XgRenderOverlayFt4Services *services) {
    return cpu != NULL && local_producer_pending.valid &&
        generation == local_producer_pending.generation &&
        expected_opcode == local_producer_pending.expected_opcode &&
        cpu->gpr[29] == local_producer_pending.entry_sp &&
        physical_address_equals(
            cpu->gpr[31], local_producer_pending.return_address) &&
        xg_render_overlay_ft4_capture_initialized_packet(
            cpu, local_producer_pending.packet_address, producer_pc,
            expected_opcode, services);
}

void xg_render_overlay_ft4_local_producer_cancel(void) {
    local_producer_pending = (XgRenderOverlayFt4LocalProducerPending){0};
}

bool xg_render_overlay_ft4_capture_direct_templates(
        CPUState *cpu, uint32_t producer_pc,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    GpuDrawState draw = {0};
    uint32_t global;
    uint32_t packet_base;
    uint32_t buffer;
    int16_t origin_x;
    int16_t origin_y;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_data_range_is_valid(global, 0x350u, 4u, false))
        return false;
    packet_base = cpu->read_word(global + 0x34cu);
    buffer = cpu->read_word(global + 0x308u);
    if (buffer > 1u || !services->guest_data_range_is_valid(
            UINT32_C(0x801ea04c), 8u, 2u, false))
        return false;
    origin_x = (int16_t)cpu->read_half(UINT32_C(0x801ea04c));
    origin_y = (int16_t)cpu->read_half(UINT32_C(0x801ea050));
    gpu_get_draw_state(&draw);
    for (uint32_t index = 0u; index < 16u; ++index) {
        XgRenderOverlayFt4Template *record;
        XgRenderQuadSource source = {0};
        const int16_t left = (int16_t)(origin_x + index * 12u);
        const int16_t right = (int16_t)(left + 12);
        const int16_t bottom = (int16_t)(origin_y + 16);
        const uint8_t u0 = (uint8_t)(index * 16u);
        const uint8_t u1 = (uint8_t)(u0 + 12u);

        record = overlay_ft4_upsert(
            packet_base + (index * 2u + buffer) * 0x28u + 0x277cu,
            producer_pc, services, NULL);
        if (record == NULL) return false;
        record->source_primitive_index = UINT32_C(0x2c730000) | index;
        xg_render_material_apply_draw_state(&source.material, &draw);
        source.material.tpage = 5u;
        source.material.texture_page_x = 5u;
        source.material.texture_page_y = 0u;
        source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
        source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
        source.material.clut_x = 0u;
        source.material.clut_y = 0x1c0u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = false;
        source.vertices[0] = (XgRenderQuadSourceVertex){
            left, origin_y, u0, 0xf0u, 0x80u, 0x80u, 0x80u};
        source.vertices[1] = (XgRenderQuadSourceVertex){
            right, origin_y, u1, 0xf0u, 0x80u, 0x80u, 0x80u};
        source.vertices[2] = (XgRenderQuadSourceVertex){
            left, bottom, u0, 0xffu, 0x80u, 0x80u, 0x80u};
        source.vertices[3] = (XgRenderQuadSourceVertex){
            right, bottom, u1, 0xffu, 0x80u, 0x80u, 0x80u};
        if (xg_render_quad_build_primitive(&source, &record->primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            return false;
        record->valid = true;
        record->material = source.material;
        record->material_ready = true;
        record->family = 1u;
    }
    overlay_snapshot.direct_template_count += 16u;
    return true;
}

bool xg_render_overlay_ft4_capture_rectangle_template(
        CPUState *cpu, uint32_t producer_pc,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    GpuDrawState draw = {0};
    XgRenderQuadSource source = {0};
    XgRenderOverlayFt4Template *record;
    uint32_t stack;
    uint32_t clut;
    uint8_t v;
    uint8_t width;
    uint8_t height;
    int16_t left;
    int16_t top;
    uint8_t u;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL)
        return false;
    left = (int16_t)cpu->gpr[5];
    top = (int16_t)cpu->gpr[6];
    u = (uint8_t)cpu->gpr[7];
    stack = cpu->gpr[29];
    v = (uint8_t)cpu->read_word(stack + 0x10u);
    width = (uint8_t)cpu->read_word(stack + 0x14u);
    height = (uint8_t)cpu->read_word(stack + 0x18u);
    record = overlay_ft4_upsert(
        cpu->gpr[4], producer_pc, services, NULL);
    if (record == NULL) return false;
    gpu_get_draw_state(&draw);
    if (record->material_ready) {
        source.material = record->material;
        xg_render_material_apply_draw_state(&source.material, &draw);
    } else {
        clut = cpu->read_half(UINT32_C(0x800595d4));
        xg_render_material_apply_draw_state(&source.material, &draw);
        source.material.tpage = 6u;
        source.material.texture_page_x = 6u;
        source.material.texture_page_y = 0u;
        source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
        source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
        source.material.clut_x = (clut & 0x3fu) << 4u;
        source.material.clut_y = clut >> 6u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = false;
        record->material = source.material;
        record->material_ready = true;
    }
    source.vertices[0] = (XgRenderQuadSourceVertex){
        left, top, u, v, 0x80u, 0x80u, 0x80u};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        (int16_t)(left + width), top, (uint8_t)(u + width), v,
        0x80u, 0x80u, 0x80u};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        left, (int16_t)(top + height), u, (uint8_t)(v + height),
        0x80u, 0x80u, 0x80u};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        (int16_t)(left + width), (int16_t)(top + height),
        (uint8_t)(u + width), (uint8_t)(v + height),
        0x80u, 0x80u, 0x80u};
    if (xg_render_quad_build_primitive(&source, &record->primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
        return false;
    record->source_primitive_index = UINT32_C(0x2c920000) |
        (record->packet_address & UINT32_C(0xffff));
    if (record->family == 0u) record->family = 2u;
    record->valid = true;
    ++overlay_snapshot.rectangle_template_count;
    return true;
}

bool xg_render_overlay_ft4_capture_projected_material(
        CPUState *cpu, uint32_t producer_pc,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    GpuDrawState draw = {0};
    uint32_t object;
    uint32_t mode;
    uint32_t clut;
    int32_t half_index;
    int32_t v0;
    uint8_t u0;
    uint8_t u1;
    uint8_t color;

    if (cpu == NULL || cpu->read_byte == NULL || cpu->read_half == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    object = cpu->gpr[4];
    mode = cpu->gpr[7] & 0xffu;
    if (!services->guest_data_range_is_valid(object, 0x80u, 1u, false))
        return false;
    gpu_get_draw_state(&draw);
    half_index = (int32_t)cpu->gpr[5] / 2;
    if (mode == 0u) {
        const int32_t quarter =
            ((int32_t)cpu->gpr[5] + (int32_t)cpu->gpr[6]) / 4;
        u0 = (uint8_t)(half_index * -128);
        v0 = quarter * 13;
        color = 0x80u;
    } else {
        u0 = (uint8_t)((cpu->gpr[5] & 1u) * 96u);
        v0 = half_index * 13 + (int32_t)cpu->gpr[6];
        color = (mode & 0x80u) == 0u ? 0x20u : 0x80u;
    }
    u1 = (uint8_t)(u0 + cpu->read_byte(object + 0x7eu));
    for (uint32_t buffer = 0u; buffer < 2u; ++buffer) {
        XgRenderOverlayFt4Template *record = overlay_ft4_upsert(
            object + buffer * 0x28u, producer_pc, services, NULL);
        XgRenderIrMaterialState material = {0};
        XgRenderQuadSource source = {0};
        uint32_t clut_selector;

        if (record == NULL) return false;
        clut_selector = mode == 0u ? (cpu->gpr[5] & 1u) :
            ((mode & 0x7fu) - 1u);
        clut = cpu->read_half(clut_selector == 0u ?
            UINT32_C(0x800595d4) : UINT32_C(0x80059414));
        xg_render_material_apply_draw_state(&material, &draw);
        material.tpage = mode == 0u ? 5u : 6u;
        if (mode != 0u && (mode & 0x80u) == 0u) {
            material.tpage |= 0x20u;
            material.semi_transparent = true;
        }
        material.texture_page_x = material.tpage & 0xfu;
        material.texture_page_y = (material.tpage >> 4u) & 1u;
        material.texture_depth = (XgRenderIrTextureDepth)(
            (material.tpage >> 7u) & 3u);
        material.blend_mode = (XgRenderIrBlendMode)(
            (material.tpage >> 5u) & 3u);
        material.clut_x = (clut & 0x3fu) << 4u;
        material.clut_y = clut >> 6u;
        material.shading = XG_RENDER_IR_SHADING_FLAT;
        material.textured = true;
        material.raw_texture = false;
        record->material = material;
        record->material_ready = true;
        record->family = 3u;
        record->source_primitive_index = UINT32_C(0x2ca70000) |
            (object & UINT32_C(0xffff));
        source.material = material;
        source.vertices[0] = (XgRenderQuadSourceVertex){
            0, 0, u0, (uint8_t)v0, color, color, color};
        source.vertices[1] = (XgRenderQuadSourceVertex){
            0, 0, u1, (uint8_t)v0, color, color, color};
        source.vertices[2] = (XgRenderQuadSourceVertex){
            0, 0, u0, (uint8_t)(v0 + 13), color, color, color};
        source.vertices[3] = (XgRenderQuadSourceVertex){
            0, 0, u1, (uint8_t)(v0 + 13), color, color, color};
        if (xg_render_quad_build_primitive(
                &source, &record->primitive) != XG_RENDER_QUAD_BUILDER_OK)
            return false;
        record->valid = true;
        ++overlay_snapshot.projected_material_count;
    }
    return true;
}

bool xg_render_overlay_ft4_capture_glyph_material(
        CPUState *cpu, uint32_t producer_pc,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    GpuDrawState draw = {0};
    XgRenderQuadSource source = {0};
    XgRenderOverlayFt4Template *record;
    uint32_t global;
    uint32_t source_base;
    uint32_t source_address;
    uint32_t row;
    uint32_t clut;
    uint8_t u0;
    uint8_t u1;
    uint8_t v0;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_byte == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_data_range_is_valid(global, 0x4d0u, 4u, false))
        return false;
    source_base = cpu->read_word(global + 0x32cu);
    row = cpu->gpr[17];
    source_address = source_base + row * 0x5cu +
        cpu->read_word(global + 0x4ccu) * 4u;
    if (!services->guest_data_range_is_valid(source_address, 1u, 1u, false))
        return false;
    record = overlay_ft4_upsert(
        cpu->gpr[16] + (cpu->gpr[7] & 1u) * 0x28u,
        producer_pc, services, NULL);
    if (record == NULL) return false;
    clut = cpu->gpr[2] & 0xffffu;
    u0 = (uint8_t)cpu->gpr[20];
    u1 = (uint8_t)(u0 + 16u);
    v0 = cpu->read_byte(source_address);
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&source.material, &draw);
    source.material.tpage = 5u;
    source.material.texture_page_x = 5u;
    source.material.texture_page_y = 0u;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    source.material.clut_x = (clut & 0x3fu) << 4u;
    source.material.clut_y = clut >> 6u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = false;
    source.vertices[0] = (XgRenderQuadSourceVertex){
        0, 0, u0, v0, 0x80u, 0x80u, 0x80u};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        0, 0, u1, v0, 0x80u, 0x80u, 0x80u};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        0, 0, u0, (uint8_t)(v0 + 16u), 0x80u, 0x80u, 0x80u};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        0, 0, u1, (uint8_t)(v0 + 16u), 0x80u, 0x80u, 0x80u};
    if (xg_render_quad_build_primitive(&source, &record->primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
        return false;
    record->material = source.material;
    record->material_ready = true;
    record->valid = true;
    record->family = 3u;
    record->source_primitive_index = UINT32_C(0x2ca80000) |
        (record->packet_address & UINT32_C(0xffff));
    ++overlay_snapshot.projected_material_count;
    return true;
}

bool xg_render_overlay_ft4_capture_projected_2e_material(
        CPUState *cpu, uint32_t template_family, uint32_t producer_pc,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    GpuDrawState draw = {0};
    uint32_t global;
    uint32_t object;
    uint32_t buffer;
    uint32_t packet_offset;
    uint8_t u0;
    uint8_t u1;
    uint8_t v0;
    uint8_t v1;
    uint16_t tpage;
    uint16_t clut;

    if (cpu == NULL || cpu->read_word == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        services->guest_data_range_is_valid == NULL)
        return false;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_data_range_is_valid(global, 0x4ccu, 4u, false))
        return false;
    object = cpu->read_word(global + 0x364u + (cpu->gpr[4] & 0xffu) * 4u);
    buffer = cpu->read_word(global + 0x308u);
    if (buffer > 1u || !services->guest_data_range_is_valid(
            object, 0x720u, 4u, false))
        return false;
    tpage = xg_render_field_sprite_tpage(
        (uint16_t)cpu->read_word(global + 0x4b8u),
        (int16_t)cpu->read_word(global + 0x4c4u),
        (int16_t)cpu->read_word(global + 0x4c8u));
    clut = xg_render_field_sprite_clut(
        (int16_t)cpu->read_word(global + 0x4bcu),
        (int16_t)cpu->read_word(global + 0x4c0u));
    if (template_family > 3u) return false;
    packet_offset = template_family == 0u ? 0x320u :
        template_family == 1u ? 0x280u :
        template_family == 2u ? 0x1e0u : 0x140u;
    u0 = template_family == 2u ? 0x08u :
        template_family == 3u ? 0x00u : 0x10u;
    u1 = template_family == 2u ? 0x0fu :
        template_family == 3u ? 0x07u : 0x20u;
    v0 = template_family == 0u ? 0x8cu : 0x84u;
    v1 = template_family == 2u ? 0x94u : (uint8_t)(v0 + 7u);
    gpu_get_draw_state(&draw);
    for (uint32_t quad = 0u; quad < 2u; ++quad) {
        XgRenderOverlayFt4Template *record = overlay_ft4_upsert(
            object + (quad * 2u + buffer) * 0x28u + packet_offset,
            producer_pc, services, NULL);
        XgRenderQuadSource source = {0};

        if (record == NULL) return false;
        xg_render_material_apply_draw_state(&source.material, &draw);
        source.material.tpage = tpage;
        source.material.texture_page_x = tpage & 0x0fu;
        source.material.texture_page_y = (tpage >> 4u) & 1u;
        source.material.texture_depth = (XgRenderIrTextureDepth)(
            (tpage >> 7u) & 3u);
        source.material.blend_mode = (XgRenderIrBlendMode)(
            (tpage >> 5u) & 3u);
        source.material.clut_x = (clut & 0x3fu) << 4u;
        source.material.clut_y = clut >> 6u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = false;
        source.material.semi_transparent = true;
        source.vertices[0] = (XgRenderQuadSourceVertex){
            0, 0, u0, v0, 0x80u, 0x80u, 0x80u};
        source.vertices[1] = (XgRenderQuadSourceVertex){
            0, 0, u1, v0, 0x80u, 0x80u, 0x80u};
        source.vertices[2] = (XgRenderQuadSourceVertex){
            0, 0, u0, v1, 0x80u, 0x80u, 0x80u};
        source.vertices[3] = (XgRenderQuadSourceVertex){
            0, 0, u1, v1, 0x80u, 0x80u, 0x80u};
        if (xg_render_quad_build_primitive(&source, &record->primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            return false;
        record->material = source.material;
        record->material_ready = true;
        record->valid = true;
        record->family = (uint8_t)(6u + template_family);
        record->source_primitive_index = UINT32_C(0x2e000000) |
            ((UINT32_C(0x49) - template_family * 3u) << 16u) | quad;
        ++overlay_snapshot.projected_2e_material_count;
    }
    return true;
}

static void set_projected_position(
        XgRenderIrNativePrimitive *primitive,
        const XgHost3dRotTransPers4Output *projection) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};

    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const XgHost3dProjectedVertex *projected =
                &projection->vertices[split[triangle][vertex]];
            XgRenderQuadSourceVertex source_vertex = {0};
            XgRenderIrVertex *target =
                &primitive->triangles[triangle].vertices[vertex];

            xg_render_quad_set_projected_position(&source_vertex, projected);
            target->x = (int32_t)source_vertex.x * INT32_C(65536);
            target->y = (int32_t)source_vertex.y * INT32_C(65536);
            target->native_view_x = source_vertex.native_view_x_16_16;
            target->native_view_y = source_vertex.native_view_y_16_16;
            target->native_view_position = source_vertex.native_view_position;
            target->projective_view_x = source_vertex.projective_view_x;
            target->projective_view_y = source_vertex.projective_view_y;
            target->projective_view_z = source_vertex.projective_view_z;
            target->projective_offset_x =
                source_vertex.projective_offset_x_16_16;
            target->projective_offset_y =
                source_vertex.projective_offset_y_16_16;
            target->projective_native_offset_x =
                source_vertex.projective_native_offset_x_16_16;
            target->projective_native_offset_y =
                source_vertex.projective_native_offset_y_16_16;
            target->projective_distance = source_vertex.projective_distance;
            target->projective_position = source_vertex.projective_position;
        }
    }
}

bool xg_render_overlay_ft4_capture_projected_geometry(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    XgRenderOverlayFt4Template *record;
    PsxXgRenderOverlayFt4Snapshot *snapshot = &overlay_snapshot;
    uint32_t packet;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    packet = cpu->read_word(cpu->gpr[29] + 0x10u) - 8u;
    record = overlay_ft4_find(packet);
    if (record == NULL || !record->material_ready) {
        const uint32_t outer_return = cpu->read_word(cpu->gpr[29] + 0x64u);
        uint32_t slot;

        if (snapshot != NULL) {
            ++snapshot->projected_missing_material_count;
            for (slot = 0u; slot < snapshot->projected_missing_outer_count;
                 ++slot) {
                if (physical_address_equals(
                        snapshot->projected_missing_outer_returns[slot],
                        outer_return))
                    break;
            }
            if (slot < snapshot->projected_missing_outer_count) {
                ++snapshot->projected_missing_outer_counts[slot];
            } else if (slot < 16u) {
                snapshot->projected_missing_outer_returns[slot] =
                    xg_render_runtime_guest_address(outer_return);
                snapshot->projected_missing_outer_counts[slot] = 1u;
                ++snapshot->projected_missing_outer_count;
            } else {
                ++snapshot->projected_missing_outer_overflow;
            }
        }
        return false;
    }
    xg_render_runtime_capture_shadow_projection(cpu, &input.projection);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[4u + vertex];
        input.vertices[vertex] = (XgHost3dVector){
            (int16_t)cpu->read_half(address),
            (int16_t)cpu->read_half(address + 2u),
            (int16_t)cpu->read_half(address + 4u),
            cpu->read_half(address + 6u),
        };
    }
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return false;
    if (!record->valid) {
        XgRenderQuadSource source = {0};
        source.material = record->material;
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            source.vertices[vertex] = (XgRenderQuadSourceVertex){
                .red = 0x80u,
                .green = 0x80u,
                .blue = 0x80u,
            };
            xg_render_quad_set_projected_position(
                &source.vertices[vertex], &output.vertices[vertex]);
        }
        if (xg_render_quad_build_primitive(&source, &record->primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            return false;
    } else {
        set_projected_position(&record->primitive, &output);
    }
    record->valid = true;
    record->family = 3u;
    if (snapshot != NULL) ++snapshot->projected_geometry_count;
    return true;
}

bool xg_render_overlay_ft4_capture_projected_2e_geometry(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    uint32_t packet;
    XgRenderOverlayFt4Template *record;
    uint8_t family;

    if (cpu == NULL || cpu->read_word == NULL) return false;
    packet = cpu->read_word(cpu->gpr[29] + 0x10u) - 8u;
    record = overlay_ft4_find(packet);
    if (record == NULL || record->family < 6u || record->family > 10u)
        return false;
    family = record->family;
    if (!xg_render_overlay_ft4_capture_projected_geometry(
            cpu, render_mode, services))
        return false;
    record = overlay_ft4_find(packet);
    if (record == NULL) return false;
    record->family = family;
    ++overlay_snapshot.projected_2e_geometry_count;
    return true;
}

bool xg_render_overlay_ft4_observe_add_prim(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    XgRenderOverlayFt4Template *record;
    PsxXgRenderOverlayFt4Snapshot *snapshot = &overlay_snapshot;

    if (!physical_address_equals(pc, UINT32_C(0x80043b48)) ||
        instruction_word != UINT32_C(0x3c0600ff))
        return false;
    if (cpu == NULL || render_mode != GUEST_RENDER_RENDER_NATIVE ||
        services == NULL || services->lifecycle == NULL ||
        services->lifecycle->matches == NULL)
        return true;
    record = overlay_ft4_find(cpu->gpr[5]);
    if (record == NULL || !services->lifecycle->matches(&record->lifecycle))
        return true;
    if (record->family >= 4u && !record->material_ready) {
        snapshot->substitution_blocker = 9u;
        return true;
    }
    if (record->family == 1u)
        ++snapshot->direct_add_prim_count;
    else if (record->family == 2u)
        ++snapshot->rectangle_add_prim_count;
    else if (record->family == 3u)
        ++snapshot->projected_add_prim_count;
    else if (record->family >= 6u && record->family <= 10u)
        ++snapshot->projected_2e_add_prim_count;
    else
        ++snapshot->field_add_prim_count;
    snapshot->last_packet = xg_render_runtime_guest_address(cpu->gpr[5]);
    snapshot->last_ot = xg_render_runtime_guest_address(cpu->gpr[4]);
    if (services->stage_primitive == NULL || !services->stage_primitive(
            &record->primitive, record->packet_address,
            record->source_primitive_index,
            record->interpolation_producer_id,
            record->interpolation_primitive_id)) {
        if (record->family == 1u)
            ++snapshot->direct_stage_failure_count;
        else if (record->family == 2u)
            ++snapshot->rectangle_stage_failure_count;
        else if (record->family == 3u)
            ++snapshot->projected_stage_failure_count;
        else if (record->family >= 6u && record->family <= 10u)
            ++snapshot->projected_2e_stage_failure_count;
        else
            ++snapshot->field_stage_failure_count;
        snapshot->substitution_blocker = 4u;
        return true;
    }
    if (record->family == 1u)
        ++snapshot->direct_native_count;
    else if (record->family == 2u)
        ++snapshot->rectangle_native_count;
    else if (record->family == 3u)
        ++snapshot->projected_native_count;
    else if (record->family >= 6u && record->family <= 10u)
        ++snapshot->projected_2e_native_count;
    else
        ++snapshot->field_native_count;
    snapshot->substitution_blocker = 0u;
    return true;
}

bool xg_render_overlay_ft4_observe_field_material(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    XgRenderOverlayFt4Template *record;
    uint32_t family;
    uint32_t packet;
    uint32_t packet_offset;
    uint8_t color;
    bool semi_transparent;

    if (physical_address_equals(pc, UINT32_C(0x801ced74)) &&
        instruction_word == UINT32_C(0xa05208c4)) {
        family = 5u;
        semi_transparent = true;
        packet_offset = 0x8c0u;
    } else if (physical_address_equals(pc, UINT32_C(0x801cef58)) &&
               instruction_word == UINT32_C(0xa0520006)) {
        family = 4u;
        semi_transparent = true;
        packet_offset = 0u;
    } else if (physical_address_equals(pc, UINT32_C(0x801cf074)) &&
               instruction_word == UINT32_C(0xa05208c4)) {
        family = 5u;
        semi_transparent = false;
        packet_offset = 0x8c0u;
    } else if (physical_address_equals(pc, UINT32_C(0x801cf258)) &&
               instruction_word == UINT32_C(0xa0520006)) {
        family = 4u;
        semi_transparent = false;
        packet_offset = 0u;
    } else {
        return false;
    }
    if (cpu == NULL || render_mode != GUEST_RENDER_RENDER_NATIVE ||
        services == NULL || services->invalidate_field_sprite_template == NULL)
        return true;
    packet = cpu->gpr[2] + packet_offset;
    record = overlay_ft4_find(packet);
    if (record == NULL || record->family != family) return true;
    services->invalidate_field_sprite_template(packet);
    color = (uint8_t)cpu->gpr[18];
    record->primitive.material.tpage |= UINT16_C(0x20);
    record->primitive.material.blend_mode = (XgRenderIrBlendMode)(
        (record->primitive.material.tpage >> 5u) & 3u);
    record->primitive.material.raw_texture = false;
    record->primitive.material.semi_transparent = semi_transparent;
    record->material = record->primitive.material;
    for (uint32_t triangle = 0u;
         triangle < record->primitive.triangle_count; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *target =
                &record->primitive.triangles[triangle].vertices[vertex];
            target->r = color;
            target->g = color;
            target->b = color;
        }
    }
    record->material_ready = true;
    ++overlay_snapshot.field_material_count;
    return true;
}

XgRenderOverlayFt4Observation xg_render_overlay_ft4_observe(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    static const uint32_t caller_calls[10] = {
        UINT32_C(0x801cd984), UINT32_C(0x801d3724),
        UINT32_C(0x801d4fb4), UINT32_C(0x801d5fa8),
        UINT32_C(0x801e6304), UINT32_C(0x801e574c),
        UINT32_C(0x801e5be4), UINT32_C(0x801e5eb4),
        UINT32_C(0x801e739c), UINT32_C(0x801e7cb0),
    };
    static const uint32_t caller_finishes[10] = {
        UINT32_C(0x801cdb10), UINT32_C(0x801d3970),
        UINT32_C(0x801d50e0), UINT32_C(0x801d6188),
        UINT32_C(0x801e6444), UINT32_C(0x801e5918),
        UINT32_C(0x801e5e40), UINT32_C(0x801e61a4),
        UINT32_C(0x801e76e0), UINT32_C(0x801e7e5c),
    };
    static const uint32_t finish_instructions[10] = {
        UINT32_C(0x27bd0058), UINT32_C(0x27bd0038),
        UINT32_C(0x27bd0038), UINT32_C(0x27bd0038),
        UINT32_C(0x27bd0040), UINT32_C(0x27bd0048),
        UINT32_C(0x27bd0030), UINT32_C(0x27bd0038),
        UINT32_C(0x27bd0030), UINT32_C(0x27bd0040),
    };

    if (physical_address_equals(pc, UINT32_C(0x801e7c50)) &&
        instruction_word == UINT32_C(0x27bdffc0)) {
        if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !xg_render_overlay_ft4_capture_projected_material(
                cpu, pc, render_mode, services))
            overlay_snapshot.substitution_blocker = 7u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801e920c)) &&
        instruction_word == UINT32_C(0x00a01821) && cpu != NULL &&
        (overlay_ft4_find(cpu->gpr[4]) != NULL ||
         physical_address_equals(cpu->gpr[31], UINT32_C(0x801e63f4)))) {
        if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !xg_render_overlay_ft4_capture_rectangle_template(
                cpu, pc, render_mode, services))
            overlay_snapshot.substitution_blocker = 5u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801ce23c)) &&
        instruction_word == UINT32_C(0x0c0129cf)) {
        if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !xg_render_overlay_ft4_capture_projected_geometry(
                cpu, render_mode, services))
            overlay_snapshot.substitution_blocker = 8u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801cf85c)) &&
        instruction_word == UINT32_C(0x0c073866)) {
        if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
            !xg_render_overlay_ft4_capture_glyph_material(
                cpu, pc, render_mode, services))
            overlay_snapshot.substitution_blocker = 10u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d3db0)) &&
        instruction_word == UINT32_C(0x27bdffb8)) {
        projected_2e_descriptor_scope = true;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d3ff0)) &&
        instruction_word == UINT32_C(0x03e00008)) {
        projected_2e_descriptor_scope = false;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d3ff8)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!xg_render_overlay_ft4_capture_projected_2e_material(
                cpu, 3u, pc, render_mode, services))
            overlay_snapshot.substitution_blocker = 10u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d433c)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!xg_render_overlay_ft4_capture_projected_2e_material(
                cpu, 2u, pc, render_mode, services))
            overlay_snapshot.substitution_blocker = 10u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d4688)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!xg_render_overlay_ft4_capture_projected_2e_material(
                cpu, 1u, pc, render_mode, services))
            overlay_snapshot.substitution_blocker = 10u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d49d0)) &&
        instruction_word == UINT32_C(0x27bdffc8)) {
        if (!xg_render_overlay_ft4_capture_projected_2e_material(
                cpu, 0u, pc, render_mode, services))
            overlay_snapshot.substitution_blocker = 10u;
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_HANDLED;
    }
    if (physical_address_equals(pc, UINT32_C(0x801d09b0)) &&
        instruction_word == UINT32_C(0x0c0129cf))
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_PROJECTED_GOURAUD_2E;
    if (physical_address_equals(pc, UINT32_C(0x801d0bdc)) &&
        instruction_word == UINT32_C(0x0c0129cf))
        return XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_PROJECTED_GOURAUD;

    if (physical_address_equals(pc, UINT32_C(0x801e927c))) {
        if (instruction_word == UINT32_C(0x27bdffe8)) {
            ++overlay_snapshot.producer_entry_count;
            overlay_snapshot.last_pc = xg_render_runtime_guest_address(pc);
        } else {
            ++overlay_snapshot.rejected_site_count;
        }
    } else if (physical_address_equals(pc, UINT32_C(0x801e92c4))) {
        if (instruction_word == UINT32_C(0x03e00008)) {
            ++overlay_snapshot.producer_return_count;
            overlay_snapshot.last_pc = xg_render_runtime_guest_address(pc);
        } else {
            ++overlay_snapshot.rejected_site_count;
        }
    } else {
        for (uint32_t caller = 0u; caller < 10u; ++caller) {
            if (physical_address_equals(pc, caller_calls[caller])) {
                if (instruction_word != UINT32_C(0x0c07a49f)) {
                    ++overlay_snapshot.rejected_site_count;
                    break;
                }
                ++overlay_snapshot.caller_call_count;
                if (caller < 5u)
                    ++overlay_snapshot.rectangle_helper_count;
                else if (caller < 9u)
                    ++overlay_snapshot.static_quad_count;
                else
                    ++overlay_snapshot.dynamic_uv_template_count;
                overlay_snapshot.last_pc = xg_render_runtime_guest_address(pc);
                break;
            }
            if (physical_address_equals(pc, caller_finishes[caller])) {
                if (instruction_word == finish_instructions[caller]) {
                    ++overlay_snapshot.caller_finish_count;
                    overlay_snapshot.last_pc =
                        xg_render_runtime_guest_address(pc);
                    if (caller == 8u &&
                        render_mode == GUEST_RENDER_RENDER_NATIVE &&
                        !xg_render_overlay_ft4_capture_direct_templates(
                            cpu, pc, render_mode, services) &&
                        overlay_snapshot.substitution_blocker == 0u)
                        overlay_snapshot.substitution_blocker = 2u;
                    if (caller == 7u)
                        return XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_STATIC_GOURAUD;
                } else {
                    ++overlay_snapshot.rejected_site_count;
                }
                break;
            }
        }
    }
    return XG_RENDER_OVERLAY_FT4_OBSERVATION_NONE;
}

void xg_render_overlay_ft4_finish_observation(
        XgRenderOverlayFt4Observation observation, CPUState *cpu,
        GuestRenderRenderMode render_mode,
        const XgRenderOverlayFt4Services *services) {
    if (observation ==
            XG_RENDER_OVERLAY_FT4_OBSERVATION_RESIDUAL_PROJECTED_GOURAUD_2E &&
        !xg_render_overlay_ft4_capture_projected_2e_geometry(
            cpu, render_mode, services))
        overlay_snapshot.substitution_blocker = 11u;
}

bool xg_render_overlay_ft4_projected_2e_descriptor_scope(void) {
    return projected_2e_descriptor_scope;
}

void xg_render_overlay_ft4_scene_boundary(void) {
    projected_2e_descriptor_scope = false;
}

void xg_render_overlay_ft4_snapshot(
        PsxXgRenderOverlayFt4Snapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = overlay_snapshot;
}

void xg_render_overlay_ft4_clear(void) {
    template_count = 0u;
}

void xg_render_overlay_ft4_invalidate(uint32_t address, uint32_t size) {
    for (uint32_t index = 0u; index < template_count; ++index) {
        XgRenderOverlayFt4Template *record = &templates[index];

        if (normalized_ranges_overlap(
                record->packet_address, 0x28u, address, size))
            record->valid = false;
    }
}

void xg_render_overlay_ft4_reset(void) {
    template_count = 0u;
    overlay_snapshot = (PsxXgRenderOverlayFt4Snapshot){0};
    projected_2e_descriptor_scope = false;
    local_producer_pending = (XgRenderOverlayFt4LocalProducerPending){0};
}

void xg_render_overlay_ft4_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.resource_mutation)
            xg_render_overlay_ft4_invalidate(event->address, event->size);
        if (event->mutation.semantic_authority_loss)
            xg_render_overlay_ft4_clear();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
               event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
               event->kind == XG_RENDER_INVALIDATION_RESOURCE_OVERLAP) {
        xg_render_overlay_ft4_clear();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_render_overlay_ft4_clear();
        xg_render_overlay_ft4_scene_boundary();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_overlay_ft4_reset();
    }
}
