#include "xg_render_resident_capture.h"
#include "xg_render_quad_builder.h"
#include "cpu_state.h"

#include <stdlib.h>

XgRenderResidentCaptureResult xg_render_resident_capture_resource_templates(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        const XgRenderResidentResourceTemplateServices *services) {
    XgRenderResidentResourceTemplate *records;
    XgRenderIrMaterialState draw_state;
    uint32_t source, table_entry, glyph, count, offset;
    int16_t origin_x, origin_y;
    uint16_t scale;
    XgRenderResidentCaptureResult result = XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;

    if (cpu == NULL || services == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL ||
        services->authorize == NULL || services->source_range_valid == NULL ||
        services->capture_draw_state == NULL || services->publish_templates == NULL ||
        (pc & UINT32_C(0x1fffffff)) != UINT32_C(0x2675c) ||
        instruction_word != UINT32_C(0x27bdffb0) ||
        !services->authorize(services->context, pc, instruction_word))
        return XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;
    source = cpu->gpr[4];
    if (source == 0u || source > UINT32_MAX - 4u || cpu->gpr[29] > UINT32_MAX - 0x1au ||
        cpu->gpr[7] > 1u ||
        !services->source_range_valid(services->context, source, 4u, 4u) ||
        !services->source_range_valid(services->context, cpu->gpr[29] + 0x10u,
                                     10u, 2u) ||
        cpu->gpr[5] >= cpu->read_word(source) ||
        cpu->gpr[5] > (UINT32_MAX - source - 4u) / 2u)
        return result;
    table_entry = source + 4u + cpu->gpr[5] * 2u;
    if (!services->source_range_valid(services->context, table_entry, 2u, 2u))
        return result;
    offset = cpu->read_half(table_entry);
    if (offset > UINT32_MAX - source) return result;
    glyph = source + offset;
    if (glyph > UINT32_MAX - 4u ||
        !services->source_range_valid(services->context, glyph, 4u, 2u))
        return result;
    count = cpu->read_half(glyph);
    if (count > (UINT32_MAX - glyph - 4u) / 0x1cu ||
        (count != 0u &&
         ((uint64_t)cpu->gpr[6] + (uint64_t)(count - 1u) * 0x50u +
              cpu->gpr[7] * 0x28u + 0x28u > (uint64_t)UINT32_MAX + 1u ||
          !services->source_range_valid(services->context, glyph + 4u,
                                       count * 0x1cu, 2u))))
        return result;
    if (!services->capture_draw_state(services->context, &draw_state)) return result;
    origin_x = (int16_t)cpu->read_half(cpu->gpr[29] + 0x10u);
    origin_y = (int16_t)cpu->read_half(cpu->gpr[29] + 0x14u);
    scale = cpu->read_half(cpu->gpr[29] + 0x18u);
    records = count != 0u ? calloc(count, sizeof(*records)) : NULL;
    if (count != 0u && records == NULL) return XG_RENDER_RESIDENT_CAPTURE_CAPACITY_EXCEEDED;
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t address = glyph + 4u + index * 0x1cu;
        XgRenderQuadSource quad = {0};
        /* slus_006.64:8002675c uses signed dimensions/offsets, unsigned 4.12
         * scale, truncation toward zero, then halfword coordinate stores. */
        int32_t left = origin_x + (int32_t)(int16_t)cpu->read_half(address + 8u) * scale / 4096;
        int32_t top = origin_y + (int32_t)(int16_t)cpu->read_half(address + 10u) * scale / 4096;
        int32_t right = left + (int32_t)(int16_t)cpu->read_half(address + 4u) * scale / 4096;
        int32_t bottom = top + (int32_t)(int16_t)cpu->read_half(address + 6u) * scale / 4096;
        uint16_t u = cpu->read_half(address);
        uint16_t v = cpu->read_half(address + 2u);
        uint8_t width = cpu->read_byte(address + 4u);
        uint8_t height = cpu->read_byte(address + 6u);
        const uint16_t depth = cpu->read_half(address + 16u);
        const uint16_t clut_x = cpu->read_half(address + 18u);
        const uint16_t clut_y = cpu->read_half(address + 20u);
        const uint16_t page_x = cpu->read_half(address + 22u);
        const uint16_t page_y = cpu->read_half(address + 24u);
        if (depth > XG_RENDER_IR_TEXTURE_15_BIT) goto finished;
        if (cpu->read_byte(address + 26u) != 0u) {
            int32_t swap = left; left = right; right = swap;
            u = (uint16_t)(u - 1u);
            if ((int16_t)u < 0) { u = 0u; --width; }
        }
        if (cpu->read_byte(address + 27u) != 0u) {
            int32_t swap = top; top = bottom; bottom = swap;
            v = (uint16_t)(v - 1u);
            if ((int16_t)v < 0) { v = 0u; --height; }
        }
        quad.material = draw_state;
        quad.material.tpage = (uint16_t)((depth << 7u) |
            ((page_y & 0x100u) >> 4u) | ((page_x & 0x3ffu) >> 6u) |
            ((page_y & 0x200u) << 2u));
        quad.material.texture_page_x = quad.material.tpage & 15u;
        quad.material.texture_page_y = (quad.material.tpage >> 4u) & 1u;
        quad.material.texture_depth = (XgRenderIrTextureDepth)depth;
        quad.material.clut_x = clut_x & 0x3f0u;
        quad.material.clut_y = clut_y & 0x1ffu;
        quad.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
        quad.material.shading = XG_RENDER_IR_SHADING_FLAT;
        quad.material.textured = true;
        quad.material.raw_texture = true;
        quad.material.semi_transparent = false;
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            quad.vertices[vertex].x = (int16_t)((vertex & 1u) ? right : left);
            quad.vertices[vertex].y = (int16_t)((vertex & 2u) ? bottom : top);
            quad.vertices[vertex].u = (uint8_t)(u + ((vertex & 1u) ? width : 0u));
            quad.vertices[vertex].v = (uint8_t)(v + ((vertex & 2u) ? height : 0u));
        }
        if (xg_render_quad_build_primitive(&quad, &records[index].primitive) !=
                XG_RENDER_QUAD_BUILDER_OK)
            goto finished;
        records[index].descriptor_address = address;
        records[index].destination_address = cpu->gpr[6] +
            index * 0x50u + cpu->gpr[7] * 0x28u;
    }
    result = services->publish_templates(services->context, cpu->gpr[6],
        cpu->gpr[7], records, count) ? XG_RENDER_RESIDENT_CAPTURE_OK : XG_RENDER_RESIDENT_CAPTURE_EMIT_FAILED;
