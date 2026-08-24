#include "xg_world_decorations.h"

#include "xg_render_quad_builder.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

_Static_assert(sizeof(XgWorldDecorationsPosition) == 8u,
               "world decoration positions must match the helper stride");

static int32_t wrap_i32(uint32_t value) {
    if (value <= INT32_MAX)
        return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int16_t wrap_i16(uint32_t value) {
    const uint16_t low = (uint16_t)value;

    if (low <= INT16_MAX)
        return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static int32_t wrap_coordinate(int32_t value, int32_t wrap) {
    const int32_t span = wrap_i32((uint32_t)wrap << 11u);

    if (value < -0x4000)
        value = wrap_i32((uint32_t)value + (uint32_t)span);
    if (value >= 0x4000)
        value = wrap_i32((uint32_t)value - (uint32_t)span);
    return value;
}

static bool fixed_vertices_are_valid(
    const XgHost3dVector vertices[XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT]) {
    static const XgHost3dVector expected[4] = {
        {-24, -72, 0, 0u},
        {24, -72, 0, 0u},
        {-24, 0, 0, 0u},
        {24, 0, 0, 0u},
    };

    return memcmp(vertices, expected, sizeof(expected)) == 0;
}

static bool
fixed_uv_is_valid(const uint8_t uv[XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT][2]) {
    static const uint8_t expected[4][2] = {
        {0u, 64u},
        {31u, 64u},
        {0u, 111u},
        {31u, 111u},
    };

    return memcmp(uv, expected, sizeof(expected)) == 0;
}

static bool depth_clut_is_valid(
    const uint16_t clut[XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT]) {
    uint32_t index;

    for (index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT; ++index) {
        const uint16_t expected =
            (uint16_t)(((0x1f0u + index) << 6u) | (0xf0u >> 4u));

        if (clut[index] != expected)
            return false;
    }
    return true;
}

static bool material_is_valid(const XgRenderIrMaterialState *material) {
    return material->tpage == 0x001eu && material->texture_page_x == 14u &&
           material->texture_page_y == 1u && material->clut_x == 240u &&
           material->clut_y == 511u &&
           material->draw_area_left <= material->draw_area_right &&
           material->draw_area_top <= material->draw_area_bottom &&
           material->draw_area_right <= 1023u &&
           material->draw_area_bottom <= 1023u &&
           material->draw_offset_x >= -1024 &&
           material->draw_offset_x <= 1023 &&
           material->draw_offset_y >= -1024 &&
           material->draw_offset_y <= 1023 &&
           material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT &&
           material->texture_window_mask_x == 0u &&
           material->texture_window_mask_y == 0u &&
           material->texture_window_offset_x == 0u &&
           material->texture_window_offset_y == 0u &&
           material->shading == XG_RENDER_IR_SHADING_FLAT &&
           material->textured && !material->raw_texture &&
           !material->semi_transparent &&
           material->blend_mode == XG_RENDER_IR_BLEND_AVERAGE;
}

static uint32_t packed_xy(const XgHost3dProjectedVertex *vertex) {
    return (uint16_t)vertex->x | ((uint32_t)(uint16_t)vertex->y << 16u);
}

static uint32_t packed_uv(uint8_t u, uint8_t v, uint16_t attribute) {
    return (uint32_t)u | ((uint32_t)v << 8u) | ((uint32_t)attribute << 16u);
}

static void build_ft4_payload(const XgWorldDecorationsSource *source,
                              XgWorldDecorationsRecord *record) {
    record->ft4_payload_words[0] = UINT32_C(0x2c808080);
    record->ft4_payload_words[1] = packed_xy(&record->ft4_vertices[0]);
    record->ft4_payload_words[2] =
        packed_uv(source->uv[0][0], source->uv[0][1], record->clut);
    record->ft4_payload_words[3] = packed_xy(&record->ft4_vertices[1]);
    record->ft4_payload_words[4] =
        packed_uv(source->uv[1][0], source->uv[1][1], source->material.tpage);
    record->ft4_payload_words[5] = packed_xy(&record->ft4_vertices[2]);
    record->ft4_payload_words[6] =
        packed_uv(source->uv[2][0], source->uv[2][1], 0u);
    record->ft4_payload_words[7] = packed_xy(&record->ft4_vertices[3]);
    record->ft4_payload_words[8] =
        packed_uv(source->uv[3][0], source->uv[3][1], 0u);
}

XgWorldDecorationsResult xg_world_decorations_build_with_temporal(
    const XgWorldDecorationsSource *source, XgWorldDecorationsRecord *records,
    uint32_t record_capacity, uint32_t *in_out_packet_count,
    XgWorldDecorationsRecord *rejected_records, uint32_t rejected_capacity,
    uint32_t *in_out_rejected_count) {
    uint32_t source_index;
    uint32_t packet_count;
    uint32_t rejected_count = in_out_rejected_count != NULL
        ? *in_out_rejected_count : 0u;

    if (source == NULL || records == NULL || in_out_packet_count == NULL ||
        (rejected_records == NULL) != (in_out_rejected_count == NULL))
        return XG_WORLD_DECORATIONS_INVALID_ARGUMENT;
    packet_count = *in_out_packet_count;
    if (record_capacity < XG_WORLD_DECORATIONS_PACKET_CAPACITY)
        return XG_WORLD_DECORATIONS_CAPACITY_EXCEEDED;
    if (rejected_count > rejected_capacity)
        return XG_WORLD_DECORATIONS_CAPACITY_EXCEEDED;
    if (packet_count > XG_WORLD_DECORATIONS_PACKET_CAPACITY ||
        source->position_count > XG_WORLD_DECORATIONS_POSITION_CAPACITY ||
        source->screen_x_cull_margin < 0 ||
        source->screen_x_cull_margin > (INT32_MAX - 320) / 2 ||
        source->projection_distance == 0u ||
        !fixed_vertices_are_valid(source->vertices) ||
        !fixed_uv_is_valid(source->uv) ||
        !depth_clut_is_valid(source->depth_clut) ||
        !material_is_valid(&source->material))
        return XG_WORLD_DECORATIONS_INVALID_SOURCE;

    for (source_index = 0u; source_index < source->position_count;
         ++source_index) {
        const XgWorldDecorationsPosition *position =
            &source->positions[source_index];
        XgHost3dMatrix camera_rotation = source->camera_matrix;
        XgHost3dMatrix transform = source->decoration_matrix;
        XgHost3dLongVector relative;
        XgHost3dLongVector transformed;
        XgHost3dProject4Input input = {0};
        XgHost3dRotTransPers4Output output;
        XgRenderQuadSource quad = {0};
        XgWorldDecorationsRecord candidate = {0};
        uint32_t flags;
        uint32_t vertex;
        uint32_t clut_index;
        bool x_visible = false;
        bool y_visible = false;
        int32_t relative_x;
        int32_t relative_z;

        if (packet_count >= XG_WORLD_DECORATIONS_PACKET_CAPACITY)
            break;

        relative_x =
            wrap_coordinate(wrap_i32((uint32_t)(int32_t)position->x -
                                     (uint32_t)source->camera_origin_x),
                            source->wrap_x);
        relative_z =
            wrap_coordinate(wrap_i32((uint32_t)(int32_t)position->z -
                                     (uint32_t)source->camera_origin_z),
                            source->wrap_z);
        relative = (XgHost3dLongVector){
            wrap_i16((uint32_t)relative_x),
            position->y,
            wrap_i16(0u - (uint32_t)relative_z),
        };
        memset(camera_rotation.translation, 0,
               sizeof(camera_rotation.translation));
        if (!xg_host_3d_rt(&camera_rotation, &relative, &transformed, &flags))
            return XG_WORLD_DECORATIONS_BUILD_FAILED;

        transform.translation[0] =
            wrap_i32((uint32_t)transformed.x +
                     (uint32_t)source->camera_matrix.translation[0]);
        transform.translation[1] =
            wrap_i32((uint32_t)transformed.y +
                     (uint32_t)source->camera_matrix.translation[1]);
        transform.translation[2] =
            wrap_i32((uint32_t)transformed.z +
                     (uint32_t)source->camera_matrix.translation[2]);
        memcpy(input.vertices, source->vertices, sizeof(input.vertices));
        memcpy(input.projection.rotation, transform.rotation,
               sizeof(transform.rotation));
        memcpy(input.projection.translation, transform.translation,
               sizeof(transform.translation));
        input.projection.screen_offset_x = source->screen_offset_x;
        input.projection.screen_offset_y = source->screen_offset_y;
        input.projection.projection_distance = source->projection_distance;
        input.projection.depth_cue_a = source->depth_cue_a;
        input.projection.depth_cue_b = source->depth_cue_b;
        if (!xg_host_3d_rot_trans_pers4(&input, &output))
            return XG_WORLD_DECORATIONS_BUILD_FAILED;

        /* The helper applies unsigned screen tests to the three RTPT results;
         * the fourth RTPS result is emitted but does not participate. */
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t widened_x =
                ((uint32_t)((int32_t)output.vertices[vertex].x +
                            source->screen_x_cull_margin)) & 0xffffu;

            x_visible |= widened_x <
                320u + 2u * (uint32_t)source->screen_x_cull_margin;
            y_visible |= (uint32_t)(int32_t)output.vertices[vertex].y < 216u;
        }
        candidate.rtpt_flags = output.rtpt_flags;
        candidate.rtps_flags = output.rtps_flags;
        candidate.depth_cue = output.depth_cue;
        candidate.third_depth = output.vertices[2].z;
        candidate.ordering_bucket = output.vertices[2].z >> 4u;
        candidate.source_index = source_index;
        candidate.semantic_id = source_index;
        candidate.packet_index = packet_count;
        candidate.tag_payload_word_count =
            XG_WORLD_DECORATIONS_FT4_PAYLOAD_WORD_COUNT;
        clut_index = (uint32_t)(uint16_t)output.depth_cue;
        if (clut_index > 0x0fffu)
            clut_index = 0x0fffu;
        candidate.clut = source->depth_clut[clut_index >> 8u];
        memcpy(candidate.ft4_vertices, output.vertices,
               sizeof(candidate.ft4_vertices));

        quad.material = source->material;
        quad.material.clut_x = (candidate.clut & 0x3fu) << 4u;
        quad.material.clut_y = candidate.clut >> 6u;
        for (vertex = 0u; vertex < XG_WORLD_DECORATIONS_FT4_VERTEX_COUNT;
             ++vertex) {
            quad.vertices[vertex] = (XgRenderQuadSourceVertex){
                .u = source->uv[vertex][0],
                .v = source->uv[vertex][1],
                .red = 0x80u,
                .green = 0x80u,
                .blue = 0x80u,
            };
            xg_render_quad_set_projected_position(
                &quad.vertices[vertex], &output.vertices[vertex]);
        }
        if (xg_render_quad_build_primitive(&quad, &candidate.primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
            return XG_WORLD_DECORATIONS_BUILD_FAILED;
        if ((int32_t)output.rtpt_flags < 0)
            candidate.cull = XG_WORLD_DECORATIONS_CULL_PROJECTIVE;
        else if (!x_visible || !y_visible)
            candidate.cull = XG_WORLD_DECORATIONS_CULL_SCREEN;
        else if (output.vertices[2].z >= 0x0e00u)
            candidate.cull = XG_WORLD_DECORATIONS_CULL_DEPTH;
        else
            candidate.accepted = true;
        if (!candidate.accepted) {
            if (rejected_records != NULL) {
                if (rejected_count >= rejected_capacity)
                    return XG_WORLD_DECORATIONS_CAPACITY_EXCEEDED;
                rejected_records[rejected_count++] = candidate;
                *in_out_rejected_count = rejected_count;
            }
            continue;
        }
        build_ft4_payload(source, &candidate);
        records[packet_count] = candidate;
        ++packet_count;
        *in_out_packet_count = packet_count;
    }
    return XG_WORLD_DECORATIONS_OK;
}

XgWorldDecorationsResult xg_world_decorations_build(
    const XgWorldDecorationsSource *source, XgWorldDecorationsRecord *records,
    uint32_t record_capacity, uint32_t *in_out_packet_count) {
    return xg_world_decorations_build_with_temporal(
        source, records, record_capacity, in_out_packet_count,
        NULL, 0u, NULL);
}
