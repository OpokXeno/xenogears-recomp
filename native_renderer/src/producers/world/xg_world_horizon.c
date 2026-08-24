#include "xg_world_horizon.h"

#include "xg_render_quad_builder.h"

#include <string.h>

static bool material_is_valid(const XgRenderIrMaterialState *material) {
    return material->tpage == 0x003eu &&
           material->texture_page_x == 14u &&
           material->texture_page_y == 1u &&
           material->clut_x == 272u && material->clut_y == 510u &&
           material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT &&
           material->texture_window_mask_x == 16u &&
           material->texture_window_mask_y == 0u &&
           material->texture_window_offset_x == 0u &&
           material->texture_window_offset_y == 0u &&
           material->shading == XG_RENDER_IR_SHADING_FLAT &&
           material->textured && !material->raw_texture &&
           material->semi_transparent &&
           material->blend_mode == XG_RENDER_IR_BLEND_ADD;
}

XgWorldHorizonResult xg_world_horizon_build_for_view(
    const XgWorldHorizonSource *source, const XgNativeView *view,
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT]) {
    static const uint8_t split[2][3] = { { 0u, 1u, 2u },
                                         { 2u, 1u, 3u } };
    const uint8_t u0 = (uint8_t)((source != NULL ? source->angle : 0u) >> 2u) &
        0x7fu;
    const uint8_t u1 = u0 | 0x80u;
    const uint8_t u[4] = { u0, u1, u0, u1 };
    const uint8_t v[4] = { 0u, 0u, 0x3fu, 0x3fu };
    uint32_t quad_index;

    if (source == NULL || records == NULL)
        return XG_WORLD_HORIZON_INVALID_ARGUMENT;
    memset(records, 0, sizeof(*records) * XG_WORLD_HORIZON_QUAD_COUNT);
    if (source->ordering_shift > 31u ||
        source->projection.projection_distance == 0u ||
        !material_is_valid(&source->material))
        return XG_WORLD_HORIZON_INVALID_SOURCE;

    for (quad_index = 0u; quad_index < XG_WORLD_HORIZON_QUAD_COUNT;
         ++quad_index) {
        XgHost3dProject4Input projection = { 0 };
        XgHost3dRotTransPers4Output projected;
        XgHost3dRotTransPers4Output native_projected;
        XgRenderQuadSource quad = { 0 };
        bool native_view_valid = false;
        uint32_t vertex;

        memcpy(projection.vertices, source->vertices[quad_index],
               sizeof(projection.vertices));
        projection.projection = source->projection;
        if (!xg_host_3d_rot_trans_pers4(&projection, &projected))
            return XG_WORLD_HORIZON_BUILD_FAILED;
        if (view != NULL && view->enabled &&
            xg_native_view_projection(
                view, &source->projection, &projection.projection)) {
            native_view_valid = xg_host_3d_rot_trans_pers4(
                &projection, &native_projected);
            projection.projection = source->projection;
            if (!native_view_valid)
                return XG_WORLD_HORIZON_BUILD_FAILED;
        }
        records[quad_index].fourth_depth = projected.fourth_depth;
        records[quad_index].projection_flags = projected.projection_flags;
        memcpy(records[quad_index].vertices, projected.vertices,
               sizeof(records[quad_index].vertices));
        quad.material = source->material;
        for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
            quad.vertices[vertex] = (XgRenderQuadSourceVertex){
                .u = u[vertex],
                .v = v[vertex],
                .red = 0x30u,
                .green = 0x30u,
                .blue = 0x30u,
            };
            xg_render_quad_set_projected_position(
                &quad.vertices[vertex], &projected.vertices[vertex]);
        }
        if (xg_render_quad_build_primitive(
                &quad, &records[quad_index].primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
            return XG_WORLD_HORIZON_BUILD_FAILED;
        if (native_view_valid) {
            for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
                for (vertex = 0u; vertex < 3u; ++vertex) {
                    const XgHost3dProjectedVertex *wide_vertex =
                        &native_projected.vertices[split[triangle][vertex]];
                    XgRenderIrVertex *ir_vertex = &records[quad_index]
                        .primitive.triangles[triangle].vertices[vertex];

                    ir_vertex->native_view_x = wide_vertex->x_16_16;
                    ir_vertex->native_view_y = wide_vertex->y_16_16;
                    ir_vertex->native_view_position = true;
                }
            }
        }
    }

    for (quad_index = 0u; quad_index < XG_WORLD_HORIZON_QUAD_COUNT;
         ++quad_index) {
        records[quad_index].ordering_bucket =
            records[XG_WORLD_HORIZON_QUAD_COUNT - 1u].fourth_depth >>
            source->ordering_shift;
        records[quad_index].accepted =
            (int32_t)records[XG_WORLD_HORIZON_QUAD_COUNT - 1u]
                .projection_flags >= 0;
    }
    return XG_WORLD_HORIZON_OK;
}

XgWorldHorizonResult xg_world_horizon_build(
    const XgWorldHorizonSource *source,
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT]) {
    return xg_world_horizon_build_for_view(source, NULL, records);
}
