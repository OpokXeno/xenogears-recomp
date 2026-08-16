#ifndef XG_RENDER_QUAD_BUILDER_H
#define XG_RENDER_QUAD_BUILDER_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_RENDER_QUAD_VERTEX_COUNT = 4,
};

typedef enum XgRenderQuadBuilderResult {
    XG_RENDER_QUAD_BUILDER_OK = 0,
    XG_RENDER_QUAD_BUILDER_INVALID_ARGUMENT = 1,
    XG_RENDER_QUAD_BUILDER_INVALID_SOURCE = 2,
} XgRenderQuadBuilderResult;

typedef struct XgRenderQuadSourceVertex {
    int16_t x;
    int16_t y;
    uint8_t u;
    uint8_t v;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    int32_t native_view_x_16_16;
    int32_t native_view_y_16_16;
    bool native_view_position;
    int32_t projective_view_x;
    int32_t projective_view_y;
    int32_t projective_view_z;
    int32_t projective_offset_x_16_16;
    int32_t projective_offset_y_16_16;
    int32_t projective_native_offset_x_16_16;
    int32_t projective_native_offset_y_16_16;
    uint16_t projective_distance;
    bool projective_position;
} XgRenderQuadSourceVertex;

typedef struct XgRenderQuadSource {
    XgRenderQuadSourceVertex vertices[XG_RENDER_QUAD_VERTEX_COUNT];
    XgRenderIrMaterialState material;
} XgRenderQuadSource;

XgRenderQuadBuilderResult xg_render_quad_build_primitive(
    const XgRenderQuadSource *source,
    XgRenderIrNativePrimitive *out_primitive);

void xg_render_quad_set_projected_position(
    XgRenderQuadSourceVertex *out_vertex,
    const XgHost3dProjectedVertex *projected);

#ifdef __cplusplus
}
#endif

#endif
