#include "xg_field_particles.h"

#include "cpu_state.h"
#include "gpu.h"
#include "psx_cyc.h"
#include "xg_field_character_adapter.h"
#include "xg_field_render_services.h"
#include "xg_render_primitive_utils.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static XgRenderParticleSourceState particle_sources = {
    .next_generation = 1u,
};
static uint32_t particle_source_count;
static uint64_t next_particle_generation = 1u;

enum {
    SHARED_TRIG_DATA_START = UINT32_C(0x800523f0),
    SHARED_TRIG_DATA_SIZE = 0x4000u,
};

bool xg_field_particles_shared_data_write_overlaps(
        uint32_t address, uint32_t size) {
    const uint64_t begin = address & UINT32_C(0x1fffffff);
    const uint64_t end = begin + size;
    const uint64_t range_begin =
        SHARED_TRIG_DATA_START & UINT32_C(0x1fffffff);

    return size != 0u && range_begin < end &&
        begin < range_begin + SHARED_TRIG_DATA_SIZE;
}

void xg_field_particles_register_code_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (set_range != NULL)
        set_range(SHARED_TRIG_DATA_START & UINT32_C(0x1fffffff),
                  SHARED_TRIG_DATA_SIZE);
}

static uint8_t clamp_uv(int32_t value) {
    if (value < 0) return 0u;
    if (value > 255) return 255u;
    return (uint8_t)value;
}

void xg_field_particles_reset(void) {
    particle_sources = (XgRenderParticleSourceState){
        .next_generation = next_particle_generation,
    };
    particle_source_count = 0u;
}

void xg_field_particles_classify_code_write(
        uint32_t address, uint32_t size,
        XgRenderMutationClassification *out_classification) {
    const bool overlaps =
        xg_field_particles_shared_data_write_overlaps(address, size);

    if (out_classification == NULL) return;
    *out_classification = (XgRenderMutationClassification){
        .properties = {
            .watched_range_mutation = overlaps,
            .shared_data_mutation = overlaps,
        },
        .code_write_mask = overlaps
            ? UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA : 0u,
    };
}

void xg_field_particles_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
        event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
        event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
        (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE &&
          (event->mutation.semantic_authority_loss ||
          xg_render_invalidation_has_code_class(
              event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA))))
        xg_field_particles_reset();
    if (event->kind == XG_RENDER_INVALIDATION_RESET)
        xg_field_particles_reset();
}

static XgRenderParticleSource *particle_source_find(
        uint32_t particle_address) {
    uint32_t index;

    particle_address = xg_render_runtime_guest_address(particle_address);
    for (index = 0u; index < XG_RENDER_PARTICLE_SOURCE_CAPACITY; ++index) {
        XgRenderParticleSource *record = &particle_sources.records[index];

        if (record->valid && record->particle_address == particle_address)
            return record;
    }
    return NULL;
}

bool xg_field_particles_lookup(
        uint32_t particle_address, XgRenderParticleSource *out_source) {
    const XgRenderParticleSource *source =
        particle_source_find(particle_address);

    if (source == NULL) return false;
    if (out_source != NULL) *out_source = *source;
    return true;
}

void xg_field_particles_invalidate(uint32_t particle_address) {
    XgRenderParticleSource *record =
        particle_source_find(particle_address);

    if (record != NULL) {
        *record = (XgRenderParticleSource){ 0 };
        --particle_source_count;
    }
}

