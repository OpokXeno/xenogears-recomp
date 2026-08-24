#include "xg_field_compass.h"

#include "cpu_state.h"
#include "gpu.h"
#include "psx_cyc.h"
#include "xg_field_character_adapter.h"
#include "xg_field_render_services.h"
#include "xg_host_3d.h"
#include "xg_render_primitive_utils.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

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

static bool fail_compass(
        const XgFieldCompassPipelineServices *services, uint32_t blocker) {
    (void)services;
    xg_field_compass_fail(blocker);
    return false;
}

void xg_field_compass_fail(uint32_t blocker) {
    xg_render_submission_pre_scene_block(blocker, false);
}

bool xg_field_compass_pre_scene_available(uint32_t primitive_count) {
    if (xg_render_submission_pre_scene_blocked()) return false;
    if (xg_render_submission_pre_scene_available(primitive_count)) return true;
    xg_field_compass_fail(10u);
    return false;
}

bool xg_field_compass_cutover(
        CPUState *cpu, uint32_t producer_pc, bool screen_aligned,
        const XgFieldCompassPipelineServices *services) {
    XgHost3dRotAverage4Input input = { 0 };
    XgHost3dRotAverage4Output output;
    XgHost3dProjectedVertex render_vertices[4];
    XgFieldCharacterCapture capture = { 0 };
    XgFieldCharacterCandidate candidate;
    XgRenderIrNativePrimitive primitive;
    XgRenderProducerLifecycle lifecycle;
    XgRenderModelFt4SourceRecord source_record;
    GpuDrawState draw = { 0 };
    uint32_t source_address;
    uint32_t matrix_address;
    uint32_t packet_address;
    uint32_t ot_address;
    uint32_t packet_tag;
    uint32_t previous_head;
    uint32_t word;
    uint32_t index;

    if (cpu == NULL || services == NULL ||
        services->pre_scene_available == NULL ||
        services->begin_lifecycle == NULL || services->stage == NULL ||
        services->publish == NULL || cpu->read_word == NULL ||
        cpu->write_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL)
        return fail_compass(services, 9u);
    if (!services->begin_lifecycle(producer_pc, &lifecycle))
        return fail_compass(services, 9u);
    if (!services->pre_scene_available(1u)) return false;
    source_address = cpu->gpr[5];
    matrix_address = cpu->gpr[6];
    if (cpu->gpr[7] > 1u || source_address > UINT32_MAX - 0x68u ||
        matrix_address > UINT32_MAX - 0x20u || cpu->gpr[4] > UINT32_MAX - 4u)
        return fail_compass(services, 2u);
    packet_address = source_address + cpu->gpr[7] * 0x28u + 0x20u;
    ot_address = cpu->gpr[4] + 4u;
    if (!xg_render_runtime_word_address_is_valid(packet_address) ||
        !xg_render_runtime_word_address_is_valid(ot_address))
        return fail_compass(services, 3u);

    for (index = 0u; index < 4u; ++index) {
        word = cpu->read_word(source_address + index * 8u);
        input.vertices[index].x = xg_render_runtime_low_s16(word);
        input.vertices[index].y = xg_render_runtime_low_s16(word >> 16u);
        word = cpu->read_word(source_address + index * 8u + 4u);
        input.vertices[index].z = xg_render_runtime_low_s16(word);
        input.vertices[index].pad = (uint16_t)(word >> 16u);
    }
    for (index = 0u; index < 4u; ++index) {
        int16_t *rotation = &input.projection.rotation[0][0];
        word = cpu->read_word(matrix_address + index * 4u);
        rotation[index * 2u] = xg_render_runtime_low_s16(word);
        rotation[index * 2u + 1u] =
            xg_render_runtime_low_s16(word >> 16u);
    }
    word = cpu->read_word(matrix_address + 16u);
    input.projection.rotation[2][2] = xg_render_runtime_low_s16(word);
    for (index = 0u; index < 3u; ++index)
        input.projection.translation[index] =
            (int32_t)cpu->read_word(matrix_address + 20u + index * 4u);
    input.projection.screen_offset_x = INT32_C(0x010a0000);
    input.projection.screen_offset_y = INT32_C(0x00a60000);
    input.projection.projection_distance = 0x80u;
    input.projection.average_z_scale4 = 0x100;
    if (!xg_host_3d_rot_average4(&input, &output) ||
        !xg_field_compass_capture_material(cpu, source_address, &capture))
        return fail_compass(services, 4u);
    memcpy(render_vertices, output.vertices, sizeof(render_vertices));
    for (index = 0u; index < 4u; ++index) {
        capture.vertices[index].x = output.vertices[index].x;
        capture.vertices[index].y = output.vertices[index].y;
    }
    if (screen_aligned) {
        const int32_t sum = (int32_t)output.vertices[2].x +
                            (int32_t)output.vertices[3].x;
        const int16_t center = (int16_t)(sum / 2);
        const int16_t top = output.vertices[3].y;
        capture.vertices[0].x = center - 8;
        capture.vertices[1].x = center + 8;
        capture.vertices[2].x = center - 8;
        capture.vertices[3].x = center + 8;
        capture.vertices[0].y = top - 10;
        capture.vertices[1].y = top - 10;
        capture.vertices[2].y = top;
        capture.vertices[3].y = top;
        for (index = 0u; index < 4u; ++index) {
            render_vertices[index].x = capture.vertices[index].x;
            render_vertices[index].y = capture.vertices[index].y;
        }
        if (output.vertices[0].native_view_position &&
            output.vertices[1].native_view_position &&
            output.vertices[2].native_view_position &&
            output.vertices[3].native_view_position) {
            const int64_t native_center_x =
                ((int64_t)output.vertices[2].native_view_x_16_16 +
                 output.vertices[3].native_view_x_16_16) / 2;
            const int64_t native_left_x =
                native_center_x - 8 * INT32_C(65536);
            const int64_t native_right_x =
                native_center_x + 8 * INT32_C(65536);
            const int64_t native_top_y =
                (int64_t)output.vertices[3].native_view_y_16_16 -
                10 * INT32_C(65536);
            if (native_left_x >= INT32_MIN && native_left_x <= INT32_MAX &&
                native_right_x >= INT32_MIN && native_right_x <= INT32_MAX &&
                native_top_y >= INT32_MIN && native_top_y <= INT32_MAX) {
                render_vertices[0].native_view_x_16_16 =
                    render_vertices[2].native_view_x_16_16 =
                        (int32_t)native_left_x;
                render_vertices[1].native_view_x_16_16 =
                    render_vertices[3].native_view_x_16_16 =
                        (int32_t)native_right_x;
                render_vertices[0].native_view_y_16_16 =
                    render_vertices[1].native_view_y_16_16 =
                        (int32_t)native_top_y;
                render_vertices[2].native_view_y_16_16 =
                    render_vertices[3].native_view_y_16_16 =
                        output.vertices[3].native_view_y_16_16;
            } else {
                for (index = 0u; index < 4u; ++index)
                    render_vertices[index].native_view_position = 0u;
            }
        } else {
            for (index = 0u; index < 4u; ++index)
                render_vertices[index].native_view_position = 0u;
        }
    }
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
        return fail_compass(services, 5u);
    xg_render_primitive_apply_projected_quad_positions(
        &primitive, render_vertices);

    packet_tag = cpu->read_word(packet_address);
    previous_head = cpu->read_word(ot_address);
    for (index = 0u; index < 4u; ++index) {
        const uint32_t xy = (uint16_t)capture.vertices[index].x |
            ((uint32_t)(uint16_t)capture.vertices[index].y << 16u);
        psx_store_cycle_barrier();
        cpu->write_word(packet_address + 8u + index * 8u, xy);
    }
    psx_store_cycle_barrier();
    cpu->write_word(packet_address,
        (packet_tag & UINT32_C(0xff000000)) |
        (previous_head & UINT32_C(0x00ffffff)));
    psx_store_cycle_barrier();
    cpu->write_word(ot_address,
        (previous_head & UINT32_C(0xff000000)) |
        (packet_address & UINT32_C(0x00ffffff)));
    if (!services->stage(&(XgRenderPreScenePrimitive){
            .primitive = primitive,
            .packet_address = packet_address,
            .source_primitive_index = UINT32_C(0x10000000) |
                (packet_address & UINT32_C(0x001ffffc)),
            .ot_bucket = 1u,
            .interpolation_producer_id = producer_pc,
            .interpolation_primitive_id =
                source_address & UINT32_C(0x1fffffff),
            .payload_word_count = 9u,
            .interpolation_identity_valid = true,
        }))
        return fail_compass(services, 10u);
    source_record = (XgRenderModelFt4SourceRecord){
        .primitive = primitive,
        .lifecycle = lifecycle,
        .source_id = (packet_address & UINT32_C(0x1fffffff)) + 4u,
        .interpolation_producer_id = producer_pc,
        .interpolation_primitive_id =
            source_address & UINT32_C(0x1fffffff),
        .opcode = (uint8_t)(cpu->read_word(packet_address + 4u) >> 24u),
        .interpolation_identity_valid = true,
        .valid = true,
    };
    if (!services->publish(&source_record, &(XgRenderModelSourcePublication){
            .resource_address = packet_address + 4u,
            .resource_size = 0x24u,
            .register_replay = true,
        }))
        return fail_compass(services, 10u);
    cpu->pc = cpu->gpr[31];
    return true;
}
