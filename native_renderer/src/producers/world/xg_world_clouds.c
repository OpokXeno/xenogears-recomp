#include "xg_world_clouds.h"

#include "xg_render_quad_builder.h"

#include <limits.h>
#include <string.h>

enum {
    CLOUD_PROJECTION_FLAG_MASK = 0x7f85e000u,
    CLOUD_MIDDLE_DEPTH = 0x401u,
    CLOUD_FAR_DEPTH = 0x581u,
    CLOUD_FAR_PREINSERT_LIMIT = 0xd00u,
    CLOUD_FAR_POSTINSERT_LIMIT = 0xb00u,
};

typedef enum QuadResult {
    QUAD_RESULT_CULLED = 0,
    QUAD_RESULT_EMITTED,
    QUAD_RESULT_FAR_PREINSERT_STOP,
    QUAD_RESULT_FAR_POSTINSERT_STOP,
    QUAD_RESULT_FAILED,
} QuadResult;

typedef struct BuildContext {
    const XgWorldCloudsSource *source;
    XgWorldCloudRecord *records;
    XgWorldCloudRecord *rejected_records;
    XgWorldCloudsBuildStats *stats;
    uint32_t bucket_head[XG_WORLD_CLOUD_OT_BUCKET_COUNT];
    uint32_t record_count;
    uint32_t rejected_count;
    uint32_t rejected_capacity;
} BuildContext;

static int64_t shift_right_floor(int64_t value, unsigned bits) {
    uint64_t magnitude;

    if (value >= 0) return value / ((int64_t)1 << bits);
    magnitude = (uint64_t)(-(value + 1)) + 1u;
    return -(int64_t)((magnitude + (((uint64_t)1u << bits) - 1u)) >> bits);
}

static int32_t wrap_i32(uint64_t value) {
    const uint32_t low = (uint32_t)value;

    if (low <= INT32_MAX) return (int32_t)low;
    return -1 - (int32_t)(UINT32_MAX - low);
}

static int16_t wrap_i16(int32_t value) {
    const uint16_t low = (uint16_t)(uint32_t)value;

    if (low <= INT16_MAX) return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static int32_t add_velocity(int32_t position, int16_t velocity) {
    int32_t value = wrap_i32((uint32_t)position +
                             (uint32_t)(int32_t)velocity);

    if (value > 0x1ffffff)
        value = wrap_i32((uint32_t)value - UINT32_C(0x02000000));
    if (value < 0)
        value = wrap_i32((uint32_t)value + UINT32_C(0x02000000));
    return value;
}

XgWorldCloudsResult xg_world_clouds_step_positions(
    XgWorldCloudPosition positions[XG_WORLD_CLOUD_COUNT],
    const XgWorldCloudVelocity velocities[XG_WORLD_CLOUD_COUNT]) {
    uint32_t index;

    if (positions == NULL || velocities == NULL)
        return XG_WORLD_CLOUDS_INVALID_ARGUMENT;
    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        positions[index].x = add_velocity(positions[index].x,
                                          velocities[index].x);
        positions[index].z = add_velocity(positions[index].z,
                                          velocities[index].z);
    }
    return XG_WORLD_CLOUDS_OK;
}

static XgHost3dVector expected_far_vertex(uint32_t index) {
    const uint32_t layer = index / XG_HOST_3D_VERTEX_COUNT;
    const uint32_t vertex = index % XG_HOST_3D_VERTEX_COUNT;
    XgHost3dVector value = { 0 };

    value.x = (vertex & 1u) != 0u ? 192 : -192;
    value.y = (int16_t)(-(int32_t)layer * 8);
    value.z = vertex >= 2u ? -192 : 192;
    return value;
}

