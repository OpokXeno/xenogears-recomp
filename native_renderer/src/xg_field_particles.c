#include "xg_field_particles.h"

#include "cpu_state.h"
#include "xg_field_render_services.h"

#include <stddef.h>
#include <string.h>

static XgRenderParticleSourceState particle_sources = {
    .next_generation = 1u,
};
static uint32_t particle_source_count;

static uint8_t clamp_uv(int32_t value) {
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

void xg_field_particles_reset(void) {
    if (particle_source_count == 0u && !particle_sources.blocked) return;
    particle_sources = (XgRenderParticleSourceState){
        .next_generation = 1u,
    };
    particle_source_count = 0u;
}

XgRenderParticleSourceState *xg_field_particles_state(void) {
    return &particle_sources;
}

XgRenderParticleSource *xg_field_particles_find(uint32_t particle_address) {
    uint32_t index;

    particle_address = xg_render_runtime_guest_address(particle_address);
    for (index = 0u; index < XG_RENDER_PARTICLE_SOURCE_CAPACITY; ++index) {
        XgRenderParticleSource *record = &particle_sources.records[index];

        if (record->valid && record->particle_address == particle_address)
            return record;
    }
    return NULL;
}

void xg_field_particles_invalidate(uint32_t particle_address) {
    XgRenderParticleSource *record =
        xg_field_particles_find(particle_address);

    if (record != NULL) {
        *record = (XgRenderParticleSource){ 0 };
        --particle_source_count;
    }
}

bool xg_field_particles_observe_initializer(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    bool artifact_candidate_authorized) {
    XgRenderParticleSource *record;
    uint32_t particle_address;
    uint32_t table_address;
    uint32_t table_index;
    uint32_t abr;
    int16_t table[12];
    int16_t left;
    int16_t top;
    int16_t right;
    int16_t bottom;
    uint32_t index;

    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL || cpu == NULL ||
        cpu->read_half == NULL ||
        !xg_render_runtime_word_address_is_valid(cpu->gpr[4]) ||
        !artifact_candidate_authorized)
        return false;
    particle_address = xg_render_runtime_guest_address(cpu->gpr[4]);
    xg_field_particles_invalidate(particle_address);
    table_index = cpu->gpr[5];
    abr = cpu->gpr[6];
    if (particle_sources.blocked || table_index >= 21u || abr >= 4u ||
        particle_address > UINT32_MAX - 0xbcu ||
        !xg_render_runtime_word_address_is_valid(particle_address) ||
        !xg_render_runtime_word_address_is_valid(particle_address + 0xbcu))
        return false;
    record = NULL;
    for (index = 0u; index < XG_RENDER_PARTICLE_SOURCE_CAPACITY; ++index) {
        if (!particle_sources.records[index].valid) {
            record = &particle_sources.records[index];
            break;
        }
    }
    if (record == NULL || particle_sources.next_generation == 0u) {
        particle_sources.blocked = true;
        return false;
    }
    table_address = UINT32_C(0x800af27c) + table_index * 0x18u;
    for (index = 0u; index < 12u; ++index)
        table[index] = (int16_t)cpu->read_half(table_address + index * 2u);
    left = xg_render_runtime_low_s16(
        (uint32_t)((int32_t)table[2] - table[0]) * 16u);
    top = xg_render_runtime_low_s16(
        (uint32_t)((int32_t)table[3] - table[1]) * 16u);
    right = xg_render_runtime_low_s16(
        (uint32_t)((int32_t)table[0] + table[2]) * 16u);
    bottom = xg_render_runtime_low_s16(
        (uint32_t)((int32_t)table[1] + table[3]) * 16u);
    *record = (XgRenderParticleSource){
        .generation = particle_sources.next_generation++,
        .particle_address = particle_address,
        .x = { left, right, left, right },
        .y = { top, top, bottom, bottom },
        .u = {
            clamp_uv(table[4]), clamp_uv((int32_t)table[6] - 1),
            clamp_uv(table[8]), clamp_uv((int32_t)table[10] - 1),
        },
        .v = {
            clamp_uv((int32_t)table[5] + 0x40),
            clamp_uv((int32_t)table[7] + 0x40),
            clamp_uv((int32_t)table[9] + 0x3f),
            clamp_uv((int32_t)table[11] + 0x3f),
        },
        .tpage = (uint16_t)(0x1fu + abr * 0x20u),
        .clut_x = 0x100u,
        .clut_y = 0xf7u,
        .command = 0x2eu,
        .payload_word_count = 9u,
        .semi_transparent = true,
        .valid = true,
    };
    ++particle_source_count;
    return true;
}

