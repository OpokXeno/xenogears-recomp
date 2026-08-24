#include "xg_field_character_capture.h"

#include <stddef.h>
#include <string.h>

enum {
    NORMAL_TEMPLATE_BASE = 0x800b06bc,
    NORMAL_TEMPLATE_COUNT = 16,
    SPECIAL_TEMPLATE_BASE = 0x800b0dbc,
    SPECIAL_TEMPLATE_COUNT = 5,
    TEMPLATE_STRIDE = 0x70,
    FIRST_PACKET_OFFSET = 0x20,
    SECOND_PACKET_OFFSET = 0x48,
    U_TABLE_BASE = 0x800add70,
    V_TABLE_BASE = 0x800addb8,
    MATERIAL_TABLE_BASE = 0x800ade00,
    TABLE_RECORD_SIZE = 8,
    MATERIAL_RECORD_SIZE = 12,
};

typedef struct XgFieldCharacterTemplateIdentity {
    uint32_t u_index;
    uint32_t v_index;
    uint32_t material_index;
} XgFieldCharacterTemplateIdentity;

static bool identify_in_range(uint32_t packet_address, uint32_t base,
                              uint32_t count, uint32_t *out_index) {
    const uint64_t begin = base;
    const uint64_t end = begin + (uint64_t)count * TEMPLATE_STRIDE;
    const uint64_t address = packet_address;
    uint32_t offset;
    uint32_t within;

    if (address < begin || address >= end) return false;
    offset = packet_address - base;
    within = offset % TEMPLATE_STRIDE;
    if (within != FIRST_PACKET_OFFSET && within != SECOND_PACKET_OFFSET)
        return false;
    *out_index = offset / TEMPLATE_STRIDE;
    return true;
}

static bool identify_template(uint32_t packet_address,
                              XgFieldCharacterTemplateIdentity *out_identity) {
    uint32_t index;

    if (identify_in_range(packet_address, NORMAL_TEMPLATE_BASE,
                          NORMAL_TEMPLATE_COUNT, &index)) {
        *out_identity = (XgFieldCharacterTemplateIdentity){
            .u_index = index & 3u,
            .v_index = index >> 2u,
            .material_index = 0u,
        };
        return true;
    }
    if (identify_in_range(packet_address, SPECIAL_TEMPLATE_BASE,
                          SPECIAL_TEMPLATE_COUNT, &index)) {
        *out_identity = (XgFieldCharacterTemplateIdentity){
            .u_index = 4u + index,
            .v_index = 4u + index,
            .material_index = 1u,
        };
        return true;
    }
    return false;
}

static bool read_record(const XgFieldCharacterSemanticReader *reader,
                        uint32_t address, uint16_t *values, size_t count) {
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (!reader->read_u16(reader->context, address + (uint32_t)index * 2u,
                              &values[index]))
            return false;
    }
    return true;
}