static XgHost3dVector expected_middle_vertex(uint32_t index) {
    const uint32_t layer = index / 16u;
    const uint32_t quad = (index % 16u) / XG_HOST_3D_VERTEX_COUNT;
    const uint32_t vertex = index % XG_HOST_3D_VERTEX_COUNT;
    const int32_t column = (int32_t)(quad & 1u);
    const int32_t row = (int32_t)(quad >> 1u);
    XgHost3dVector value = { 0 };

    value.x = (int16_t)(-192 + column * 192 +
                        (int32_t)(vertex & 1u) * 192);
    value.y = (int16_t)(-(int32_t)layer * 8);
    value.z = (int16_t)(192 - row * 192 -
                        (vertex >= 2u ? 192 : 0));
    return value;
}

static bool material_is_valid(const XgRenderIrMaterialState *material) {
    return material->tpage == 0x003fu && material->texture_page_x == 15u &&
           material->texture_page_y == 1u && material->clut_x == 304u &&
           material->clut_y == 510u &&
           material->draw_area_left <= material->draw_area_right &&
           material->draw_area_top <= material->draw_area_bottom &&
           material->draw_area_right <= 1023u &&
           material->draw_area_bottom <= 1023u &&
           material->draw_offset_x >= -1024 &&
           material->draw_offset_x <= 1023 &&
           material->draw_offset_y >= -1024 &&
           material->draw_offset_y <= 1023 &&
           material->texture_window_mask_x <= 31u &&
           material->texture_window_mask_y <= 31u &&
           material->texture_window_offset_x <= 31u &&
           material->texture_window_offset_y <= 31u &&
           material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT &&
           material->shading == XG_RENDER_IR_SHADING_FLAT &&
           material->textured && !material->raw_texture &&
           material->semi_transparent &&
           material->blend_mode == XG_RENDER_IR_BLEND_ADD;
}

static bool source_is_valid(const XgWorldCloudsSource *source) {
    static const uint16_t expected_uv[XG_WORLD_CLOUD_UV_BASE_COUNT] = {
        0x0000u, 0x0040u, 0x0080u, 0x0000u,
        0x00c0u, 0x4000u, 0x4040u, 0x0000u,
    };
    uint32_t index;

    if (source->projection_distance == 0u ||
        source->screen_x_cull_margin < 0 ||
        source->screen_x_cull_margin > (INT32_MAX - 320) / 2 ||
        !material_is_valid(&source->material) ||
        memcmp(source->uv_base, expected_uv, sizeof(expected_uv)) != 0)
        return false;
    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        if (source->velocities[index].uv_group > 1u) return false;
    }
    for (index = 0u;
         index < XG_WORLD_CLOUD_FAR_QUAD_COUNT * XG_HOST_3D_VERTEX_COUNT;
         ++index) {
        const XgHost3dVector expected = expected_far_vertex(index);
        const XgHost3dVector *actual =
            &source->far_vertices[index / XG_HOST_3D_VERTEX_COUNT]
                                 [index % XG_HOST_3D_VERTEX_COUNT];

        if (memcmp(actual, &expected, sizeof(*actual)) != 0) return false;
    }
    for (index = 0u;
         index < XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT * XG_HOST_3D_VERTEX_COUNT;
         ++index) {
        const XgHost3dVector expected = expected_middle_vertex(index);
        const XgHost3dVector *actual =
            &source->middle_vertices[index / XG_HOST_3D_VERTEX_COUNT]
                                    [index % XG_HOST_3D_VERTEX_COUNT];

        if (memcmp(actual, &expected, sizeof(*actual)) != 0) return false;
    }
    return true;
}

static int16_t rotated_component(int64_t value) {
    return wrap_i16((int32_t)shift_right_floor(value, 12u));
}

static void rotate_x(XgHost3dMatrix *matrix,
                     const XgWorldCloudTrigValue *trig) {
    int16_t row1[3];
    int16_t row2[3];
    uint32_t column;

    memcpy(row1, matrix->rotation[1], sizeof(row1));
    memcpy(row2, matrix->rotation[2], sizeof(row2));
    for (column = 0u; column < 3u; ++column) {
        matrix->rotation[1][column] = rotated_component(
            (int64_t)trig->cosine * row1[column] -
            (int64_t)trig->sine * row2[column]);
        matrix->rotation[2][column] = rotated_component(
            (int64_t)trig->sine * row1[column] +
            (int64_t)trig->cosine * row2[column]);
    }
}

