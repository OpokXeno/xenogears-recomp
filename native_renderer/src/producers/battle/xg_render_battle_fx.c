#include "xg_render_battle_fx.h"
#include "cpu_state.h"

#include <limits.h>
#include <stdlib.h>

static int32_t ripple_signed_word(uint32_t value) {
    return value <= INT32_MAX ? (int32_t)value
        : (int32_t)((int64_t)value - INT64_C(4294967296));
}

static int32_t ripple_shift(int64_t value, uint32_t bits) {
    const int64_t divisor = INT64_C(1) << bits;
    return (int32_t)(value / divisor - (value % divisor < 0));
}

XgRenderBattleFxResult xg_render_battle_fx_capture_ripple(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        const XgRenderBattleFxOwnerIdentity *owner,
        const XgRenderBattleRippleCaptureServices *services) {
    XgRenderIrMaterialState draw_state;
    XgHost3dProjection projection;
    XgRenderBattleRipplePrimitive *draws;
    uint32_t draw_count = 0u;
    uint32_t mesh, phase, amplitude, parity;
    int32_t gray, sine[3], cosine[3], intermediate;
    bool oscillating;
    XgRenderBattleFxResult result = XG_RENDER_BATTLE_FX_INVALID_ARGUMENT;

    if (cpu == NULL || owner == NULL || services == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL || cpu->read_byte == NULL ||
        services->authorize == NULL || services->source_range_valid == NULL ||
        services->capture_projection == NULL || services->capture_draw_state == NULL ||
        services->publish_captures == NULL ||
        (pc & UINT32_C(0x1fffffff)) != UINT32_C(0x1fc11c) ||
        instruction_word != UINT32_C(0x27bdff78) ||
        owner->kind != XG_RENDER_BATTLE_FX_RIPPLE_DISSOLVE ||
        owner->overlay_identity != XG_RENDER_BATTLE_FX_RIPPLE_OVERLAY_IDENTITY ||
        owner->authentication_receipt == 0u || owner->owner_generation == 0u ||
        owner->executable_identity == 0u ||
        !services->authorize(services->context, pc, instruction_word, owner))
        return result;
    if (cpu->gpr[4] == 0u || cpu->gpr[4] > UINT32_MAX - 8u ||
        !services->source_range_valid(services->context, cpu->gpr[4], 8u, 4u))
        return result;
    mesh = cpu->read_word(cpu->gpr[4] + 4u);
    if (mesh == 0u || mesh > UINT32_MAX - 0x10fa4u ||
        !services->source_range_valid(services->context, mesh + 0x38u, 0x2cu, 4u) ||
        !services->source_range_valid(services->context, UINT32_C(0x801fce14), 1u, 1u) ||
        !services->source_range_valid(services->context, UINT32_C(0x800ccb34), 4u, 4u) ||
        !services->source_range_valid(services->context, UINT32_C(0x800523f0), 0x4000u, 2u) ||
        !services->capture_projection(services->context, &projection) ||
        !services->capture_draw_state(services->context, &draw_state))
        return result;
    parity = cpu->read_word(UINT32_C(0x800ccb34));
    if (parity > 1u) return result;
    oscillating = cpu->read_byte(UINT32_C(0x801fce14)) != 0u;
    gray = ripple_signed_word(cpu->read_word(mesh + 0x38u));
    phase = cpu->read_word(mesh + 0x3cu);
    amplitude = cpu->read_word(mesh + 0x40u);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
        const uint32_t angle = cpu->read_half(mesh + 0x5cu + axis * 2u) & 0xfffu;
        sine[axis] = (int16_t)cpu->read_half(UINT32_C(0x800523f0) + angle * 4u);
        cosine[axis] = (int16_t)cpu->read_half(UINT32_C(0x800523f2) + angle * 4u);
        if (sine[axis] < -4096 || sine[axis] > 4096 ||
            cosine[axis] < -4096 || cosine[axis] > 4096)
            return result;
        projection.translation[axis] = ripple_signed_word(
            cpu->read_word(mesh + 0x4cu + axis * 4u));
    }
    /* Exact fixed-point RotMatrix at slus_006.64:8003f738, not host sin/cos. */
    projection.rotation[0][0] = (int16_t)ripple_shift(cosine[2] * cosine[1], 12u);
    intermediate = ripple_shift(cosine[2] * -sine[1], 12u);
    projection.rotation[1][0] = (int16_t)(ripple_shift(sine[2] * cosine[0], 12u) -
        ripple_shift(intermediate * sine[0], 12u));
    projection.rotation[2][0] = (int16_t)(ripple_shift(intermediate * cosine[0], 12u) +
        ripple_shift(sine[2] * sine[0], 12u));
    projection.rotation[0][1] = (int16_t)ripple_shift(-sine[2] * cosine[1], 12u);
    intermediate = ripple_shift(sine[2] * -sine[1], 12u);
    projection.rotation[1][1] = (int16_t)(ripple_shift(cosine[2] * cosine[0], 12u) +
        ripple_shift(intermediate * sine[0], 12u));
    projection.rotation[2][1] = (int16_t)(ripple_shift(cosine[2] * sine[0], 12u) -
        ripple_shift(intermediate * cosine[0], 12u));
    projection.rotation[0][2] = (int16_t)sine[1];
    projection.rotation[1][2] = (int16_t)ripple_shift(-cosine[1] * sine[0], 12u);
    projection.rotation[2][2] = (int16_t)ripple_shift(cosine[1] * cosine[0], 12u);
    projection.screen_offset_x = 160 * 65536;
    projection.screen_offset_y = 112 * 65536;
    projection.projection_distance = 512u;
    draws = calloc(XG_RENDER_BATTLE_FX_RIPPLE_TRIANGLES, sizeof(*draws));
    if (draws == NULL) return XG_RENDER_BATTLE_FX_RESOURCE_FAILED;
    for (uint32_t index = 0u; index < XG_RENDER_BATTLE_FX_RIPPLE_TRIANGLES; ++index) {
        const uint32_t record = mesh + 0x64u + index * 0x7cu;
        const uint32_t bank = index / 280u;
        const uint32_t column = index % 20u;
        const uint32_t row = (index % 280u) / 20u;
        const uint8_t u = (uint8_t)((column * 16u) & 63u);
        const uint8_t v = (uint8_t)(row * 16u);
        const uint8_t uv[2][3][2] = {
            {{u,v}, {(uint8_t)(u+16u),v}, {u,(uint8_t)(v+16u)}},
            {{(uint8_t)(u+16u),v}, {(uint8_t)(u+16u),(uint8_t)(v+16u)}, {u,(uint8_t)(v+16u)}}};
        XgHost3dProject4Input input = {.projection = projection};
        XgHost3dRotTransPers4Output projected;
        uint8_t colors[3];
        XgRenderBattleRipplePrimitive *draw = &draws[draw_count];
        /* Only SVECTOR inputs and radial phases are read. The first 0x54 bytes
         * contain double-buffered target packets and are deliberately skipped. */
        if (!services->source_range_valid(services->context, record + 0x54u, 0x24u, 2u))
            goto finished;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            uint32_t angle = cpu->read_word(record + 0x6cu + vertex * 4u);
            int32_t product, shade;
            int16_t wave;
            if (oscillating) angle += phase;
            wave = (int16_t)cpu->read_half(UINT32_C(0x800523f0) +
                (angle & 0xfffu) * 4u + (oscillating ? 0u : 2u));
            product = ripple_signed_word((uint32_t)(int32_t)wave * amplitude);
            if (product < 0) product += 0xfff;
            input.vertices[vertex] = (XgHost3dVector){
                .x = (int16_t)cpu->read_half(record + 0x54u + vertex * 8u),
                .y = (int16_t)cpu->read_half(record + 0x56u + vertex * 8u),
                .z = (int16_t)ripple_shift(product, 14u),
            };
            shade = ripple_signed_word((uint32_t)ripple_shift(product, 19u) + (uint32_t)gray);
            colors[vertex] = shade < 0 ? 0u : shade > 255 ? 255u : (uint8_t)shade;
        }
        input.vertices[3] = input.vertices[2];
        if (!xg_host_3d_rot_trans_pers4(&input, &projected)) goto finished;
        if ((projected.rtpt_flags & 0x8000u) != 0u) continue;
        draw->packet_address = record + 4u + parity * 0x28u;
        draw->ot_bucket = projected.vertices[2].z >> 8u;
        draw->source_primitive_index = index;
        draw->primitive.material = draw_state;
        draw->primitive.material.tpage = (uint16_t)(0x110u | ((704u + column * 16u) >> 6u));
        draw->primitive.material.texture_page_x = draw->primitive.material.tpage & 15u;
        draw->primitive.material.texture_page_y = 1u;
        draw->primitive.material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
        draw->primitive.material.clut_x = draw->primitive.material.clut_y = 0u;
        draw->primitive.material.shading = XG_RENDER_IR_SHADING_GOURAUD;
        draw->primitive.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
        draw->primitive.material.textured = true;
        draw->primitive.material.raw_texture = false;
        draw->primitive.material.semi_transparent = true;
        draw->primitive.triangle_count = 1u;
        draw->primitive.triangles[0].split_count = 1u;
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const XgHost3dProjectedVertex *p = &projected.vertices[vertex];
            draw->primitive.triangles[0].vertices[vertex] = (XgRenderIrVertex){
                .x = (int32_t)p->x * 65536, .y = (int32_t)p->y * 65536,
                .u = (int32_t)uv[bank][vertex][0] * 65536,
                .v = (int32_t)uv[bank][vertex][1] * 65536,
                .r = colors[vertex], .g = colors[vertex], .b = colors[vertex],
                .projective_view_x = p->projective_view_x,
                .projective_view_y = p->projective_view_y,
                .projective_view_z = p->projective_view_z,
                .projective_offset_x = p->projective_offset_x_16_16,
                .projective_offset_y = p->projective_offset_y_16_16,
                .projective_native_offset_x = p->projective_native_offset_x_16_16,
                .projective_native_offset_y = p->projective_native_offset_y_16_16,
                .projective_distance = p->projective_distance,
                .projective_position = p->projective_position != 0u,
                .native_view_x = p->native_view_x_16_16,
                .native_view_y = p->native_view_y_16_16,
                .native_view_position = p->native_view_position != 0u,
            };
        }
        ++draw_count;
    }
    result = services->publish_captures(services->context, owner, draws, draw_count)
        ? XG_RENDER_BATTLE_FX_OK : XG_RENDER_BATTLE_FX_EMIT_FAILED;
finished:
    free(draws);
    return result;
}