bool xg_field_particles_source_matches_memory(
    CPUState *cpu, const XgRenderParticleSource *source) {
    uint32_t index;

    if (cpu == NULL || source == NULL || cpu->read_half == NULL ||
        !source->valid || source->generation == 0u)
        return false;
    for (index = 0u; index < 4u; ++index) {
        const uint32_t vertex = source->particle_address + 0xa0u + index * 8u;

        if ((int16_t)cpu->read_half(vertex) != source->x[index] ||
            (int16_t)cpu->read_half(vertex + 2u) != source->y[index] ||
            cpu->read_half(vertex + 4u) != 0u)
            return false;
    }
    return true;
}

static int32_t particle_position_component(uint32_t value) {
    const int32_t signed_value = (int32_t)value;

    if (signed_value >= 0) return signed_value / 4096;
    return -(int32_t)(((uint64_t)(-(int64_t)signed_value) + 4095u) / 4096u);
}

bool xg_field_particles_build_matrix(
    CPUState *cpu, uint32_t particle_address, uint32_t source_matrix_address,
    uint32_t extra_scale_address, uint32_t composition_selector,
    XgHost3dMatrix *matrix) {
    XgHost3dMatrix source_matrix;
    XgHost3dMatrix local = { 0 };
    XgHost3dMatrix composed;
    XgHost3dLongVector particle_scale;
    XgHost3dLongVector extra_scale;
    uint32_t sin_cos;

    if (cpu == NULL || matrix == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || composition_selector >= 4u ||
        particle_address > UINT32_MAX - 0x3cu ||
        extra_scale_address > UINT32_MAX - 8u ||
        !xg_render_runtime_word_address_is_valid(particle_address) ||
        !xg_render_runtime_word_address_is_valid(particle_address + 0x3cu) ||
        !xg_render_runtime_word_address_is_valid(extra_scale_address) ||
        !xg_render_runtime_word_address_is_valid(extra_scale_address + 8u) ||
        !xg_render_runtime_capture_matrix(cpu, source_matrix_address,
                                           &source_matrix))
        return false;
    sin_cos = cpu->read_word(UINT32_C(0x800523f0) +
        ((cpu->gpr[6] & 0xfffu) * 4u));
    local.rotation[0][0] = xg_render_runtime_low_s16(sin_cos >> 16u);
    local.rotation[0][1] = (int16_t)-xg_render_runtime_low_s16(sin_cos);
    local.rotation[1][0] = xg_render_runtime_low_s16(sin_cos);
    local.rotation[1][1] = xg_render_runtime_low_s16(sin_cos >> 16u);
    local.rotation[2][2] = 4096;
    local.translation[0] = particle_position_component(
        cpu->read_word(particle_address + 8u));
    local.translation[1] = particle_position_component(
        cpu->read_word(particle_address + 12u));
    local.translation[2] = particle_position_component(
        cpu->read_word(particle_address + 16u));
    particle_scale = (XgHost3dLongVector){
        (int32_t)(int16_t)cpu->read_half(particle_address + 0x38u),
        (int32_t)(int16_t)cpu->read_half(particle_address + 0x3au),
        (int32_t)(int16_t)cpu->read_half(particle_address + 0x3cu),
    };
    extra_scale = (XgHost3dLongVector){
        (int32_t)cpu->read_word(extra_scale_address),
        (int32_t)cpu->read_word(extra_scale_address + 4u),
        (int32_t)cpu->read_word(extra_scale_address + 8u),
    };
    if (composition_selector == 3u &&
        !xg_host_3d_scale_matrix(&source_matrix, &extra_scale))
        return false;
    if (!xg_host_3d_comp_matrix(&source_matrix, &local, &composed))
        return false;
    memcpy(composed.rotation, local.rotation, sizeof(composed.rotation));
    if (!xg_host_3d_scale_matrix(&composed, &particle_scale)) return false;
    *matrix = composed;
    return true;
}