static void rotate_y(XgHost3dMatrix *matrix,
                     const XgWorldCloudTrigValue *trig) {
    int16_t row0[3];
    int16_t row2[3];
    uint32_t column;

    memcpy(row0, matrix->rotation[0], sizeof(row0));
    memcpy(row2, matrix->rotation[2], sizeof(row2));
    for (column = 0u; column < 3u; ++column) {
        matrix->rotation[0][column] = rotated_component(
            (int64_t)trig->cosine * row0[column] +
            (int64_t)trig->sine * row2[column]);
        matrix->rotation[2][column] = rotated_component(
            -(int64_t)trig->sine * row0[column] +
            (int64_t)trig->cosine * row2[column]);
    }
}

static bool multiply_rotation(const XgHost3dMatrix *left,
                              const XgHost3dMatrix *right,
                              XgHost3dMatrix *output) {
    XgHost3dMatrix result = { 0 };
    uint32_t column;

    for (column = 0u; column < 3u; ++column) {
        const XgHost3dVector input = {
            right->rotation[0][column], right->rotation[1][column],
            right->rotation[2][column], 0u,
        };
        XgHost3dVector transformed;
        uint32_t flags;

        if (!xg_host_3d_rtir(left, &input, &transformed, &flags)) return false;
        result.rotation[0][column] = transformed.x;
        result.rotation[1][column] = transformed.y;
        result.rotation[2][column] = transformed.z;
    }
    *output = result;
    return true;
}

static bool prepare_rotation(const XgWorldCloudsSource *source,
                             XgHost3dMatrix *output) {
    XgHost3dMatrix pitch = source->billboard;
    XgHost3dMatrix negative_yaw = source->billboard;
    XgHost3dMatrix positive_yaw = source->billboard;
    XgHost3dMatrix intermediate;
    XgHost3dMatrix world;

    rotate_x(&pitch, &source->pitch_rotation);
    rotate_y(&negative_yaw, &source->negative_yaw_rotation);
    rotate_y(&positive_yaw, &source->positive_yaw_rotation);
    return multiply_rotation(&pitch, &negative_yaw, &intermediate) &&
           multiply_rotation(&positive_yaw, &intermediate, &world) &&
           multiply_rotation(&source->camera, &world, output);
}

static int32_t wrapped_relative(int32_t position, int32_t origin) {
    int32_t relative = (int32_t)shift_right_floor(
        wrap_i32((uint32_t)position - (uint32_t)origin), 12u);

    if (relative < -0x1000)
        relative += 0x2000;
    else if (relative > 0xfff)
        relative -= 0x2000;
    return relative;
}

static int32_t normal_clip(int16_t x0, int16_t y0, int16_t x1,
                           int16_t y1, int16_t x2, int16_t y2) {
    const int64_t value =
        (int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 -
        (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1;

    return wrap_i32((uint64_t)value);
}

static bool world_wedge_accepts(const XgWorldCloudsSource *source,
                                int32_t relative_x,
                                int32_t negative_relative_z) {
    const int16_t x = wrap_i16(relative_x - source->wedge_offset_x);
    const int16_t y = wrap_i16(negative_relative_z - source->wedge_offset_z);

    if (normal_clip(x, y, source->wedge_right.sine,
                    source->wedge_right.cosine, 0, 0) > 0)
        return false;
    return normal_clip(0, 0, source->wedge_left.sine,
                       source->wedge_left.cosine, x, y) <= 0;
}

static bool screen_accepts(
    const XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT],
    int32_t x_margin) {
    const uint32_t x_limit = 320u + 2u * (uint32_t)x_margin;
    bool x_visible = false;
    bool y_visible = false;
    uint32_t index;

    for (index = 0u; index < XG_HOST_3D_VERTEX_COUNT; ++index) {
        const uint32_t x =
            ((uint32_t)((int32_t)vertices[index].x + x_margin)) & 0xffffu;

        if (x < x_limit) x_visible = true;
        if ((uint16_t)vertices[index].y < 216u) y_visible = true;
    }
    return x_visible && y_visible;
}

static uint32_t ordering_depth(
    const XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT]) {
    uint32_t maximum =
        vertices[0].z < vertices[1].z ? vertices[1].z : vertices[0].z;

    /* The guest branches directly to the store when SZ2 wins, skipping the
     * SZ3 comparison on that path. Preserve that ordering rather than taking
     * the mathematical maximum. */
    if (maximum < vertices[2].z) return vertices[2].z;
    if (maximum < vertices[3].z) return vertices[3].z;
    return maximum;
}