finished:
    free(records);
    return result;
}

XgRenderResidentCaptureResult xg_render_resident_capture_font_character(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        const XgRenderResidentResourceTemplateServices *services) {
    XgRenderResidentResourceTemplate record = {0};
    XgRenderQuadSource quad = {0};
    uint32_t font, glyph, advance, destination;
    int32_t character, x, y, line_height, count, maximum;
    uint16_t flags, tpage, clut, cell_size;
    uint8_t code, u, v;

    if (cpu == NULL || services == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL ||
        services->authorize == NULL || services->source_range_valid == NULL ||
        services->capture_draw_state == NULL || services->publish_templates == NULL ||
        (pc & UINT32_C(0x1fffffff)) != UINT32_C(0x370dc) ||
        instruction_word != UINT32_C(0x3c068006) ||
        !services->authorize(services->context, pc, instruction_word) ||
        !services->source_range_valid(services->context, UINT32_C(0x80059394), 4u, 4u))
        return XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;
    font = cpu->read_word(UINT32_C(0x80059394));
    if (font == 0u) return XG_RENDER_RESIDENT_CAPTURE_OK;
    if (font > UINT32_MAX - 0xd4u ||
        !services->source_range_valid(services->context, font, 0xd4u, 4u))
        return XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;
    y = (int16_t)cpu->read_half(font + 0x32u);
    line_height = (int16_t)cpu->read_half(font + 0x16u);
    count = (int16_t)cpu->read_half(font + 0x34u);
    maximum = (int16_t)cpu->read_half(font + 0x2cu);
    if (y + line_height > (int16_t)cpu->read_half(font + 0xeu) +
            (int32_t)(int16_t)cpu->read_half(font + 0x12u) || count > maximum)
        return XG_RENDER_RESIDENT_CAPTURE_OK;
    character = (int32_t)cpu->gpr[4];
    if (character < 32) return XG_RENDER_RESIDENT_CAPTURE_OK;
    flags = cpu->read_half(font + 0x2eu);
    if ((flags & 4u) != 0u && character > 0x5f) character -= 0x20;
    glyph = (uint32_t)character - 0x20u;
    if ((flags & 8u) != 0u) {
        if (character < 0x20 || character >= 0x80)
            return XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;
        advance = cpu->read_byte(font + (uint32_t)character + 0x44u);
    } else {
        advance = (uint32_t)(int32_t)(int16_t)cpu->read_half(font + 0x14u);
    }
    x = (int16_t)cpu->read_half(font + 0x30u);
    if ((int16_t)cpu->read_half(font + 0xcu) +
            (int32_t)(int16_t)cpu->read_half(font + 0x10u) <=
            (int32_t)((uint32_t)x + advance)) {
        if ((cpu->read_half(font) & 8u) != 0u) return XG_RENDER_RESIDENT_CAPTURE_OK;
        x = (int16_t)cpu->read_half(font + 0x36u);
        y = (int16_t)(y + line_height);
    }
    if (glyph == 0u) return XG_RENDER_RESIDENT_CAPTURE_OK;
    destination = cpu->read_word(font + 0x38u);
    if (destination > UINT32_MAX - 0x10u ||
        !services->capture_draw_state(services->context, &quad.material))
        return XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;
    /* FontLoadFont stores this color/style prototype outside the packet arena.
     * No emitted SPRT or ordering-table payload is read here. */
    code = cpu->read_byte(font + 0x1bu);
    cell_size = (flags & 2u) != 0u ? 16u : 8u;
    if ((code & 0xfcu) != (cell_size == 16u ? 0x7cu : 0x74u))
        return XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;
    if (cell_size == 8u) {
        clut = cpu->read_half(font + 0x3cu + ((glyph & 0x30u) >> 3u));
        u = (uint8_t)((glyph & 15u) << 3u);
        v = (uint8_t)(cpu->read_byte(font + 0xd2u) + ((glyph & 0xc0u) >> 3u));
    } else {
        clut = cpu->read_half(font + 0x3cu + ((glyph & 0x18u) >> 2u));
        u = (uint8_t)((glyph & 7u) << 4u);
        v = (uint8_t)(cpu->read_byte(font + 0xd2u) + ((glyph & 0x60u) >> 1u));
    }
    tpage = cpu->read_half(font + 2u);
    quad.material.tpage = tpage;
    quad.material.texture_page_x = tpage & 15u;
    quad.material.texture_page_y = (tpage >> 4u) & 1u;
    quad.material.texture_depth = (XgRenderIrTextureDepth)((tpage >> 7u) & 3u);
    quad.material.blend_mode = (XgRenderIrBlendMode)((tpage >> 5u) & 3u);
    quad.material.clut_x = (clut & 63u) * 16u;
    quad.material.clut_y = clut >> 6u;
    quad.material.shading = XG_RENDER_IR_SHADING_FLAT;
    quad.material.textured = true;
    quad.material.raw_texture = (code & 1u) != 0u;
    quad.material.semi_transparent = (code & 2u) != 0u;
    quad.material.dither = false; /* FontLoadFont's SetDrawTPage(...,0,0,...). */
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        quad.vertices[vertex].x = (int16_t)(x + ((vertex & 1u) ? cell_size : 0u));
        quad.vertices[vertex].y = (int16_t)(y + ((vertex & 2u) ? cell_size : 0u));
        quad.vertices[vertex].u = (uint8_t)(u + ((vertex & 1u) ? cell_size : 0u));
        quad.vertices[vertex].v = (uint8_t)(v + ((vertex & 2u) ? cell_size : 0u));
        quad.vertices[vertex].red = cpu->read_byte(font + 0x18u);
        quad.vertices[vertex].green = cpu->read_byte(font + 0x19u);
        quad.vertices[vertex].blue = cpu->read_byte(font + 0x1au);
    }
    if (xg_render_quad_build_primitive(&quad, &record.primitive) != XG_RENDER_QUAD_BUILDER_OK)
        return XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT;
    /* Cursor/style writes do not mutate already-emitted letters. This dynamic
     * context is copied source state, not a persistent asset watch. */
    record.descriptor_address = 0u;
    record.destination_address = destination;
    return services->publish_templates(services->context, destination, flags & 1u,
        &record, 1u) ? XG_RENDER_RESIDENT_CAPTURE_OK : XG_RENDER_RESIDENT_CAPTURE_EMIT_FAILED;
}
