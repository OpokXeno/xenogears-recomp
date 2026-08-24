#include "xg_render_primitive_utils.h"

#include "xg_render_backend.h"
#include "xg_render_quad_builder.h"

#include <limits.h>
#include <stddef.h>

void xg_render_primitive_apply_projected_quad_positions(
        XgRenderIrNativePrimitive *primitive,
        const XgHost3dProjectedVertex projected[4]) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};

    /* Screen-space quads retain projected positions instead of being
     * reprojected as 3D geometry by the temporal path. */
    if (primitive == NULL || projected == NULL) return;
    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t source_index = split[triangle][vertex];
            XgRenderQuadSourceVertex source_vertex = {0};
            XgRenderIrVertex *target =
                &primitive->triangles[triangle].vertices[vertex];

            xg_render_quad_set_projected_position(
                &source_vertex, &projected[source_index]);
            target->x = (int32_t)source_vertex.x * INT32_C(65536);
            target->y = (int32_t)source_vertex.y * INT32_C(65536);
            target->native_view_x = source_vertex.native_view_x_16_16;
            target->native_view_y = source_vertex.native_view_y_16_16;
            target->native_view_position = source_vertex.native_view_position;
            target->projective_view_z = source_vertex.projective_view_z;
            target->temporal_depth = source_vertex.projective_view_z;
            target->temporal_depth_valid = true;
        }
    }
}

void xg_render_semantic_set_interpolation_identity(
        GpuRenderSemantic *semantic, uint64_t scene_id,
        uint32_t producer_id, uint32_t primitive_id) {
    if (semantic == NULL || producer_id == 0u) return;
    semantic->interpolation_identity = (GpuRenderInterpolationIdentity){
        scene_id, producer_id, primitive_id, 1u,
    };
}

void xg_render_semantic_set_corner_identities(
        GpuRenderSemantic *semantic, uint32_t producer_id,
        uint32_t primitive_id) {
    static const uint8_t quad_corners[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};

    if (semantic == NULL || semantic->topology != GPU_RENDER_SEMANTIC_TRIANGLES ||
        semantic->triangle_count == 0u || primitive_id > UINT32_MAX / 4u)
        return;
    for (uint32_t triangle = 0u; triangle < semantic->triangle_count;
         ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            GpuRenderSemanticVertex *target =
                &semantic->triangles[triangle].vertices[vertex];
            const uint32_t corner = semantic->triangle_count == 2u &&
                    triangle < 2u
                ? quad_corners[triangle][vertex] : vertex;

            if (target->interpolation_vertex_identity_valid) continue;
            target->interpolation_group_id = producer_id;
            target->interpolation_vertex_id = primitive_id * 4u + corner;
            target->interpolation_vertex_identity_valid = 1u;
        }
    }
}

bool xg_render_primitive_all_projective(
        const XgRenderIrNativePrimitive *primitive) {
    if (primitive == NULL || primitive->triangle_count == 0u) return false;
    for (uint32_t triangle = 0u; triangle < primitive->triangle_count;
         ++triangle)
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex)
            if (!primitive->triangles[triangle].vertices[vertex]
                    .projective_position)
                return false;
    return true;
}

void xg_render_material_apply_draw_state(
        XgRenderIrMaterialState *material, const GpuDrawState *draw) {
    if (material == NULL || draw == NULL) return;
    material->draw_area_left = draw->left;
    material->draw_area_top = draw->top;
    material->draw_area_right = draw->right;
    material->draw_area_bottom = draw->bottom;
    material->draw_offset_x = draw->offset_x;
    material->draw_offset_y = draw->offset_y;
    material->texture_window_mask_x = draw->texture_window_mask_x;
    material->texture_window_mask_y = draw->texture_window_mask_y;
    material->texture_window_offset_x = draw->texture_window_offset_x;
    material->texture_window_offset_y = draw->texture_window_offset_y;
    material->dither = draw->dither != 0u;
    material->mask_set = draw->mask_set != 0u;
    material->mask_check = draw->mask_check != 0u;
}

uint32_t xg_render_ir_fixed_uv(const XgRenderIrVertex *vertex) {
    return (uint32_t)vertex->u >> 16u |
        (((uint32_t)vertex->v >> 16u) << 8u);
}

uint32_t xg_render_projected_xy(const XgHost3dProjectedVertex *vertex) {
    return (uint16_t)vertex->x |
        ((uint32_t)(uint16_t)vertex->y << 16u);
}

bool xg_render_primitive_translate_cached(
        const XgRenderIrNativePrimitive *primitive,
        GpuRenderSemantic *cached_semantic, bool *semantic_ready,
        GpuRenderSemantic *out_semantic) {
    if (primitive == NULL || cached_semantic == NULL ||
        semantic_ready == NULL || out_semantic == NULL)
        return false;
    if (*semantic_ready) {
        *out_semantic = *cached_semantic;
        return true;
    }
    if (xg_render_backend_translate_primitive(primitive, cached_semantic) !=
            XG_RENDER_BACKEND_OK)
        return false;
    *semantic_ready = true;
    *out_semantic = *cached_semantic;
    return true;
}