static QuadResult build_quad(BuildContext *context,
                             const XgHost3dProjection *projection,
                             const XgHost3dVector vertices[4],
                              uint16_t uv_base, uint8_t uv_mask,
                              uint32_t source_index, XgWorldCloudLod lod,
                              uint32_t lod_quad_index) {
    XgHost3dProject4Input input = { 0 };
    XgHost3dRotTransPers4Output projected;
    XgRenderQuadSource quad = { 0 };
    XgWorldCloudRecord candidate = { 0 };
    uint32_t depth;
    uint32_t bucket;
    uint32_t index;
    const uint32_t flag_mask = lod == XG_WORLD_CLOUD_LOD_NEAR
        ? CLOUD_PROJECTION_FLAG_MASK : UINT32_C(0x80000000);

    ++context->stats->quad_attempt_count;
    memcpy(input.vertices, vertices, sizeof(input.vertices));
    input.projection = *projection;
    if (!xg_host_3d_rot_trans_pers4(&input, &projected))
        return QUAD_RESULT_FAILED;
    depth = ordering_depth(projected.vertices);
    bucket = depth >> 4u;

    candidate.uv[0] = uv_base;
    candidate.uv[1] = (uint16_t)(uv_base | uv_mask);
    candidate.uv[2] = (uint16_t)(uv_base | ((uint16_t)uv_mask << 8u));
    candidate.uv[3] = (uint16_t)(uv_base | uv_mask |
                                 ((uint16_t)uv_mask << 8u));
    quad.material = context->source->material;
    for (index = 0u; index < XG_HOST_3D_VERTEX_COUNT; ++index) {
        quad.vertices[index] = (XgRenderQuadSourceVertex){
            .u = (uint8_t)candidate.uv[index],
            .v = (uint8_t)(candidate.uv[index] >> 8u),
            .red = 0x26u,
            .green = 0x26u,
            .blue = 0x26u,
        };
        xg_render_quad_set_projected_position(
            &quad.vertices[index], &projected.vertices[index]);
    }
    if (xg_render_quad_build_primitive(&quad, &candidate.primitive) !=
        XG_RENDER_QUAD_BUILDER_OK)
        return QUAD_RESULT_FAILED;

    memcpy(candidate.vertices, projected.vertices,
           sizeof(candidate.vertices));
    candidate.projection_flags = projected.projection_flags;
    candidate.ordering_depth = depth;
    candidate.ordering_bucket = bucket;
    candidate.emission_index = context->record_count;
    candidate.prior_emission_in_bucket =
        bucket < XG_WORLD_CLOUD_OT_BUCKET_COUNT
            ? context->bucket_head[bucket] : UINT32_MAX;
    candidate.source_index = source_index;
    candidate.lod_quad_index = lod_quad_index;
    candidate.material_word = UINT32_C(0x2e262626);
    candidate.tpage = 0x003fu;
    candidate.clut = 0x7f93u;
    candidate.lod = lod;
    if ((projected.projection_flags & flag_mask) != 0u) {
        ++context->stats->quad_projection_culled;
        candidate.cull = XG_WORLD_CLOUD_CULL_PROJECTIVE;
    } else if (!screen_accepts(projected.vertices,
                               context->source->screen_x_cull_margin)) {
        ++context->stats->quad_screen_culled;
        candidate.cull = XG_WORLD_CLOUD_CULL_SCREEN;
    } else if (lod == XG_WORLD_CLOUD_LOD_FAR &&
               depth > CLOUD_FAR_PREINSERT_LIMIT) {
        ++context->stats->far_preinsert_depth_stops;
        candidate.cull = XG_WORLD_CLOUD_CULL_DEPTH;
    } else {
        candidate.accepted = true;
    }
    if (!candidate.accepted) {
        if (context->rejected_records != NULL) {
            if (context->rejected_count >= context->rejected_capacity)
                return QUAD_RESULT_FAILED;
            context->rejected_records[context->rejected_count++] = candidate;
        }
        return candidate.cull == XG_WORLD_CLOUD_CULL_DEPTH
            ? QUAD_RESULT_FAR_PREINSERT_STOP : QUAD_RESULT_CULLED;
    }
    if (bucket >= XG_WORLD_CLOUD_OT_BUCKET_COUNT)
        return QUAD_RESULT_FAILED;
    context->records[context->record_count] = candidate;
    context->bucket_head[bucket] = context->record_count;
    ++context->record_count;

    if (lod == XG_WORLD_CLOUD_LOD_FAR &&
        depth > CLOUD_FAR_POSTINSERT_LIMIT) {
        ++context->stats->far_postinsert_depth_stops;
        return QUAD_RESULT_FAR_POSTINSERT_STOP;
    }
    return QUAD_RESULT_EMITTED;
}

