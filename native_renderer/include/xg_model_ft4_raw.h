#ifndef XG_MODEL_FT4_RAW_H
#define XG_MODEL_FT4_RAW_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgModelFt4RawResult {
    XG_MODEL_FT4_RAW_OK = 0,
    XG_MODEL_FT4_RAW_INVALID_ARGUMENT,
    XG_MODEL_FT4_RAW_INVALID_SOURCE,
    XG_MODEL_FT4_RAW_BUILD_FAILED,
} XgModelFt4RawResult;

typedef enum XgModelFt4RawDispatchMode {
    XG_MODEL_FT4_RAW_DISPATCH_AVERAGE = 0,
    XG_MODEL_FT4_RAW_DISPATCH_FARTHEST = 2,
    XG_MODEL_FT4_RAW_DISPATCH_NEAREST = 3,
    XG_MODEL_FT4_RAW_DISPATCH_AVERAGE_DEPTH_CUE = 4,
    XG_MODEL_FT4_RAW_DISPATCH_FARTHEST_DEPTH_CUE = 5,
} XgModelFt4RawDispatchMode;

typedef struct XgModelFt4RawSource {
    XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT];
    XgHost3dProjection projection;
    XgRenderIrMaterialState material;
    uint8_t uv[XG_HOST_3D_VERTEX_COUNT][2];
    uint32_t screen_right;
    int32_t screen_x_cull_margin;
    uint32_t packed_screen_bottom;
    uint32_t packet_address;
    uint32_t ordering_shift;
    uint32_t material_word;
    uint32_t depth_cue_color_word;
    int32_t far_color[3];
    uint8_t dispatch_mode;
} XgModelFt4RawSource;

typedef struct XgModelFt4RawRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    int32_t nclip;
    uint32_t projection_flags;
    uint32_t ordering_bucket;
    uint32_t material_word;
    bool passed_screen_cull;
    bool counter_incremented;
    bool accepted;
} XgModelFt4RawRecord;

XgModelFt4RawResult xg_model_ft4_raw_build(
    const XgModelFt4RawSource *source, XgModelFt4RawRecord *record);

#ifdef __cplusplus
}
#endif

#endif