bool xg_field_particles_observe_initializer(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    bool artifact_candidate_authorized, uint32_t producer_pc) {
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
        .producer_pc = producer_pc,
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
    next_particle_generation = particle_sources.next_generation;
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

static bool reject_particle(
        const XgFieldParticlePipelineServices *services, uint32_t blocker) {
    if (services != NULL && services->reject != NULL)
        services->reject(blocker);
    return false;
}

bool xg_field_particles_cutover(
        CPUState *cpu, uint32_t producer_pc,
        const XgFieldParticlePipelineServices *services) {
    const XgRenderParticleSource *source;
    XgHost3dMatrix matrix;
    XgHost3dRotAverage4Input input = { 0 };
    XgHost3dRotAverage4Output output;
    XgFieldCharacterCapture capture = { 0 };
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    GpuDrawState draw = { 0 };
    uint32_t particle_address;
    uint32_t stack_pointer;
    uint32_t extra_scale_address;
    uint32_t composition_selector;
    uint32_t buffer_index;
    uint32_t packet_address;
    uint32_t packet_tag;
    uint32_t shifted_depth;
    uint32_t ordering_shift;
    uint32_t bucket;
    uint32_t ot_base;
    uint32_t ot_address;
    uint32_t previous_head;
    uint32_t new_ot_word;
    uint32_t index;
    int64_t temporal_depth;

    if (cpu == NULL || services == NULL || services->stage_active == NULL ||
        services->stage_temporal == NULL || cpu->read_word == NULL ||
        cpu->write_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || cpu->write_byte == NULL ||
        !xg_render_runtime_word_address_is_valid(cpu->gpr[4]) ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]))
        return reject_particle(services, 41u);
    particle_address = xg_render_runtime_guest_address(cpu->gpr[4]);
    source = particle_source_find(particle_address);
    stack_pointer = cpu->gpr[29];
    extra_scale_address = cpu->read_word(stack_pointer + 0x10u);
    composition_selector = cpu->read_word(stack_pointer + 0x14u);
    if (particle_sources.blocked || source == NULL || cpu->gpr[7] >= 4u ||
        source->generation == 0u || source->generation > UINT32_MAX ||
        source->command != 0x2eu || source->payload_word_count != 9u ||
        !source->semi_transparent ||
        !xg_field_particles_source_matches_memory(cpu, source) ||
        !xg_field_particles_build_matrix(
            cpu, particle_address, cpu->gpr[5], extra_scale_address,
            composition_selector, &matrix))
        return reject_particle(services, 42u);
    for (index = 0u; index < 4u; ++index) {
        input.vertices[index] = (XgHost3dVector){
            source->x[index], source->y[index], 0, 0u,
        };
    }
    xg_render_runtime_capture_shadow_projection(cpu, &input.projection);
    memcpy(input.projection.rotation, matrix.rotation,
           sizeof(input.projection.rotation));
    memcpy(input.projection.translation, matrix.translation,
           sizeof(input.projection.translation));
    if (!xg_host_3d_rot_average4(&input, &output))
        return reject_particle(services, 44u);
    buffer_index = cpu->read_word(UINT32_C(0x800adb08));
    if (buffer_index > 1u || particle_address > UINT32_MAX - 0xa0u)
        return reject_particle(services, 43u);
    packet_address = particle_address + 0x50u + buffer_index * 0x28u;
    if (!xg_render_runtime_word_address_is_valid(packet_address) ||
        !xg_render_runtime_word_address_is_valid(packet_address + 0x24u))
        return reject_particle(services, 43u);
    packet_tag = cpu->read_word(packet_address);
    if ((packet_tag >> 24u) != source->payload_word_count)
        return reject_particle(services, 43u);
    for (index = 0u; index < 4u; ++index) {
        capture.vertices[index] = (XgFieldCharacterCaptureVertex){
            output.vertices[index].x, output.vertices[index].y,
            source->u[index], source->v[index],
        };
    }
    capture.red = cpu->read_byte(particle_address + 0x48u);
    capture.green = cpu->read_byte(particle_address + 0x49u);
    capture.blue = cpu->read_byte(particle_address + 0x4au);
    capture.tpage = source->tpage;
    capture.clut_x = source->clut_x;
    capture.clut_y = source->clut_y;
    capture.semi_transparent = source->semi_transparent;
    gpu_get_draw_state(&draw);
    capture.draw_area_left = draw.left;
    capture.draw_area_top = draw.top;
    capture.draw_area_right = draw.right;
    capture.draw_area_bottom = draw.bottom;
    capture.draw_offset_x = draw.offset_x;
    capture.draw_offset_y = draw.offset_y;
    capture.texture_window_mask_x = draw.texture_window_mask_x;
    capture.texture_window_mask_y = draw.texture_window_mask_y;
    capture.texture_window_offset_x = draw.texture_window_offset_x;
    capture.texture_window_offset_y = draw.texture_window_offset_y;
    capture.dither = draw.dither;
    capture.mask_set = draw.mask_set;
    capture.mask_check = draw.mask_check;
    if (xg_field_character_adapter_build(&capture, &candidate) !=
            XG_FIELD_CHARACTER_ADAPTER_OK ||
        xg_field_character_adapter_build_primitive(&candidate, &primitive) !=
            XG_FIELD_CHARACTER_ADAPTER_OK)
        return reject_particle(services, 45u);
    xg_render_primitive_apply_projected_quad_positions(
        &primitive, output.vertices);

    ordering_shift = cpu->read_word(UINT32_C(0x80050100)) & 31u;
    shifted_depth = (uint32_t)output.ordering_depth >> ordering_shift;
    switch (cpu->gpr[7]) {
    case 0u: bucket = 1u; break;
    case 1u: bucket = shifted_depth - 16u; break;
    case 2u: bucket = shifted_depth; break;
    case 3u: bucket = shifted_depth + 16u; break;
    default: return reject_particle(services, 42u);
    }
    temporal_depth = (int64_t)output.ordering_depth +
        (cpu->gpr[7] == 1u ? -((int64_t)16 << ordering_shift) :
         (cpu->gpr[7] == 3u ? ((int64_t)16 << ordering_shift) : 0));
    for (uint32_t triangle = 0u; triangle < primitive.triangle_count;
         ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            XgRenderIrVertex *target =
                &primitive.triangles[triangle].vertices[vertex];
            target->temporal_depth = temporal_depth < INT32_MIN
                ? INT32_MIN
                : (temporal_depth > INT32_MAX ? INT32_MAX
                                              : (int32_t)temporal_depth);
            target->temporal_depth_valid = true;
        }
    }
    if (bucket >= 1u && bucket <= 0xfffu) {
        ot_base = cpu->read_word(UINT32_C(0x800c426c));
        if (ot_base > UINT32_MAX - 0xccu - bucket * 4u)
            return reject_particle(services, 43u);
        ot_address = ot_base + 0xccu + bucket * 4u;
        if (!xg_render_runtime_word_address_is_valid(ot_address))
            return reject_particle(services, 46u);
        if (!services->stage_active(
                &primitive, packet_address,
                UINT32_C(0x20000000) |
                    (packet_address & UINT32_C(0x001ffffc)),
                bucket, XG_FIELD_CHARACTER_PACKET_WORD_COUNT,
                source->producer_pc, (uint32_t)source->generation, NULL))
            return reject_particle(services, 46u);
        previous_head = cpu->read_word(ot_address);
    } else {
        const int64_t minimum = INT64_C(1) << ordering_shift;
        const int64_t maximum = INT64_C(4096) << ordering_shift;
        const GpuRenderTemporalCullPolicy policy = {
            .flags = GPU_RENDER_TEMPORAL_CULL_DEPTH,
            .depth_min_inclusive = minimum > INT32_MAX
                ? INT32_MAX : (int32_t)minimum,
            .depth_max_exclusive = maximum > INT32_MAX
                ? INT32_MAX : (int32_t)maximum,
            .depth_mode = GPU_RENDER_TEMPORAL_DEPTH_MAXIMUM,
            .ordering_depth_shift = (uint8_t)ordering_shift,
        };
        if (!services->stage_temporal(
                &primitive, particle_address, 0u, &policy))
            return reject_particle(services, 46u);
        ot_address = 0u;
        previous_head = 0u;
    }

    psx_store_cycle_barrier();
    cpu->write_byte(packet_address + 4u, capture.red);
    psx_store_cycle_barrier();
    cpu->write_byte(packet_address + 5u, capture.green);
    psx_store_cycle_barrier();
    cpu->write_byte(packet_address + 6u, capture.blue);
    for (index = 0u; index < 4u; ++index) {
        const uint32_t xy = (uint16_t)capture.vertices[index].x |
            ((uint32_t)(uint16_t)capture.vertices[index].y << 16u);
        psx_store_cycle_barrier();
        cpu->write_word(packet_address + 8u + index * 8u, xy);
    }
    if (ot_address != 0u) {
        psx_store_cycle_barrier();
        cpu->write_word(packet_address,
            (packet_tag & UINT32_C(0xff000000)) |
            (previous_head & UINT32_C(0x00ffffff)));
        new_ot_word = (previous_head & UINT32_C(0xff000000)) |
            (packet_address & UINT32_C(0x00ffffff));
        psx_store_cycle_barrier();
        cpu->write_word(ot_address, new_ot_word);
        cpu->gpr[2] = new_ot_word;
    } else {
        cpu->gpr[2] = 0u;
    }
    cpu->pc = cpu->gpr[31];
    return true;
}
