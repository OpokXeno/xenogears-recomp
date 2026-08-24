#include "xg_field_render_services.h"

#include "cpu_state.h"
#include "psx_cyc.h"

#include <limits.h>

uint32_t xg_render_runtime_guest_address(uint32_t address) {
    return address == 0u ? 0u : (address & UINT32_C(0x1fffffff)) |
        UINT32_C(0x80000000);
}

int16_t xg_render_runtime_low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

bool xg_render_runtime_vector_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    return (segment == 0u || segment == UINT32_C(0x80000000) ||
            segment == UINT32_C(0xa0000000)) &&
           physical <= UINT32_C(0x001ffff8) && (physical & 1u) == 0u;
}

bool xg_render_runtime_stack_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    const bool valid_segment =
        segment == 0u || segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);

    return valid_segment && (physical & 3u) == 0u &&
           (physical <= UINT32_C(0x001fffd8) ||
            (physical >= UINT32_C(0x1f800000) &&
             physical <= UINT32_C(0x1f8003d8)));
}

bool xg_render_runtime_word_address_is_valid(uint32_t address) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    const bool valid_segment =
        segment == 0u || segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);

    return valid_segment && (physical & 3u) == 0u &&
           (physical <= UINT32_C(0x001ffffc) ||
            (physical >= UINT32_C(0x1f800000) &&
             physical <= UINT32_C(0x1f8003fc)));
}

void xg_render_runtime_capture_shadow_projection(
        const CPUState *cpu, XgHost3dProjection *projection) {
    const uint32_t *control = cpu->gte_ctrl;

    projection->rotation[0][0] = xg_render_runtime_low_s16(control[0]);
    projection->rotation[0][1] = xg_render_runtime_low_s16(control[0] >> 16u);
    projection->rotation[0][2] = xg_render_runtime_low_s16(control[1]);
    projection->rotation[1][0] = xg_render_runtime_low_s16(control[1] >> 16u);
    projection->rotation[1][1] = xg_render_runtime_low_s16(control[2]);
    projection->rotation[1][2] = xg_render_runtime_low_s16(control[2] >> 16u);
    projection->rotation[2][0] = xg_render_runtime_low_s16(control[3]);
    projection->rotation[2][1] = xg_render_runtime_low_s16(control[3] >> 16u);
    projection->rotation[2][2] = xg_render_runtime_low_s16(control[4]);
    projection->translation[0] = (int32_t)control[5];
    projection->translation[1] = (int32_t)control[6];
    projection->translation[2] = (int32_t)control[7];
    projection->screen_offset_x = (int32_t)control[24];
    projection->screen_offset_y = (int32_t)control[25];
    projection->projection_distance = (uint16_t)control[26];
    projection->depth_cue_a = xg_render_runtime_low_s16(control[27]);
    projection->depth_cue_b = (int32_t)control[28];
    projection->average_z_scale4 = xg_render_runtime_low_s16(control[30]);
}

bool xg_render_runtime_capture_matrix(
        CPUState *cpu, uint32_t address, XgHost3dMatrix *matrix) {
    uint32_t word;

    if (cpu == NULL || matrix == NULL || cpu->read_word == NULL ||
        address > UINT32_MAX - 28u ||
        !xg_render_runtime_word_address_is_valid(address) ||
        !xg_render_runtime_word_address_is_valid(address + 28u))
        return false;
    word = cpu->read_word(address);
    matrix->rotation[0][0] = xg_render_runtime_low_s16(word);
    matrix->rotation[0][1] = xg_render_runtime_low_s16(word >> 16u);
    word = cpu->read_word(address + 4u);
    matrix->rotation[0][2] = xg_render_runtime_low_s16(word);
    matrix->rotation[1][0] = xg_render_runtime_low_s16(word >> 16u);
    word = cpu->read_word(address + 8u);
    matrix->rotation[1][1] = xg_render_runtime_low_s16(word);
    matrix->rotation[1][2] = xg_render_runtime_low_s16(word >> 16u);
    word = cpu->read_word(address + 12u);
    matrix->rotation[2][0] = xg_render_runtime_low_s16(word);
    matrix->rotation[2][1] = xg_render_runtime_low_s16(word >> 16u);
    word = cpu->read_word(address + 16u);
    matrix->rotation[2][2] = xg_render_runtime_low_s16(word);
    matrix->pad = (uint16_t)(word >> 16u);
    matrix->translation[0] = (int32_t)cpu->read_word(address + 20u);
    matrix->translation[1] = (int32_t)cpu->read_word(address + 24u);
    matrix->translation[2] = (int32_t)cpu->read_word(address + 28u);
    return true;
}

void xg_render_runtime_store_matrix_rotation(
        CPUState *cpu, uint32_t address, const XgHost3dMatrix *matrix) {
    const uint32_t words[5] = {
        (uint16_t)matrix->rotation[0][0] |
            ((uint32_t)(uint16_t)matrix->rotation[0][1] << 16u),
        (uint16_t)matrix->rotation[0][2] |
            ((uint32_t)(uint16_t)matrix->rotation[1][0] << 16u),
        (uint16_t)matrix->rotation[1][1] |
            ((uint32_t)(uint16_t)matrix->rotation[1][2] << 16u),
        (uint16_t)matrix->rotation[2][0] |
            ((uint32_t)(uint16_t)matrix->rotation[2][1] << 16u),
        (uint16_t)matrix->rotation[2][2] | ((uint32_t)matrix->pad << 16u),
    };

    for (uint32_t index = 0u; index < 5u; ++index) {
        psx_store_cycle_barrier();
        cpu->write_word(address + index * 4u, words[index]);
    }
}
