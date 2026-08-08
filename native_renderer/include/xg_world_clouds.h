#ifndef XG_WORLD_CLOUDS_H
#define XG_WORLD_CLOUDS_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_CLOUD_COUNT = 80,
    XG_WORLD_CLOUD_FAR_QUAD_COUNT = 3,
    XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT = 12,
    XG_WORLD_CLOUD_NEAR_QUAD_COUNT = 48,
    XG_WORLD_CLOUD_UV_BASE_COUNT = 8,
    XG_WORLD_CLOUD_PACKET_ENTRY_LIMIT = 240,
    XG_WORLD_CLOUD_PACKET_CAPACITY = 288,
    XG_WORLD_CLOUD_PACKET_WORD_COUNT = 10,
    XG_WORLD_CLOUD_OT_BUCKET_COUNT = 1024,
};

typedef enum XgWorldCloudLod {
    XG_WORLD_CLOUD_LOD_NEAR = 0,
    XG_WORLD_CLOUD_LOD_MIDDLE = 1,
    XG_WORLD_CLOUD_LOD_FAR = 2,
} XgWorldCloudLod;

typedef enum XgWorldCloudsResult {
    XG_WORLD_CLOUDS_OK = 0,
    XG_WORLD_CLOUDS_INVALID_ARGUMENT,
    XG_WORLD_CLOUDS_INVALID_SOURCE,
    XG_WORLD_CLOUDS_CAPACITY_EXCEEDED,
    XG_WORLD_CLOUDS_BUILD_FAILED,
} XgWorldCloudsResult;

typedef struct XgWorldCloudPosition {
    int32_t x;
    int32_t y;
    int32_t z;
} XgWorldCloudPosition;

typedef struct XgWorldCloudVelocity {
    int16_t x;
    uint16_t uv_group;
    int16_t z;
} XgWorldCloudVelocity;

typedef struct XgWorldCloudTrigValue {
    int16_t sine;
    int16_t cosine;
} XgWorldCloudTrigValue;

typedef struct XgWorldCloudsSource {
    XgHost3dMatrix camera;
    XgHost3dMatrix billboard;
    XgHost3dVector far_vertices[XG_WORLD_CLOUD_FAR_QUAD_COUNT]
                                     [XG_HOST_3D_VERTEX_COUNT];
    XgHost3dVector middle_vertices[XG_WORLD_CLOUD_MIDDLE_QUAD_COUNT]
                                        [XG_HOST_3D_VERTEX_COUNT];
    XgWorldCloudPosition positions[XG_WORLD_CLOUD_COUNT];
    XgWorldCloudVelocity velocities[XG_WORLD_CLOUD_COUNT];
    uint16_t uv_base[XG_WORLD_CLOUD_UV_BASE_COUNT];
    XgWorldCloudTrigValue pitch_rotation;
    XgWorldCloudTrigValue negative_yaw_rotation;
    XgWorldCloudTrigValue positive_yaw_rotation;
    XgWorldCloudTrigValue wedge_left;
    XgWorldCloudTrigValue wedge_right;
    XgRenderIrMaterialState material;
    int32_t camera_origin_x;
    int32_t camera_origin_z;
    int32_t wedge_offset_x;
    int32_t wedge_offset_z;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    int16_t depth_cue_a;
    int32_t depth_cue_b;
    uint16_t projection_distance;
} XgWorldCloudsSource;

typedef struct XgWorldCloudRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    uint16_t uv[XG_HOST_3D_VERTEX_COUNT];
    uint32_t projection_flags;
    uint32_t ordering_depth;
    uint32_t ordering_bucket;
    uint32_t emission_index;
    uint32_t prior_emission_in_bucket;
    uint32_t source_index;
    uint32_t lod_quad_index;
    uint32_t material_word;
    uint16_t tpage;
    uint16_t clut;
    XgWorldCloudLod lod;
} XgWorldCloudRecord;

typedef struct XgWorldCloudsBuildStats {
    uint32_t clouds_entered;
    uint32_t clouds_world_culled;
    uint32_t clouds_anchor_culled;
    uint32_t near_groups_culled;
    uint32_t quad_attempt_count;
    uint32_t quad_projection_culled;
    uint32_t quad_screen_culled;
    uint32_t far_preinsert_depth_stops;
    uint32_t far_postinsert_depth_stops;
    uint32_t first_world_accepted_source;
    uint32_t first_anchor_flags;
    int32_t first_anchor_translation_x;
    int32_t first_anchor_translation_y;
    int32_t first_anchor_translation_z;
    int32_t first_world_relative_x;
    int32_t first_world_relative_z;
    bool has_world_accepted_source;
    bool packet_entry_limit_stopped;
} XgWorldCloudsBuildStats;

XgWorldCloudsResult xg_world_clouds_step_positions(
    XgWorldCloudPosition positions[XG_WORLD_CLOUD_COUNT],
    const XgWorldCloudVelocity velocities[XG_WORLD_CLOUD_COUNT]);

XgWorldCloudsResult xg_world_clouds_build(
    const XgWorldCloudsSource *source,
    XgWorldCloudRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count,
    XgWorldCloudsBuildStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif
