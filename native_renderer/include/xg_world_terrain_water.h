#ifndef XG_WORLD_TERRAIN_WATER_H
#define XG_WORLD_TERRAIN_WATER_H

#include "xg_host_3d.h"
#include "xg_render_ir.h"
#include "xg_world_terrain_water_snapshot_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_TERRAIN_WATER_TILE_COUNT = 25,
    XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT = 4,
    XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE = 9,
    XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT = 81,
    XG_WORLD_TERRAIN_WATER_CELL_SIDE = 8,
    XG_WORLD_TERRAIN_WATER_PAGE_COUNT = 8,
    XG_WORLD_TERRAIN_WATER_CLUT_COUNT = 64,
    XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY = 2047,
    XG_WORLD_TERRAIN_WATER_UNCULLED_CAPACITY =
        XG_WORLD_TERRAIN_WATER_TILE_COUNT *
        XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT *
        XG_WORLD_TERRAIN_WATER_CELL_SIDE *
        XG_WORLD_TERRAIN_WATER_CELL_SIDE * 2,
    XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY = 145 * 145,
};

typedef enum XgWorldTerrainWaterResult {
    XG_WORLD_TERRAIN_WATER_OK = 0,
    XG_WORLD_TERRAIN_WATER_INVALID_ARGUMENT,
    XG_WORLD_TERRAIN_WATER_INVALID_SOURCE,
    XG_WORLD_TERRAIN_WATER_CAPACITY_EXCEEDED,
    XG_WORLD_TERRAIN_WATER_BUILD_FAILED,
} XgWorldTerrainWaterResult;

enum {
    XG_WORLD_TERRAIN_WATER_CULL_SCREEN = 1u << 0,
    XG_WORLD_TERRAIN_WATER_CULL_BACKFACE = 1u << 1,
    XG_WORLD_TERRAIN_WATER_CULL_DEPTH = 1u << 2,
};

typedef struct XgWorldTerrainWaterTileSource {
    uint32_t samples[XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT]
                    [XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT];
    uint32_t resource_address;
    uint16_t terrain_id;
    uint8_t grid_index;
    bool active;
    bool has_data;
} XgWorldTerrainWaterTileSource;

typedef struct XgWorldTerrainWaterSource {
    XgHost3dMatrix camera;
    XgHost3dMatrix local;
    XgWorldTerrainWaterTileSource tiles[XG_WORLD_TERRAIN_WATER_TILE_COUNT];
    uint16_t quadrant_visibility[XG_WORLD_TERRAIN_WATER_TILE_COUNT]
                                [XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT];
    int16_t wave_sine_x[XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE];
    int16_t wave_sine_z[XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE];
    uint16_t cluts[XG_WORLD_TERRAIN_WATER_CLUT_COUNT];
    uint16_t tpages[XG_WORLD_TERRAIN_WATER_PAGE_COUNT];
    XgRenderIrMaterialState material;
    int32_t position_x;
    int32_t position_z;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint32_t wave_phase_x;
    uint32_t wave_phase_z;
    uint32_t fog_mode;
    uint16_t projection_distance;
} XgWorldTerrainWaterSource;

typedef struct XgWorldTerrainWaterRecord {
    XgRenderIrNativePrimitive primitive;
    XgHost3dVector source_vertices[3];
    XgHost3dProjectedVertex projected_vertices[3];
    uint32_t projection_flags;
    uint32_t ordering_bucket;
    uint32_t allocation_ordinal;
    uint32_t source_primitive_index;
    uint32_t interpolation_primitive_id;
    uint32_t ordering_predecessor_record;
    int16_t depth_cue;
    uint16_t encoded_clut;
    uint16_t encoded_tpage;
    uint16_t max_depth;
    uint8_t tile_index;
    uint8_t quadrant_index;
    uint8_t cell_x;
    uint8_t cell_z;
    uint8_t triangle_index;
    uint8_t uv_orientation;
    uint8_t page_selector;
    bool animated_height;
    bool alternate_diagonal;
    bool alternate_clut_bank;
    bool ordering_predecessor_is_external;
    bool temporal_only;
    uint8_t temporal_cull_reasons;
} XgWorldTerrainWaterRecord;

typedef struct XgWorldTerrainWaterAnchor {
    XgRenderIrMaterialState material;
    XgRenderIrVertex vertex;
} XgWorldTerrainWaterAnchor;

XgWorldTerrainWaterResult xg_world_terrain_water_build(
    const XgWorldTerrainWaterSource *source,
    XgWorldTerrainWaterRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count);
XgWorldTerrainWaterResult xg_world_terrain_water_build_unculled(
    const XgWorldTerrainWaterSource *source,
    XgWorldTerrainWaterRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count);
uint32_t xg_world_terrain_water_interpolation_primitive_id(
    uint8_t grid_index, uint8_t terrain_id, uint16_t local_primitive);
XgWorldTerrainWaterResult xg_world_terrain_water_append_temporal_anchors(
    const XgWorldTerrainWaterSource *previous,
    const XgWorldTerrainWaterSource *current,
    XgWorldTerrainWaterAnchor *anchors, uint32_t anchor_capacity,
    uint32_t *in_out_anchor_count);
XgWorldTerrainWaterResult xg_world_terrain_water_append_temporal_tile_anchors(
    const XgWorldTerrainWaterTileSource *tiles, uint32_t tile_count,
    const XgWorldTerrainWaterSource *current,
    XgWorldTerrainWaterAnchor *anchors, uint32_t anchor_capacity,
    uint32_t *in_out_anchor_count);
void xg_world_terrain_water_build_diagnostics(
    XgWorldTerrainWaterBuildDiagnostics *out_diagnostics);
void xg_world_terrain_water_set_temporal_coverage(bool enabled);
bool xg_world_terrain_water_temporal_coverage(void);

#ifdef __cplusplus
}
#endif

#endif