static bool flags_accept(uint32_t flags) {
    return (flags & CLOUD_PROJECTION_FLAG_MASK) == 0u;
}

XgWorldCloudsResult xg_world_clouds_build_with_temporal(
    const XgWorldCloudsSource *source,
    XgWorldCloudRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count,
    XgWorldCloudsBuildStats *out_stats,
    XgWorldCloudRecord *rejected_records,
    uint32_t rejected_capacity,
    uint32_t *out_rejected_count) {
    XgWorldCloudsBuildStats stats = { 0 };
    XgWorldCloudPosition positions[XG_WORLD_CLOUD_COUNT];
    XgHost3dMatrix cloud_rotation;
    BuildContext context = { 0 };
    uint32_t source_index;

    if (source == NULL || records == NULL || out_record_count == NULL ||
        out_stats == NULL ||
        (rejected_records == NULL) != (out_rejected_count == NULL))
        return XG_WORLD_CLOUDS_INVALID_ARGUMENT;
    *out_record_count = 0u;
    if (out_rejected_count != NULL) *out_rejected_count = 0u;
    memset(out_stats, 0, sizeof(*out_stats));
    if (record_capacity < XG_WORLD_CLOUD_PACKET_CAPACITY)
        return XG_WORLD_CLOUDS_CAPACITY_EXCEEDED;
    memset(records, 0,
           sizeof(*records) * XG_WORLD_CLOUD_PACKET_CAPACITY);
    if (rejected_records != NULL)
        memset(rejected_records, 0,
               sizeof(*rejected_records) * rejected_capacity);
    if (!source_is_valid(source)) return XG_WORLD_CLOUDS_INVALID_SOURCE;

    memcpy(positions, source->positions, sizeof(positions));
    if (xg_world_clouds_step_positions(positions, source->velocities) !=
            XG_WORLD_CLOUDS_OK ||
        !prepare_rotation(source, &cloud_rotation))
        return XG_WORLD_CLOUDS_BUILD_FAILED;

    context.source = source;
    context.records = records;
    context.rejected_records = rejected_records;
    context.rejected_capacity = rejected_capacity;
    context.stats = &stats;
    for (source_index = 0u;
         source_index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++source_index)
        context.bucket_head[source_index] = UINT32_MAX;

    for (source_index = 0u; source_index < XG_WORLD_CLOUD_COUNT;
         ++source_index) {
        XgHost3dProjection projection = { 0 };
        XgHost3dLongVector transformed;
        XgHost3dLongVector center;
        XgHost3dProjectedVertex anchor;
        uint32_t anchor_flags;
        uint32_t anchor_depth;
        uint32_t matrix_flags;
        uint32_t base_uv_index;
        int32_t relative_x;
        int32_t relative_z;
        int32_t negative_relative_z;

        if (context.record_count > XG_WORLD_CLOUD_PACKET_ENTRY_LIMIT) {
            stats.packet_entry_limit_stopped = true;
            break;
        }
        ++stats.clouds_entered;
        relative_x = wrapped_relative(positions[source_index].x,
                                      source->camera_origin_x);
        relative_z = wrapped_relative(positions[source_index].z,
                                      source->camera_origin_z);
        negative_relative_z = wrap_i32(-(uint32_t)relative_z);
        if (!world_wedge_accepts(source, relative_x, negative_relative_z)) {
            ++stats.clouds_world_culled;
            continue;
        }
        if (!stats.has_world_accepted_source) {
            stats.has_world_accepted_source = true;
            stats.first_world_accepted_source = source_index;
            stats.first_world_relative_x = relative_x;
            stats.first_world_relative_z = relative_z;
        }

        center = (XgHost3dLongVector){
            relative_x,
            (int32_t)shift_right_floor(positions[source_index].y, 12u),
            negative_relative_z,
        };
        if (!xg_host_3d_rt(&source->camera, &center, &transformed,
                           &matrix_flags))
            return XG_WORLD_CLOUDS_BUILD_FAILED;
        memcpy(projection.rotation, cloud_rotation.rotation,
               sizeof(projection.rotation));
        projection.translation[0] = transformed.x;
        projection.translation[1] = transformed.y;
        projection.translation[2] = transformed.z;
        projection.screen_offset_x = source->screen_offset_x;
        projection.screen_offset_y = source->screen_offset_y;
        projection.projection_distance = source->projection_distance;
        projection.depth_cue_a = source->depth_cue_a;
        projection.depth_cue_b = source->depth_cue_b;
        if (!xg_host_3d_rtps(&projection,
                             &(XgHost3dVector){ 0, 0, 0, 0u },
                             &anchor, &anchor_flags))
            return XG_WORLD_CLOUDS_BUILD_FAILED;
        if (source_index == stats.first_world_accepted_source) {
            stats.first_anchor_flags = anchor_flags;
            stats.first_anchor_translation_x = transformed.x;
            stats.first_anchor_translation_y = transformed.y;
            stats.first_anchor_translation_z = transformed.z;
        }
        if (!flags_accept(anchor_flags)) {
            ++stats.clouds_anchor_culled;
            continue;
        }

        anchor_depth = anchor.z;
        base_uv_index = source->velocities[source_index].uv_group * 4u;
        if (anchor_depth < CLOUD_MIDDLE_DEPTH) {
            uint32_t layer;

            for (layer = 0u; layer < 3u; ++layer) {
                XgHost3dVector group_vertices[4];
                XgHost3dProjectedVertex ignored;
                uint32_t group_flags = 0u;
                uint32_t flags;
                uint32_t row;
                const XgHost3dVector base = source->far_vertices[0][0];
                const int16_t y = wrap_i16((int32_t)base.y -
                                           (int32_t)layer * 8);

                group_vertices[0] = (XgHost3dVector){ base.x, y, base.z, 0u };
                group_vertices[1] = (XgHost3dVector){
                    wrap_i16((int32_t)base.x + 0x180), y, base.z, 0u };
                group_vertices[2] = (XgHost3dVector){
                    base.x, y, wrap_i16((int32_t)base.z - 0x180), 0u };
                group_vertices[3] = (XgHost3dVector){
                    wrap_i16((int32_t)base.x + 0x180), y,
                    wrap_i16((int32_t)base.z - 0x180), 0u };
                if (!xg_host_3d_rtps(&projection, &group_vertices[0],
                                     &ignored, &group_flags))
                    return XG_WORLD_CLOUDS_BUILD_FAILED;
                for (row = 1u; row < 4u; ++row) {
                    if (!xg_host_3d_rtps(&projection, &group_vertices[row],
                                         &ignored, &flags))
                        return XG_WORLD_CLOUDS_BUILD_FAILED;
                    group_flags |= flags;
                }
                if (!flags_accept(group_flags)) {
                    ++stats.near_groups_culled;
                    continue;
                }

                for (row = 0u; row < 4u; ++row) {
                    uint32_t column;

                    for (column = 0u; column < 4u; ++column) {
                        XgHost3dVector quad_vertices[4];
                        const int32_t x = (int32_t)base.x +
                                          (int32_t)column * 0x60;
                        const int32_t z = (int32_t)base.z -
                                          (int32_t)row * 0x60;
                        const uint16_t uv = (uint16_t)(
                            source->uv_base[base_uv_index + layer] +
                            row * 0x1000u + column * 0x10u);
                        QuadResult quad_result;

                        quad_vertices[0] = (XgHost3dVector){
                            wrap_i16(x), y, wrap_i16(z), 0u };
                        quad_vertices[1] = (XgHost3dVector){
                            wrap_i16(x + 0x60), y, wrap_i16(z), 0u };
                        quad_vertices[2] = (XgHost3dVector){
                            wrap_i16(x), y, wrap_i16(z - 0x60), 0u };
                        quad_vertices[3] = (XgHost3dVector){
                            wrap_i16(x + 0x60), y,
                            wrap_i16(z - 0x60), 0u };
                        quad_result = build_quad(
                            &context, &projection, quad_vertices, uv, 0x0fu,
                            source_index, XG_WORLD_CLOUD_LOD_NEAR,
                            layer * 16u + row * 4u + column);
                        if (quad_result == QUAD_RESULT_FAILED)
                            return XG_WORLD_CLOUDS_BUILD_FAILED;
                    }
                }
            }
        } else if (anchor_depth < CLOUD_FAR_DEPTH) {
            uint32_t layer;

            for (layer = 0u; layer < 3u; ++layer) {
                uint32_t quad;

                for (quad = 0u; quad < 4u; ++quad) {
                    const uint16_t uv = (uint16_t)(
                        source->uv_base[base_uv_index + layer] +
                        ((quad & 2u) << 12u) + ((quad & 1u) << 5u));
                    const QuadResult quad_result = build_quad(
                        &context, &projection,
                        source->middle_vertices[layer * 4u + quad],
                        uv, 0x1fu, source_index,
                        XG_WORLD_CLOUD_LOD_MIDDLE, layer * 4u + quad);

                    if (quad_result == QUAD_RESULT_FAILED)
                        return XG_WORLD_CLOUDS_BUILD_FAILED;
                }
            }
        } else {
            uint32_t quad;

            for (quad = 0u; quad < XG_WORLD_CLOUD_FAR_QUAD_COUNT; ++quad) {
                const QuadResult quad_result = build_quad(
                    &context, &projection, source->far_vertices[quad],
                    source->uv_base[base_uv_index + quad], 0x3fu,
                    source_index, XG_WORLD_CLOUD_LOD_FAR, quad);

                if (quad_result == QUAD_RESULT_FAILED)
                    return XG_WORLD_CLOUDS_BUILD_FAILED;
                if (quad_result == QUAD_RESULT_FAR_PREINSERT_STOP ||
                    quad_result == QUAD_RESULT_FAR_POSTINSERT_STOP)
                    break;
            }
        }
    }

    *out_record_count = context.record_count;
    if (out_rejected_count != NULL)
        *out_rejected_count = context.rejected_count;
    *out_stats = stats;
    return XG_WORLD_CLOUDS_OK;
}

XgWorldCloudsResult xg_world_clouds_build(
    const XgWorldCloudsSource *source,
    XgWorldCloudRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count,
    XgWorldCloudsBuildStats *out_stats) {
    return xg_world_clouds_build_with_temporal(
        source, records, record_capacity, out_record_count, out_stats,
        NULL, 0u, NULL);
}
