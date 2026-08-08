#include "xg_render_quad_builder.h"

#include <stddef.h>
#include <string.h>

static int material_is_valid(const XgRenderIrMaterialState *material) {
    const uint16_t encoded_depth = (uint16_t)((material->tpage >> 7u) & 3u);

    return material->tpage <= UINT16_C(0x01ff) && encoded_depth != 3u &&
        material->texture_page_x == (material->tpage & UINT16_C(0x000f)) &&
        material->texture_page_y == ((material->tpage >> 4u) & 1u) &&
        material->texture_depth == (XgRenderIrTextureDepth)encoded_depth &&
        material->blend_mode ==
            (XgRenderIrBlendMode)((material->tpage >> 5u) & 3u) &&
        material->clut_x <= 1008u && (material->clut_x & 15u) == 0u &&
        material->clut_y <= 511u &&
        material->draw_area_left <= material->draw_area_right &&
        material->draw_area_top <= material->draw_area_bottom &&
        material->draw_area_right <= 1023u &&
        material->draw_area_bottom <= 1023u &&
        material->draw_offset_x >= -1024 && material->draw_offset_x <= 1023 &&
        material->draw_offset_y >= -1024 && material->draw_offset_y <= 1023 &&
        material->texture_window_mask_x <= 31u &&
        material->texture_window_mask_y <= 31u &&
        material->texture_window_offset_x <= 31u &&
        material->texture_window_offset_y <= 31u &&
        (material->shading == XG_RENDER_IR_SHADING_FLAT ||
         material->shading == XG_RENDER_IR_SHADING_GOURAUD) &&
        (!material->raw_texture || material->textured);
}

void xg_render_quad_set_projected_position(
    XgRenderQuadSourceVertex *out_vertex,
    const XgHost3dProjectedVertex *projected) {
    if (out_vertex == NULL || projected == NULL) return;
    out_vertex->x = projected->x;
    out_vertex->y = projected->y;
    out_vertex->native_view_x_16_16 = projected->native_view_x_16_16;
    out_vertex->native_view_y_16_16 = projected->native_view_y_16_16;
    out_vertex->native_view_position = projected->native_view_position != 0u;
}

XgRenderQuadBuilderResult xg_render_quad_build_primitive(
    const XgRenderQuadSource *source,
    XgRenderIrNativePrimitive *out_primitive) {
    static const uint8_t split[2][3] = { { 0u, 1u, 2u },
                                         { 2u, 1u, 3u } };
    size_t triangle;
    size_t vertex;

    if (source == NULL || out_primitive == NULL)
        return XG_RENDER_QUAD_BUILDER_INVALID_ARGUMENT;
    memset(out_primitive, 0, sizeof(*out_primitive));
    if (!material_is_valid(&source->material))
        return XG_RENDER_QUAD_BUILDER_INVALID_SOURCE;
    if (source->material.shading == XG_RENDER_IR_SHADING_FLAT) {
        for (vertex = 1u; vertex < XG_RENDER_QUAD_VERTEX_COUNT; ++vertex) {
            if (source->vertices[vertex].red != source->vertices[0].red ||
                source->vertices[vertex].green != source->vertices[0].green ||
                source->vertices[vertex].blue != source->vertices[0].blue)
                return XG_RENDER_QUAD_BUILDER_INVALID_SOURCE;
        }
    }
    out_primitive->material = source->material;
    out_primitive->triangle_count = 2u;
    for (triangle = 0u; triangle < 2u; ++triangle) {
        out_primitive->triangles[triangle].split_index = (uint8_t)triangle;
        out_primitive->triangles[triangle].split_count = 2u;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const XgRenderQuadSourceVertex *source_vertex =
                &source->vertices[split[triangle][vertex]];

            out_primitive->triangles[triangle].vertices[vertex] =
                (XgRenderIrVertex){
                    .x = (int32_t)source_vertex->x * INT32_C(65536),
                    .y = (int32_t)source_vertex->y * INT32_C(65536),
                    .u = (int32_t)source_vertex->u * INT32_C(65536),
                    .v = (int32_t)source_vertex->v * INT32_C(65536),
                    .r = source_vertex->red,
                    .g = source_vertex->green,
                    .b = source_vertex->blue,
                    .native_view_x = source_vertex->native_view_x_16_16,
                    .native_view_y = source_vertex->native_view_y_16_16,
                    .native_view_position =
                        source_vertex->native_view_position,
                };
        }
    }
    return XG_RENDER_QUAD_BUILDER_OK;
}
