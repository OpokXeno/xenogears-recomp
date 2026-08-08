#ifndef XG_WORLD_SKY_H
#define XG_WORLD_SKY_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_SKY_QUAD_COUNT = 4,
};

typedef enum XgWorldSkyResult {
    XG_WORLD_SKY_OK = 0,
    XG_WORLD_SKY_INVALID_ARGUMENT,
    XG_WORLD_SKY_INVALID_SOURCE,
    XG_WORLD_SKY_BUILD_FAILED,
} XgWorldSkyResult;

typedef struct XgWorldSkySource {
    XgHost3dVector vertices[XG_WORLD_SKY_QUAD_COUNT]
                               [XG_HOST_3D_VERTEX_COUNT];
    XgHost3dProjection projection;
    XgRenderIrMaterialState material;
    uint32_t ordering_shift;
    uint32_t buffer_index;
} XgWorldSkySource;

typedef struct XgWorldSkyRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    uint32_t packet_address;
    uint32_t ordering_bucket;
    uint32_t projection_flags;
    bool accepted;
} XgWorldSkyRecord;

XgWorldSkyResult xg_world_sky_build(
    const XgWorldSkySource *source,
    XgWorldSkyRecord records[XG_WORLD_SKY_QUAD_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
