#ifndef XG_WORLD_TERRAIN_WATER_SNAPSHOT_TYPES_H
#define XG_WORLD_TERRAIN_WATER_SNAPSHOT_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct XgWorldTerrainWaterBuildDiagnostics {
    uint32_t active_tiles;
    uint32_t selected_quadrants;
    uint32_t rejected_quadrants;
    uint32_t considered_triangles;
    uint32_t projective_vertices;
    uint32_t projective_invalid_x;
    uint32_t projective_invalid_y;
    uint32_t projective_invalid_z;
    uint32_t projective_invalid_near;
    uint32_t emitted_projective_vertices;
    uint32_t shared_duplicate_vertices;
    uint32_t shared_raster_conflicts;
    uint32_t projection_rejects;
    uint32_t screen_rejects;
    uint32_t backface_rejects;
    uint32_t depth_rejects;
    uint32_t emitted_triangles;
    uint32_t packet_limit_stops;
} XgWorldTerrainWaterBuildDiagnostics;

typedef enum XgWorldTerrainWaterShadowBlocker {
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_NONE = 0,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_NESTED_BEGIN,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_ARGUMENT,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_AUTHENTICATION,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CPU_ACCESS,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_ENTRY_PC,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_FINISH_PC,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_STACK,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_CONTEXT,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_ADDRESS_RANGE,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_SOURCE_CAPTURE,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_BUILD,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_INVALID_BUILD_OUTPUT,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_UNMATCHED_FINISH,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_FINISH_STACK,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_ORIGINAL_COUNT,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_COUNTER_OVERFLOW,
    XG_WORLD_TERRAIN_WATER_SHADOW_BLOCKER_LIFECYCLE_PENDING,
} XgWorldTerrainWaterShadowBlocker;

typedef enum XgWorldTerrainWaterShadowMismatchKind {
    XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_NONE = 0,
    XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_COUNT,
    XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_PACKET,
    XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_TOUCHED_OT,
    XG_WORLD_TERRAIN_WATER_SHADOW_MISMATCH_UNTOUCHED_OT,
} XgWorldTerrainWaterShadowMismatchKind;

typedef enum XgWorldTerrainWaterShadowMismatchField {
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TAG = 1u << 0,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_MATERIAL = 1u << 1,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY0 = 1u << 2,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV0 = 1u << 3,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_CLUT = 1u << 4,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY1 = 1u << 5,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV1 = 1u << 6,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TPAGE = 1u << 7,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_XY2 = 1u << 8,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UV2 = 1u << 9,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_MATERIAL = 1u << 10,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNCHANGED_UV2_HIGH = 1u << 11,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_COUNT = 1u << 12,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_TOUCHED_OT = 1u << 13,
    XG_WORLD_TERRAIN_WATER_SHADOW_FIELD_UNTOUCHED_OT = 1u << 14,
} XgWorldTerrainWaterShadowMismatchField;

typedef struct XgWorldTerrainWaterShadowMismatch {
    XgWorldTerrainWaterShadowMismatchKind kind;
    uint64_t invocation;
    uint32_t field_bits;
    uint32_t record_index;
    uint32_t source_primitive_index;
    uint32_t packet_address;
    uint32_t address;
    uint32_t expected;
    uint32_t actual;
    uint32_t expected_tag;
    uint32_t actual_tag;
    uint32_t expected_material;
    uint32_t actual_material;
    uint32_t expected_xy[3];
    uint32_t actual_xy[3];
    uint16_t expected_uv[3];
    uint16_t actual_uv[3];
    uint16_t expected_clut;
    uint16_t actual_clut;
    uint16_t expected_tpage;
    uint16_t actual_tpage;
    uint16_t expected_uv2_high;
    uint16_t actual_uv2_high;
} XgWorldTerrainWaterShadowMismatch;

typedef struct XgWorldTerrainWaterShadowSnapshot {
    uint64_t native_cutover_count;
    uint64_t native_primitive_count;
    uint64_t begin_count;
    uint64_t completion_count;
    uint64_t invocation_match_count;
    uint64_t invocation_mismatch_count;
    uint64_t lifecycle_invalidation_count;
    uint64_t pending_block_count;
    uint64_t block_count;
    uint64_t source_capture_failure_count;
    uint64_t build_failure_count;
    uint64_t source_read_count;
    uint64_t source_read_bytes;
    uint64_t captured_resource_count;
    uint64_t candidate_count;
    uint64_t original_primitive_count;
    uint64_t packet_compared_count;
    uint64_t packet_match_count;
    uint64_t packet_mismatch_count;
    uint64_t missing_primitive_count;
    uint64_t unexpected_primitive_count;
    uint64_t count_mismatch_count;
    uint64_t payload_mismatch_count;
    uint64_t material_mismatch_count;
    uint64_t geometry_mismatch_count;
    uint64_t geometry_vertex_mismatch_count;
    uint64_t uv_mismatch_count;
    uint64_t clut_mismatch_count;
    uint64_t tpage_mismatch_count;
    uint64_t tag_mismatch_count;
    uint64_t packet_unchanged_mismatch_count;
    uint64_t touched_ot_bucket_count;
    uint64_t untouched_ot_bucket_count;
    uint64_t touched_ot_mismatch_count;
    uint64_t untouched_ot_mismatch_count;
    uint64_t authentication_generation;
    uint32_t last_capture_result;
    uint32_t last_build_result;
    uint32_t last_candidate_count;
    uint32_t last_original_count;
    uint32_t last_caller_return;
    int32_t last_position_x;
    int32_t last_position_z;
    uint16_t last_projection_distance;
    uint32_t last_ot_base;
    uint32_t last_packet_base;
    uint32_t blocker_detail;
    uint32_t blocker_address;
    uint32_t blocker_expected;
    uint32_t blocker_actual;
    uint32_t mesh_duplicate_vertices;
    uint32_t mesh_cross_tile_duplicate_vertices;
    uint32_t mesh_canonical_raster_conflicts;
    uint32_t mesh_native_raster_conflicts;
    uint32_t mesh_cross_tile_native_raster_conflicts;
    XgWorldTerrainWaterBuildDiagnostics build_diagnostics;
    XgWorldTerrainWaterShadowBlocker blocker;
    XgWorldTerrainWaterShadowMismatch first_mismatch;
    bool pending;
    bool blocked;
} XgWorldTerrainWaterShadowSnapshot;

#endif
