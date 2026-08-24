#ifndef XG_RENDER_PRIMITIVE_UTILS_H
#define XG_RENDER_PRIMITIVE_UTILS_H

#include "gpu.h"
#include "gpu_render.h"
#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

void xg_render_primitive_apply_projected_quad_positions(
    XgRenderIrNativePrimitive *primitive,
    const XgHost3dProjectedVertex projected[4]);
void xg_render_semantic_set_interpolation_identity(
    GpuRenderSemantic *semantic, uint64_t scene_id,
    uint32_t producer_id, uint32_t primitive_id);
void xg_render_semantic_set_corner_identities(
    GpuRenderSemantic *semantic, uint32_t producer_id, uint32_t primitive_id);
bool xg_render_primitive_all_projective(
    const XgRenderIrNativePrimitive *primitive);
void xg_render_material_apply_draw_state(
    XgRenderIrMaterialState *material, const GpuDrawState *draw);
uint32_t xg_render_ir_fixed_uv(const XgRenderIrVertex *vertex);
uint32_t xg_render_projected_xy(const XgHost3dProjectedVertex *vertex);
bool xg_render_primitive_translate_cached(
    const XgRenderIrNativePrimitive *primitive,
    GpuRenderSemantic *cached_semantic, bool *semantic_ready,
    GpuRenderSemantic *out_semantic);

#endif
