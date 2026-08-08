#ifndef XG_WORLD_HORIZON_H
#define XG_WORLD_HORIZON_H

#include "xg_host_3d.h"
#include "xg_native_view.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_HORIZON_QUAD_COUNT = 2,
};

typedef enum XgWorldHorizonResult {
    XG_WORLD_HORIZON_OK = 0,
    XG_WORLD_HORIZON_INVALID_ARGUMENT,
    XG_WORLD_HORIZON_INVALID_SOURCE,
    XG_WORLD_HORIZON_BUILD_FAILED,
} XgWorldHorizonResult;

typedef struct XgWorldHorizonSource {
    XgHost3dVector vertices[XG_WORLD_HORIZON_QUAD_COUNT]
                               [XG_HOST_3D_VERTEX_COUNT];
    XgHost3dProjection projection;
    XgRenderIrMaterialState material;
    uint32_t ordering_shift;
    uint16_t angle;
} XgWorldHorizonSource;

typedef struct XgWorldHorizonRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    uint32_t ordering_bucket;
    uint32_t projection_flags;
    uint16_t fourth_depth;
    bool accepted;
} XgWorldHorizonRecord;

XgWorldHorizonResult xg_world_horizon_build(
    const XgWorldHorizonSource *source,
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT]);
XgWorldHorizonResult xg_world_horizon_build_for_view(
    const XgWorldHorizonSource *source, const XgNativeView *view,
    XgWorldHorizonRecord records[XG_WORLD_HORIZON_QUAD_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
