#include "xg_sprite_ft4.h"

#include "xg_render_quad_builder.h"

#include <string.h>

XgSpriteFt4Result xg_sprite_ft4_select_ot_address(
    uint32_t base_address, uint32_t sprite_flags, uint32_t descriptor_flags,
    uint32_t *out_address) {
    const uint32_t bucket_offset = (descriptor_flags & 7u) * 4u;

    if (out_address == NULL) return XG_SPRITE_FT4_INVALID_ARGUMENT;
    if ((sprite_flags & UINT32_C(0x08000000)) != 0u) {
        if (base_address < bucket_offset)
            return XG_SPRITE_FT4_INVALID_SOURCE;
        base_address -= bucket_offset;
    }
    *out_address = base_address;
    return XG_SPRITE_FT4_OK;
}

XgSpriteFt4Result xg_sprite_ft4_map_uv(
    uint8_t base_u, uint8_t base_v, uint8_t width, uint8_t height,
    bool horizontal_reversed,
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2]) {
    uint8_t right_span = (uint8_t)(width - 1u);
    const uint8_t bottom_v = (uint8_t)(base_v + height - 1u);
    uint8_t right_u;

    if (uv == NULL) return XG_SPRITE_FT4_INVALID_ARGUMENT;
    if (horizontal_reversed) {
        if (base_u != 0u) {
            --base_u;
        } else {
            right_span = (uint8_t)(width - 2u);
        }
    }
    right_u = (uint8_t)(base_u + right_span);
    uv[0][0] = uv[2][0] = base_u;
    uv[1][0] = uv[3][0] = right_u;
    uv[0][1] = uv[1][1] = base_v;
    uv[2][1] = uv[3][1] = bottom_v;
    return XG_SPRITE_FT4_OK;
}

XgSpriteFt4Result xg_sprite_ft4_build(
    const XgSpriteFt4Source *source, XgSpriteFt4Record *record) {
    XgHost3dProject4Input input = { 0 };
    XgHost3dRotTransPers4Output output;
    XgRenderQuadSource quad = { 0 };
    uint8_t seen_vertices = 0u;
    uint32_t vertex;

    if (source == NULL || record == NULL)
        return XG_SPRITE_FT4_INVALID_ARGUMENT;
    memset(record, 0, sizeof(*record));
    if (source->material.shading != XG_RENDER_IR_SHADING_FLAT ||
        !source->material.textured ||
        source->material.texture_depth > XG_RENDER_IR_TEXTURE_15_BIT)
        return XG_SPRITE_FT4_INVALID_SOURCE;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        const uint32_t packet_vertex =
            source->packet_vertex_for_projection[vertex];

        if (packet_vertex >= XG_HOST_3D_VERTEX_COUNT ||
            (seen_vertices & (1u << packet_vertex)) != 0u)
            return XG_SPRITE_FT4_INVALID_SOURCE;
        seen_vertices |= (uint8_t)(1u << packet_vertex);
    }
    memcpy(input.vertices, source->vertices, sizeof(input.vertices));
    input.projection = source->projection;
    if (!xg_host_3d_rot_trans_pers4(&input, &output))
        return XG_SPRITE_FT4_BUILD_FAILED;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex)
        record->vertices[source->packet_vertex_for_projection[vertex]] =
            output.vertices[vertex];
    record->depth_cue = output.depth_cue;
    record->fourth_depth = output.fourth_depth;
    record->projection_flags = output.projection_flags;

    quad.material = source->material;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        quad.vertices[vertex] = (XgRenderQuadSourceVertex){
            .u = source->uv[vertex][0],
            .v = source->uv[vertex][1],
            .red = source->color[0],
            .green = source->color[1],
            .blue = source->color[2],
        };
        xg_render_quad_set_projected_position(
            &quad.vertices[vertex], &record->vertices[vertex]);
        quad.vertices[vertex].projective_position = false;
    }
    if (xg_render_quad_build_primitive(&quad, &record->primitive) !=
        XG_RENDER_QUAD_BUILDER_OK)
        return XG_SPRITE_FT4_BUILD_FAILED;
    return XG_SPRITE_FT4_OK;
}
