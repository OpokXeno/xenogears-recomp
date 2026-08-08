#ifndef XG_SPRITE_FT4_H
#define XG_SPRITE_FT4_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgSpriteFt4Result {
    XG_SPRITE_FT4_OK = 0,
    XG_SPRITE_FT4_INVALID_ARGUMENT,
    XG_SPRITE_FT4_INVALID_SOURCE,
    XG_SPRITE_FT4_BUILD_FAILED,
} XgSpriteFt4Result;

typedef struct XgSpriteFt4Source {
    XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT];
    XgHost3dProjection projection;
    XgRenderIrMaterialState material;
    uint8_t color[3];
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2];
    uint8_t packet_vertex_for_projection[XG_HOST_3D_VERTEX_COUNT];
} XgSpriteFt4Source;

typedef struct XgSpriteFt4Record {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    int16_t depth_cue;
    uint16_t fourth_depth;
    uint32_t projection_flags;
} XgSpriteFt4Record;

XgSpriteFt4Result xg_sprite_ft4_build(
    const XgSpriteFt4Source *source, XgSpriteFt4Record *record);

XgSpriteFt4Result xg_sprite_ft4_map_uv(
    uint8_t base_u, uint8_t base_v, uint8_t width, uint8_t height,
    bool horizontal_reversed,
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2]);

XgSpriteFt4Result xg_sprite_ft4_select_ot_address(
    uint32_t base_address, uint32_t sprite_flags, uint32_t descriptor_flags,
    uint32_t *out_address);

#ifdef __cplusplus
}
#endif

#endif
