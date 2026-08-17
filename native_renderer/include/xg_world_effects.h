#ifndef XG_WORLD_EFFECTS_H
#define XG_WORLD_EFFECTS_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_EFFECTS_SOURCE_CAPACITY = 256,
    XG_WORLD_EFFECTS_TYPE_COUNT = 10,
};

typedef enum XgWorldEffectsResult {
    XG_WORLD_EFFECTS_OK = 0,
    XG_WORLD_EFFECTS_INVALID_ARGUMENT,
    XG_WORLD_EFFECTS_INVALID_SOURCE,
    XG_WORLD_EFFECTS_CAPACITY_EXCEEDED,
    XG_WORLD_EFFECTS_BUILD_FAILED,
} XgWorldEffectsResult;

typedef enum XgWorldEffectsCull {
    XG_WORLD_EFFECTS_CULL_NONE = 0,
    XG_WORLD_EFFECTS_CULL_PROJECTIVE,
    XG_WORLD_EFFECTS_CULL_SCREEN,
    XG_WORLD_EFFECTS_CULL_DEPTH,
} XgWorldEffectsCull;

typedef struct XgWorldEffectsParticleSource {
    int32_t position[3];
    int16_t angle;
    int16_t sine;
    int16_t cosine;
    uint16_t scale_x;
    uint16_t scale_y;
    uint16_t tpage;
    uint8_t type;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    bool rotate;
    bool active;
} XgWorldEffectsParticleSource;

typedef struct XgWorldEffectsSource {
    XgHost3dMatrix camera;
    XgHost3dMatrix billboard;
    XgHost3dVector vertices[XG_WORLD_EFFECTS_TYPE_COUNT]
                           [XG_HOST_3D_VERTEX_COUNT];
    uint16_t uv[XG_WORLD_EFFECTS_TYPE_COUNT][XG_HOST_3D_VERTEX_COUNT];
    XgWorldEffectsParticleSource particles[XG_WORLD_EFFECTS_SOURCE_CAPACITY];
    XgRenderIrMaterialState material;
    int32_t camera_origin_x;
    int32_t camera_origin_z;
    int32_t wrap_x;
    int32_t wrap_z;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
} XgWorldEffectsSource;

typedef struct XgWorldEffectsRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    uint32_t projection_flags;
    uint32_t ordering_bucket;
    uint16_t fourth_depth;
    uint16_t uv[XG_HOST_3D_VERTEX_COUNT];
    uint16_t tpage;
    uint16_t clut;
    uint32_t material_word;
    uint32_t source_index;
    XgWorldEffectsCull cull;
    bool accepted;
} XgWorldEffectsRecord;

XgWorldEffectsResult xg_world_effects_build(
    const XgWorldEffectsSource *source,
    XgWorldEffectsRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count);

XgWorldEffectsResult xg_world_effects_build_with_temporal(
    const XgWorldEffectsSource *source,
    XgWorldEffectsRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count,
    XgWorldEffectsRecord *rejected_records,
    uint32_t rejected_capacity,
    uint32_t *out_rejected_count);

#ifdef __cplusplus
}
#endif

#endif
