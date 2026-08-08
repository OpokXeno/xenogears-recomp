#include "xg_field_compass.h"

#include "cpu_state.h"

#include <stddef.h>

static uint8_t clamp_uv(int16_t value) {
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

static bool material_indices(uint32_t source_address,
                             uint32_t *out_u_index,
                             uint32_t *out_v_index,
                             uint32_t *out_material_index,
                             uint32_t *out_special_index) {
    const uint32_t physical = source_address & UINT32_C(0x1fffffff);

    if (physical >= UINT32_C(0x000b06bc) &&
        physical <= UINT32_C(0x000b0d4c) &&
        (physical - UINT32_C(0x000b06bc)) % 0x70u == 0u) {
        const uint32_t index =
            (physical - UINT32_C(0x000b06bc)) / 0x70u;
        *out_u_index = index % 4u;
        *out_v_index = index / 4u;
        *out_material_index = 0u;
        *out_special_index = UINT32_MAX;
        return true;
    }
    if (physical >= UINT32_C(0x000b0dbc) &&
        physical <= UINT32_C(0x000b0f7c) &&
        (physical - UINT32_C(0x000b0dbc)) % 0x70u == 0u) {
        const uint32_t index =
            (physical - UINT32_C(0x000b0dbc)) / 0x70u + 4u;
        *out_u_index = index;
        *out_v_index = index;
        *out_material_index = 1u;
        *out_special_index = UINT32_MAX;
        return true;
    }
    if (physical >= UINT32_C(0x000b0fec) &&
        physical <= UINT32_C(0x000b113c) &&
        (physical - UINT32_C(0x000b0fec)) % 0x70u == 0u) {
        const uint32_t index =
            (physical - UINT32_C(0x000b0fec)) / 0x70u;
        if (index >= 4u) return false;
        *out_u_index = 0u;
        *out_v_index = 0u;
        *out_material_index = 0u;
        *out_special_index = index;
        return true;
    }
    return false;
}

bool xg_field_compass_capture_material(
    CPUState *cpu, uint32_t source_address,
    XgFieldCharacterCapture *capture) {
    uint32_t u_index;
    uint32_t v_index;
    uint32_t material_index;
    uint32_t special_index;
    uint32_t index;

    if (cpu == NULL || capture == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        !material_indices(source_address, &u_index, &v_index,
                          &material_index, &special_index))
        return false;
    capture->red = 0x80u;
    capture->green = 0x80u;
    capture->blue = 0x80u;
    if (special_index != UINT32_MAX) {
        const uint32_t table = UINT32_C(0x800ade70) + special_index * 8u;
        for (index = 0u; index < 4u; ++index) {
            capture->vertices[index].u = cpu->read_byte(table + index * 2u);
            capture->vertices[index].v =
                (uint8_t)(cpu->read_byte(table + index * 2u + 1u) + 0xc0u);
        }
        capture->tpage = UINT16_C(0x005a);
        capture->clut_x = UINT16_C(0x0100);
        capture->clut_y = UINT16_C(0x00f2);
        capture->semi_transparent = 1u;
        return true;
    }
    for (index = 0u; index < 4u; ++index) {
        capture->vertices[index].u = clamp_uv((int16_t)cpu->read_half(
            UINT32_C(0x800add70) + u_index * 8u + index * 2u));
        capture->vertices[index].v = clamp_uv((int16_t)cpu->read_half(
            UINT32_C(0x800addb8) + v_index * 8u + index * 2u));
    }
    {
        const uint32_t material = UINT32_C(0x800ade00) + material_index * 12u;
        const uint16_t texture_depth = cpu->read_half(material);
        const uint16_t blend = cpu->read_half(material + 2u);
        const uint16_t page_x = cpu->read_half(material + 4u);
        const uint16_t page_y = cpu->read_half(material + 6u);

        capture->tpage = (uint16_t)(((page_x >> 6u) & 0x0fu) |
            (((page_y >> 8u) & 1u) << 4u) | ((blend & 3u) << 5u) |
            ((texture_depth & 3u) << 7u));
        capture->clut_x = cpu->read_half(material + 8u);
        capture->clut_y = cpu->read_half(material + 10u);
    }
    capture->semi_transparent = 0u;
    return true;
}