static uint8_t clamp_texture_coordinate(uint16_t raw) {
    const int16_t value = (int16_t)raw;

    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

XgFieldCharacterCaptureResult xg_field_character_capture_build(
    const XgFieldCharacterGeometryInput *geometry,
    const XgFieldCharacterDrawStateInput *draw_state,
    const XgFieldCharacterSemanticReader *reader,
    XgFieldCharacterCapture *out_capture) {
    XgFieldCharacterTemplateIdentity identity;
    uint16_t u_values[XG_FIELD_CHARACTER_VERTEX_COUNT];
    uint16_t v_values[XG_FIELD_CHARACTER_VERTEX_COUNT];
    uint16_t material[6];
    size_t index;

    if (!geometry || !draw_state || !reader || !reader->read_u16 ||
        !out_capture)
        return XG_FIELD_CHARACTER_CAPTURE_INVALID_ARGUMENT;
    memset(out_capture, 0, sizeof(*out_capture));
    if (geometry->semantic_template ==
        XG_FIELD_CHARACTER_SEMANTIC_DYNAMIC_ACTOR) {
        static const uint8_t u[4] = { 0u, 15u, 0u, 15u };
        static const uint8_t v[4] = { 224u, 224u, 239u, 239u };

        for (index = 0u; index < XG_FIELD_CHARACTER_VERTEX_COUNT; ++index) {
            out_capture->vertices[index].x = geometry->x[index];
            out_capture->vertices[index].y = geometry->y[index];
            out_capture->vertices[index].u = u[index];
            out_capture->vertices[index].v = v[index];
        }
        out_capture->red = 0x80u;
        out_capture->green = 0x80u;
        out_capture->blue = 0x80u;
        out_capture->tpage = 0x5au;
        out_capture->clut_x = 256u;
        out_capture->clut_y = 243u;
        out_capture->semi_transparent = 1u;
        goto copy_draw_state;
    }
    if (geometry->semantic_template !=
        XG_FIELD_CHARACTER_SEMANTIC_STATIC_TABLE)
        return XG_FIELD_CHARACTER_CAPTURE_UNMAPPED_TEMPLATE;
    if (!identify_template(geometry->packet_guest_address, &identity))
        return XG_FIELD_CHARACTER_CAPTURE_UNMAPPED_TEMPLATE;
    if (!read_record(reader, U_TABLE_BASE + identity.u_index * TABLE_RECORD_SIZE,
                     u_values, XG_FIELD_CHARACTER_VERTEX_COUNT) ||
        !read_record(reader, V_TABLE_BASE + identity.v_index * TABLE_RECORD_SIZE,
                     v_values, XG_FIELD_CHARACTER_VERTEX_COUNT) ||
        !read_record(reader,
                     MATERIAL_TABLE_BASE +
                         identity.material_index * MATERIAL_RECORD_SIZE,
                     material, 6u))
        return XG_FIELD_CHARACTER_CAPTURE_SEMANTIC_READ_FAILED;
    if (material[0] > 2u || material[1] > 3u || material[2] > 1023u ||
        material[3] > 511u || material[4] > 1008u ||
        (material[4] & 15u) != 0u || material[5] > 511u)
        return XG_FIELD_CHARACTER_CAPTURE_INVALID_MATERIAL;

    for (index = 0u; index < XG_FIELD_CHARACTER_VERTEX_COUNT; ++index) {
        out_capture->vertices[index].x = geometry->x[index];
        out_capture->vertices[index].y = geometry->y[index];
        out_capture->vertices[index].u = clamp_texture_coordinate(u_values[index]);
        out_capture->vertices[index].v = clamp_texture_coordinate(v_values[index]);
    }
    out_capture->red = 0x80u;
    out_capture->green = 0x80u;
    out_capture->blue = 0x80u;
    out_capture->tpage = (uint16_t)((material[0] << 7u) |
                                    (material[1] << 5u) |
                                    ((material[3] & 0x100u) >> 4u) |
                                    ((material[2] & 0x3ffu) >> 6u));
    out_capture->clut_x = material[4];
    out_capture->clut_y = material[5];
copy_draw_state:
    out_capture->draw_area_left = draw_state->draw_area_left;
    out_capture->draw_area_top = draw_state->draw_area_top;
    out_capture->draw_area_right = draw_state->draw_area_right;
    out_capture->draw_area_bottom = draw_state->draw_area_bottom;
    out_capture->draw_offset_x = draw_state->draw_offset_x;
    out_capture->draw_offset_y = draw_state->draw_offset_y;
    out_capture->texture_window_mask_x = draw_state->texture_window_mask_x;
    out_capture->texture_window_mask_y = draw_state->texture_window_mask_y;
    out_capture->texture_window_offset_x = draw_state->texture_window_offset_x;
    out_capture->texture_window_offset_y = draw_state->texture_window_offset_y;
    out_capture->dither = draw_state->dither;
    out_capture->mask_set = draw_state->mask_set;
    out_capture->mask_check = draw_state->mask_check;
    return XG_FIELD_CHARACTER_CAPTURE_OK;
}
