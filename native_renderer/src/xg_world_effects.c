#include "xg_world_effects.h"

#include "xg_render_quad_builder.h"

#include <limits.h>
#include <string.h>

static int64_t shift_right_floor(int64_t value, unsigned bits) {
    uint64_t magnitude;

    if (value >= 0) return value >> bits;
    magnitude = (uint64_t)(-(value + 1)) + 1u;
    return -(int64_t)((magnitude + (((uint64_t)1u << bits) - 1u)) >> bits);
}

static int32_t wrap_i32(int64_t value) {
    const uint32_t low = (uint32_t)(uint64_t)value;

    if (low <= INT32_MAX) return (int32_t)low;
    return -1 - (int32_t)(UINT32_MAX - low);
}

static int16_t wrap_i16(int32_t value) {
    const uint16_t low = (uint16_t)(uint32_t)value;

    if (low <= INT16_MAX) return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static void rotate_matrix_z(XgHost3dMatrix *matrix, int16_t sine,
                            int16_t cosine) {
    int16_t row0[3];
    int16_t row1[3];
    uint32_t column;

    memcpy(row0, matrix->rotation[0], sizeof(row0));
    memcpy(row1, matrix->rotation[1], sizeof(row1));
    for (column = 0u; column < 3u; ++column) {
        matrix->rotation[0][column] = wrap_i16((int32_t)shift_right_floor(
            (int64_t)cosine * row0[column] -
                (int64_t)sine * row1[column],
            12u));
        matrix->rotation[1][column] = wrap_i16((int32_t)shift_right_floor(
            (int64_t)sine * row0[column] +
                (int64_t)cosine * row1[column],
            12u));
    }
}

static int32_t wrap_world_coordinate(int32_t value, int32_t wrap) {
    if (value < -0x4000)
        return wrap_i32((uint32_t)value + ((uint32_t)wrap << 11u));
    if (value > 0x4000)
        return wrap_i32((uint32_t)value - ((uint32_t)wrap << 11u));
    return value;
}

static bool material_template_is_valid(
    const XgRenderIrMaterialState *material) {
    return material->clut_x == 256u && material->clut_y == 511u &&
           material->shading == XG_RENDER_IR_SHADING_FLAT &&
           material->textured && !material->raw_texture &&
           material->semi_transparent;
}

XgWorldEffectsResult xg_world_effects_build(
    const XgWorldEffectsSource *source,
    XgWorldEffectsRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count) {
    uint32_t source_index;
    uint32_t record_count = 0u;

    if (source == NULL || records == NULL || out_record_count == NULL)
        return XG_WORLD_EFFECTS_INVALID_ARGUMENT;
    *out_record_count = 0u;
    memset(records, 0, sizeof(*records) * record_capacity);
    if (record_capacity < XG_WORLD_EFFECTS_SOURCE_CAPACITY ||
        source->screen_x_cull_margin < 0 ||
        source->screen_x_cull_margin > INT32_MAX - 320 ||
        source->projection_distance == 0u ||
        !material_template_is_valid(&source->material))
        return XG_WORLD_EFFECTS_INVALID_SOURCE;

    for (source_index = 0u; source_index < XG_WORLD_EFFECTS_SOURCE_CAPACITY;
         ++source_index) {
        const XgWorldEffectsParticleSource *particle =
            &source->particles[source_index];
        XgHost3dMatrix camera_rotation;
        XgHost3dMatrix transform;
        XgHost3dLongVector position;
        XgHost3dLongVector transformed;
        XgHost3dLongVector scale;
        XgHost3dProject4Input projection = { 0 };
        XgHost3dRotTransPers4Output projected;
        XgRenderQuadSource quad = { 0 };
        XgWorldEffectsRecord candidate = { 0 };
        uint32_t flags;
        uint32_t vertex;
        int32_t relative_x;
        int32_t relative_z;
        bool x_visible = false;
        bool y_visible = false;

        if (!particle->active) continue;
        if (particle->type == 0u ||
            particle->type >= XG_WORLD_EFFECTS_TYPE_COUNT ||
            particle->tpage > 0x1ffu ||
            ((particle->tpage >> 7u) & 3u) == 3u)
            return XG_WORLD_EFFECTS_INVALID_SOURCE;

        relative_x = wrap_world_coordinate(
            wrap_i32(shift_right_floor(particle->position[0], 12u) -
                     source->camera_origin_x),
            source->wrap_x);
        relative_z = wrap_world_coordinate(
            wrap_i32(shift_right_floor(particle->position[2], 12u) -
                     source->camera_origin_z),
            source->wrap_z);
        position = (XgHost3dLongVector){
            wrap_i16(relative_x),
            wrap_i16((int32_t)shift_right_floor(particle->position[1], 12u)),
            wrap_i16(wrap_i32(-(uint32_t)relative_z)),
        };
        camera_rotation = source->camera;
        memset(camera_rotation.translation, 0,
               sizeof(camera_rotation.translation));
        if (!xg_host_3d_rt(&camera_rotation, &position, &transformed, &flags))
            return XG_WORLD_EFFECTS_BUILD_FAILED;

        transform = source->billboard;
        if (particle->rotate)
            rotate_matrix_z(&transform, particle->sine, particle->cosine);
        scale = (XgHost3dLongVector){
            particle->scale_x, particle->scale_y, 0x1000,
        };
        if (!xg_host_3d_scale_matrix(&transform, &scale))
            return XG_WORLD_EFFECTS_BUILD_FAILED;
        transform.translation[0] = wrap_i32(
            (uint32_t)transformed.x + (uint32_t)source->camera.translation[0]);
        transform.translation[1] = wrap_i32(
            (uint32_t)transformed.y + (uint32_t)source->camera.translation[1]);
        transform.translation[2] = wrap_i32(
            (uint32_t)transformed.z + (uint32_t)source->camera.translation[2]);

        memcpy(projection.vertices, source->vertices[particle->type],
               sizeof(projection.vertices));
        memcpy(projection.projection.rotation, transform.rotation,
               sizeof(transform.rotation));
        memcpy(projection.projection.translation, transform.translation,
               sizeof(transform.translation));
        projection.projection.screen_offset_x = source->screen_offset_x;
        projection.projection.screen_offset_y = source->screen_offset_y;
        projection.projection.projection_distance =
            source->projection_distance;
        if (!xg_host_3d_rot_trans_pers4(&projection, &projected))
            return XG_WORLD_EFFECTS_BUILD_FAILED;
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            if ((int32_t)projected.vertices[vertex].x <
                320 + source->screen_x_cull_margin)
                x_visible = true;
            if (projected.vertices[vertex].y < 216) y_visible = true;
        }
        if ((int32_t)projected.projection_flags < 0 || !x_visible ||
            !y_visible || projected.vertices[3].z >= 0xc00u)
            continue;
        if (record_count >= record_capacity)
            return XG_WORLD_EFFECTS_CAPACITY_EXCEEDED;

        quad.material = source->material;
        quad.material.tpage = particle->tpage;
        quad.material.texture_page_x = particle->tpage & 0x0fu;
        quad.material.texture_page_y = (particle->tpage >> 4u) & 1u;
        quad.material.blend_mode =
            (XgRenderIrBlendMode)((particle->tpage >> 5u) & 3u);
        quad.material.texture_depth =
            (XgRenderIrTextureDepth)((particle->tpage >> 7u) & 3u);
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            const uint16_t uv = source->uv[particle->type][vertex];

            quad.vertices[vertex] = (XgRenderQuadSourceVertex){
                .u = (uint8_t)uv,
                .v = (uint8_t)(uv >> 8u),
                .red = particle->red,
                .green = particle->green,
                .blue = particle->blue,
            };
            xg_render_quad_set_projected_position(
                &quad.vertices[vertex], &projected.vertices[vertex]);
            quad.vertices[vertex].projective_position = false;
            candidate.uv[vertex] = uv;
        }
        if (xg_render_quad_build_primitive(&quad, &candidate.primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
            return XG_WORLD_EFFECTS_BUILD_FAILED;
        memcpy(candidate.vertices, projected.vertices,
               sizeof(candidate.vertices));
        candidate.projection_flags = projected.projection_flags;
        candidate.fourth_depth = projected.vertices[3].z;
        candidate.ordering_bucket = projected.vertices[3].z >> 4u;
        candidate.tpage = particle->tpage;
        candidate.clut = 0x7fd0u;
        candidate.material_word = (uint32_t)particle->red |
            ((uint32_t)particle->green << 8u) |
            ((uint32_t)particle->blue << 16u) | UINT32_C(0x2e000000);
        candidate.source_index = source_index;
        records[record_count++] = candidate;
    }

    *out_record_count = record_count;
    return XG_WORLD_EFFECTS_OK;
}
