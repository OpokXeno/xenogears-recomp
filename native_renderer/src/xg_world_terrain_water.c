#include "xg_world_terrain_water.h"

#include "xg_world_terrain_water_source_capture.h"

#include <limits.h>
#include <string.h>

enum {
    TERRAIN_PACKET_STOP_COUNT = 0x7fe,
    TERRAIN_ORDERING_BUCKET_COUNT = 0xf0,
};

static XgWorldTerrainWaterBuildDiagnostics build_diagnostics;
static bool temporal_coverage_enabled;
static bool unculled_build;

void xg_world_terrain_water_set_temporal_coverage(bool enabled) {
    temporal_coverage_enabled = enabled;
}

bool xg_world_terrain_water_temporal_coverage(void) {
    return temporal_coverage_enabled;
}

uint32_t xg_world_terrain_water_interpolation_primitive_id(
        uint8_t grid_index, uint8_t terrain_id, uint16_t local_primitive) {
    const uint32_t tile_identity =
        ((uint32_t)grid_index << 8u) | terrain_id;

    return tile_identity * 512u +
        (local_primitive & UINT16_C(0x01ff));
}

void xg_world_terrain_water_build_diagnostics(
    XgWorldTerrainWaterBuildDiagnostics *out_diagnostics) {
    if (out_diagnostics != NULL) *out_diagnostics = build_diagnostics;
}

static int32_t u32_as_i32(uint32_t value) {
    if (value <= INT32_MAX) return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int32_t wrap_i32(int64_t value) {
    return u32_as_i32((uint32_t)(uint64_t)value);
}

static int16_t wrap_i16(int32_t value) {
    const uint16_t low = (uint16_t)(uint32_t)value;

    if (low <= INT16_MAX) return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static int32_t shift_right_floor(int64_t value, unsigned bits) {
    uint64_t magnitude;

    if (value >= 0) return (int32_t)(value >> bits);
    magnitude = (uint64_t)(-(value + 1)) + 1u;
    return -(int32_t)((magnitude + (((uint64_t)1u << bits) - 1u)) >> bits);
}

static int32_t signed_byte(uint32_t value) {
    const uint32_t byte = value & 0xffu;

    return byte < 0x80u ? (int32_t)byte : (int32_t)byte - 0x100;
}

static bool compute_fog(uint32_t mode, int32_t projection_distance,
                        int16_t *depth_cue_a, int32_t *depth_cue_b) {
    const int32_t near_distance = mode == 2u ? 0xb00 : 0x800;
    const int32_t far_distance = 0xe80;
    const int32_t range = far_distance - near_distance;
    int64_t value;

    if (projection_distance == 0 || depth_cue_a == NULL || depth_cue_b == NULL)
        return false;
    value = ((int64_t)-near_distance * far_distance / range) * 256;
    value /= projection_distance;
    if (value < INT16_MIN) value = INT16_MIN;
    if (value > INT16_MAX) value = INT16_MAX;
    *depth_cue_a = (int16_t)value;
    value = ((int64_t)far_distance * 4096 / range) * 4096;
    *depth_cue_b = wrap_i32(value);
    return true;
}

static bool material_template_is_valid(
    const XgRenderIrMaterialState *material) {
    return material->tpage == 0u && material->texture_page_x == 0u &&
           material->texture_page_y == 0u && material->clut_x == 0u &&
           material->clut_y == 0u &&
           material->draw_area_left <= material->draw_area_right &&
           material->draw_area_top <= material->draw_area_bottom &&
           material->draw_area_right <= 1023u &&
           material->draw_area_bottom <= 1023u &&
           material->draw_offset_x >= -1024 &&
           material->draw_offset_x <= 1023 &&
           material->draw_offset_y >= -1024 &&
           material->draw_offset_y <= 1023 &&
           material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT &&
           material->texture_window_mask_x <= 31u &&
           material->texture_window_mask_y <= 31u &&
           material->texture_window_offset_x <= 31u &&
           material->texture_window_offset_y <= 31u &&
           material->shading == XG_RENDER_IR_SHADING_FLAT &&
           material->textured && !material->raw_texture &&
           !material->semi_transparent &&
           material->blend_mode == XG_RENDER_IR_BLEND_AVERAGE;
}

static bool tpage_is_valid(uint16_t tpage) {
    return tpage <= 0x1ffu && ((tpage >> 7u) & 3u) != 3u;
}

static bool clut_is_valid(uint16_t clut) {
    return (clut >> 6u) <= 511u;
}

static void select_tile_quadrants(const XgWorldTerrainWaterSource *source,
                                  uint32_t tile_index,
                                  bool selected[4]) {
    uint32_t quadrant;
    uint16_t combined_mask = 0u;

    memset(selected, 0, sizeof(*selected) * 4u);
    for (quadrant = 0u; quadrant < 4u; ++quadrant)
        combined_mask |= source->quadrant_visibility[tile_index][quadrant];
    for (quadrant = 0u; quadrant < 4u; ++quadrant) {
        selected[quadrant] =
            (combined_mask != UINT16_MAX ||
             source->quadrant_visibility[tile_index][quadrant] != UINT16_MAX);
    }
}

static bool triangle_is_on_screen(
    const XgHost3dProjectedVertex vertices[3], int32_t x_margin) {
    const uint32_t x_limit = 320u + 2u * (uint32_t)x_margin;
    bool x_visible = false;
    bool y_visible = false;
    uint32_t vertex;

    for (vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t x =
            ((uint32_t)((int32_t)vertices[vertex].x + x_margin)) & 0xffffu;

        if (x < x_limit) x_visible = true;
        if ((uint32_t)(int32_t)vertices[vertex].y < 216u) y_visible = true;
    }
    return x_visible && y_visible;
}

static bool triangle_is_front_facing(
    const XgHost3dProjectedVertex vertices[3]) {
    const int64_t area =
        (int64_t)vertices[0].x * vertices[1].y +
        (int64_t)vertices[1].x * vertices[2].y +
        (int64_t)vertices[2].x * vertices[0].y -
        (int64_t)vertices[0].x * vertices[2].y -
        (int64_t)vertices[1].x * vertices[0].y -
        (int64_t)vertices[2].x * vertices[1].y;

    return area > 0;
}

static void decode_material(uint16_t tpage, uint16_t clut,
                            XgRenderIrMaterialState *material) {
    material->tpage = tpage;
    material->texture_page_x = tpage & 0x0fu;
    material->texture_page_y = (tpage >> 4u) & 1u;
    material->texture_depth =
        (XgRenderIrTextureDepth)((tpage >> 7u) & 3u);
    material->blend_mode = (XgRenderIrBlendMode)((tpage >> 5u) & 3u);
    material->clut_x = (clut & 0x3fu) << 4u;
    material->clut_y = clut >> 6u;
}

static void make_grid(const XgWorldTerrainWaterSource *source,
                      const uint32_t samples[XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT],
                      int16_t origin_x, int16_t origin_z,
                      XgHost3dVector grid[XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT]) {
    uint32_t z;

    for (z = 0u; z < XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE; ++z) {
        uint32_t x;

        for (x = 0u; x < XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE; ++x) {
            const uint32_t index = z * XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE + x;
            const uint32_t word = samples[index];
            int32_t height = signed_byte(word) * 8;

            if ((word & 0x1000u) != 0u) {
                height += shift_right_floor(
                    (int64_t)source->wave_sine_x[x] *
                        ((int32_t)source->wave_sine_z[z] * 2),
                    20u);
            }
            grid[index] = (XgHost3dVector){
                wrap_i16((int32_t)origin_x + (int32_t)x * 0x80),
                wrap_i16(height),
                wrap_i16((int32_t)origin_z - (int32_t)z * 0x80),
                0u,
            };
        }
    }
}

static void make_uv(uint32_t word, uint16_t uv[4]) {
    const uint16_t base =
        (uint16_t)(((word >> 12u) & 0x00f0u) | ((word >> 8u) & 0xf000u));

    switch ((word >> 13u) & 3u) {
    case 0u:
        uv[0] = base;
        uv[1] = (uint16_t)(base + 0x000fu);
        uv[2] = (uint16_t)(base + 0x0f00u);
        uv[3] = (uint16_t)(base + 0x0f0fu);
        break;
    case 1u:
        uv[0] = (uint16_t)(base + 0x000fu);
        uv[1] = base;
        uv[2] = (uint16_t)(base + 0x0f0fu);
        uv[3] = (uint16_t)(base + 0x0f00u);
        break;
    case 2u:
        uv[0] = (uint16_t)(base + 0x0f00u);
        uv[1] = (uint16_t)(base + 0x0f0fu);
        uv[2] = base;
        uv[3] = (uint16_t)(base + 0x000fu);
        break;
    default:
        uv[0] = (uint16_t)(base + 0x0f0fu);
        uv[1] = (uint16_t)(base + 0x0f00u);
        uv[2] = (uint16_t)(base + 0x000fu);
        uv[3] = base;
        break;
    }
}

static void copy_interpolation_position(XgRenderIrVertex *destination,
                                        const XgRenderIrVertex *source) {
    destination->x = source->x;
    destination->y = source->y;
    destination->native_view_x = source->native_view_x;
    destination->native_view_y = source->native_view_y;
    destination->native_view_position = source->native_view_position;
    destination->projective_view_x = source->projective_view_x;
    destination->projective_view_y = source->projective_view_y;
    destination->projective_view_z = source->projective_view_z;
    destination->projective_offset_x = source->projective_offset_x;
    destination->projective_offset_y = source->projective_offset_y;
    destination->projective_native_offset_x =
        source->projective_native_offset_x;
    destination->projective_native_offset_y =
        source->projective_native_offset_y;
    destination->projective_distance = source->projective_distance;
    destination->projective_position = source->projective_position;
}

static bool raster_position_equal(const XgRenderIrVertex *left,
                                  const XgRenderIrVertex *right) {
    return shift_right_floor(left->x, 16u) ==
               shift_right_floor(right->x, 16u) &&
           shift_right_floor(left->y, 16u) ==
               shift_right_floor(right->y, 16u) &&
           left->native_view_position == right->native_view_position &&
           (!left->native_view_position ||
            (shift_right_floor(left->native_view_x, 16u) ==
                 shift_right_floor(right->native_view_x, 16u) &&
             shift_right_floor(left->native_view_y, 16u) ==
                 shift_right_floor(right->native_view_y, 16u)));
}

static void reconcile_shared_vertices(XgWorldTerrainWaterRecord *records,
                                      uint32_t record_count) {
    enum { WORLD_VERTEX_SIDE = 145, WORLD_VERTEX_COUNT = 145 * 145 };
    uint16_t first_record[WORLD_VERTEX_COUNT];
    uint8_t first_vertex[WORLD_VERTEX_COUNT];
    uint32_t record_index;

    memset(first_record, 0xff, sizeof(first_record));
    memset(first_vertex, 0, sizeof(first_vertex));
    for (record_index = 0u; record_index < record_count; ++record_index) {
        XgRenderIrTriangle *triangle =
            &records[record_index].primitive.triangles[0];
        uint32_t vertex_index;

        for (vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            XgRenderIrVertex *vertex = &triangle->vertices[vertex_index];
            const uint32_t tile_identity =
                vertex->interpolation_group_id & UINT32_C(0x00ffffff);
            const uint32_t grid_index = (tile_identity >> 8u) & 0xffu;
            const uint32_t local_vertex = vertex->interpolation_vertex_id;
            const uint32_t global_x =
                (grid_index % 9u) * 16u + local_vertex % 17u;
            const uint32_t global_z =
                (grid_index / 9u) * 16u + local_vertex / 17u;
            const uint32_t global_vertex =
                global_z * WORLD_VERTEX_SIDE + global_x;
            XgRenderIrVertex *first;

            if (grid_index >= 81u || local_vertex >= 289u ||
                global_vertex >= WORLD_VERTEX_COUNT)
                continue;
            if (first_record[global_vertex] == UINT16_MAX) {
                first_record[global_vertex] = (uint16_t)record_index;
                first_vertex[global_vertex] = (uint8_t)vertex_index;
                vertex->interpolation_group_id = UINT32_C(0x63000000);
                vertex->interpolation_vertex_id = global_vertex;
                continue;
            }
            first = &records[first_record[global_vertex]].primitive.triangles[0]
                         .vertices[first_vertex[global_vertex]];
            ++build_diagnostics.shared_duplicate_vertices;
            if (!raster_position_equal(first, vertex)) {
                ++build_diagnostics.shared_raster_conflicts;
                continue;
            }
            copy_interpolation_position(vertex, first);
            vertex->interpolation_group_id = UINT32_C(0x63000000);
            vertex->interpolation_vertex_id = global_vertex;
        }
    }
}

static XgWorldTerrainWaterResult emit_cell(
    const XgWorldTerrainWaterSource *source,
    const XgHost3dProjection *projection,
    const XgHost3dVector grid[XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT],
    uint32_t word, uint32_t tile_index, uint32_t quadrant_index,
    uint32_t cell_x, uint32_t cell_z,
    XgWorldTerrainWaterRecord *records, uint32_t *record_count,
    uint32_t bucket_heads[TERRAIN_ORDERING_BUCKET_COUNT]) {
    static const uint8_t triangle_corners[2][2][3] = {
        { { 0u, 1u, 2u }, { 1u, 3u, 2u } },
        { { 0u, 3u, 2u }, { 1u, 3u, 0u } },
    };
    XgHost3dVector corners[4];
    uint16_t uv[4];
    const uint32_t diagonal = (word >> 15u) & 1u;
    const uint32_t page_selector = (word >> 8u) & 7u;
    const uint32_t grid_tile = source->tiles[tile_index].grid_index;
    const uint32_t terrain_id = source->tiles[tile_index].terrain_id;
    const uint32_t tile_identity = (grid_tile << 8u) | terrain_id;
    uint32_t triangle;

    corners[0] = grid[cell_z * 9u + cell_x];
    corners[1] = grid[cell_z * 9u + cell_x + 1u];
    corners[2] = grid[(cell_z + 1u) * 9u + cell_x];
    corners[3] = grid[(cell_z + 1u) * 9u + cell_x + 1u];
    make_uv(word, uv);

    for (triangle = 0u; triangle < 2u; ++triangle) {
        XgHost3dProject4Input input = { 0 };
        XgHost3dRotTransPers4Output output;
        XgWorldTerrainWaterRecord candidate = { 0 };
        uint16_t max_depth = 0u;
        uint16_t clut;
        uint32_t clut_index;
        uint32_t vertex;
        uint32_t bucket;
        bool temporal_only = false;
        uint8_t temporal_cull_reasons = 0u;

        ++build_diagnostics.considered_triangles;

        for (vertex = 0u; vertex < 3u; ++vertex) {
            input.vertices[vertex] =
                corners[triangle_corners[diagonal][triangle][vertex]];
        }
        input.vertices[3] = input.vertices[2];
        input.projection = *projection;
        if (!xg_host_3d_rot_trans_pers4(&input, &output))
            return XG_WORLD_TERRAIN_WATER_BUILD_FAILED;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const XgHost3dProjectedVertex *projected =
                &output.vertices[vertex];

            if (projected->projective_position) {
                ++build_diagnostics.projective_vertices;
                continue;
            }
            if (projected->projective_view_x < -0x8000 ||
                projected->projective_view_x > 0x7fff)
                ++build_diagnostics.projective_invalid_x;
            if (projected->projective_view_y < -0x8000 ||
                projected->projective_view_y > 0x7fff)
                ++build_diagnostics.projective_invalid_y;
            if (projected->projective_view_z <= 0 ||
                projected->projective_view_z > 0xffff)
                ++build_diagnostics.projective_invalid_z;
            else if ((uint32_t)projected->projective_view_z * 2u <=
                     projected->projective_distance)
                ++build_diagnostics.projective_invalid_near;
        }
        if ((int32_t)output.rtpt_flags < 0) {
            ++build_diagnostics.projection_rejects;
            continue;
        }
        if (!triangle_is_on_screen(output.vertices,
                                   source->screen_x_cull_margin)) {
            ++build_diagnostics.screen_rejects;
            if (!unculled_build) continue;
            temporal_only = true;
            temporal_cull_reasons |= XG_WORLD_TERRAIN_WATER_CULL_SCREEN;
        }
        if (!triangle_is_front_facing(output.vertices)) {
            ++build_diagnostics.backface_rejects;
            if (!unculled_build) continue;
            temporal_only = true;
            temporal_cull_reasons |= XG_WORLD_TERRAIN_WATER_CULL_BACKFACE;
        }
        for (vertex = 0u; vertex < 3u; ++vertex) {
            if (output.vertices[vertex].z > max_depth)
                max_depth = output.vertices[vertex].z;
        }
        if (max_depth >= 0x0f00u) {
            ++build_diagnostics.depth_rejects;
            if (!unculled_build) continue;
            temporal_only = true;
            temporal_cull_reasons |= XG_WORLD_TERRAIN_WATER_CULL_DEPTH;
        }

        clut_index = (uint16_t)output.depth_cue;
        if (clut_index >= 0x1000u) clut_index = 0x0fffu;
        clut_index = (clut_index >> 7u) +
            (((word & 0x0800u) != 0u) ? 32u : 0u);
        clut = source->cluts[clut_index];
        if (!tpage_is_valid(source->tpages[page_selector]) ||
            !clut_is_valid(clut))
            return XG_WORLD_TERRAIN_WATER_INVALID_SOURCE;
        candidate.primitive.material = source->material;
        decode_material(source->tpages[page_selector], clut,
                        &candidate.primitive.material);
        candidate.primitive.triangle_count = 1u;
        candidate.primitive.triangles[0].split_index = 0u;
        candidate.primitive.triangles[0].split_count = 1u;
        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t corner =
                triangle_corners[diagonal][triangle][vertex];
            const uint32_t grid_x =
                ((quadrant_index & 1u) != 0u ? 8u : 0u) + cell_x +
                (corner & 1u);
            const uint32_t grid_z =
                ((quadrant_index & 2u) != 0u ? 8u : 0u) + cell_z +
                (corner >> 1u);

            candidate.source_vertices[vertex] = input.vertices[vertex];
            candidate.projected_vertices[vertex] = output.vertices[vertex];
            candidate.primitive.triangles[0].vertices[vertex] =
                (XgRenderIrVertex){
                    .x = (int32_t)output.vertices[vertex].x *
                        INT32_C(65536),
                    .y = (int32_t)output.vertices[vertex].y *
                        INT32_C(65536),
                    .u = (int32_t)(uint8_t)uv[corner] * INT32_C(65536),
                    .v = (int32_t)(uint8_t)(uv[corner] >> 8u) *
                        INT32_C(65536),
                    .r = 0x80u,
                    .g = 0x80u,
                    .b = 0x80u,
                    .native_view_x =
                        output.vertices[vertex].native_view_x_16_16,
                     .native_view_y =
                         output.vertices[vertex].native_view_y_16_16,
                     .native_view_position =
                          output.vertices[vertex].native_view_position != 0u,
                    .projective_view_x =
                        output.vertices[vertex].projective_view_x,
                    .projective_view_y =
                        output.vertices[vertex].projective_view_y,
                    .projective_view_z =
                        output.vertices[vertex].projective_view_z,
                    .projective_offset_x =
                        output.vertices[vertex].projective_offset_x_16_16,
                    .projective_offset_y =
                        output.vertices[vertex].projective_offset_y_16_16,
                    .projective_native_offset_x = output.vertices[vertex]
                        .projective_native_offset_x_16_16,
                    .projective_native_offset_y = output.vertices[vertex]
                        .projective_native_offset_y_16_16,
                    .projective_distance =
                        output.vertices[vertex].projective_distance,
                    .projective_position =
                        output.vertices[vertex].projective_position != 0u,
                    .interpolation_group_id =
                        UINT32_C(0x63000000) | tile_identity,
                    .interpolation_vertex_id = grid_z * 17u + grid_x,
                    .interpolation_vertex_identity_valid = true,
                };
            if (output.vertices[vertex].projective_position)
                ++build_diagnostics.emitted_projective_vertices;
        }
        bucket = max_depth >> 4u;
        if (bucket >= TERRAIN_ORDERING_BUCKET_COUNT)
            bucket = TERRAIN_ORDERING_BUCKET_COUNT - 1u;
        candidate.projection_flags = output.rtpt_flags;
        candidate.ordering_bucket = bucket;
        candidate.allocation_ordinal = *record_count;
        candidate.source_primitive_index =
            ((((tile_index * 4u + quadrant_index) * 64u) +
              cell_z * 8u + cell_x) * 2u) + triangle;
        candidate.interpolation_primitive_id =
            xg_world_terrain_water_interpolation_primitive_id(
                (uint8_t)grid_tile, (uint8_t)terrain_id,
                (uint16_t)(((quadrant_index * 64u + cell_z * 8u + cell_x) *
                            2u) + triangle));
        candidate.ordering_predecessor_record = bucket_heads[bucket];
        candidate.ordering_predecessor_is_external =
            bucket_heads[bucket] == UINT32_MAX;
        candidate.temporal_only = temporal_only;
        candidate.temporal_cull_reasons = temporal_cull_reasons;
        candidate.depth_cue = output.depth_cue;
        candidate.encoded_clut = clut;
        candidate.encoded_tpage = source->tpages[page_selector];
        candidate.max_depth = max_depth;
        candidate.tile_index = (uint8_t)tile_index;
        candidate.quadrant_index = (uint8_t)quadrant_index;
        candidate.cell_x = (uint8_t)cell_x;
        candidate.cell_z = (uint8_t)cell_z;
        candidate.triangle_index = (uint8_t)triangle;
        candidate.uv_orientation = (uint8_t)((word >> 13u) & 3u);
        candidate.page_selector = (uint8_t)page_selector;
        candidate.animated_height = (word & 0x1000u) != 0u;
        candidate.alternate_diagonal = diagonal != 0u;
        candidate.alternate_clut_bank = (word & 0x0800u) != 0u;
        records[*record_count] = candidate;
        bucket_heads[bucket] = *record_count;
        ++*record_count;
        ++build_diagnostics.emitted_triangles;
    }
    return XG_WORLD_TERRAIN_WATER_OK;
}

static XgWorldTerrainWaterResult build_interpolation_anchors(
        const XgWorldTerrainWaterSource *source,
        const XgHost3dProjection *projection,
        const bool selected[XG_WORLD_TERRAIN_WATER_TILE_COUNT]
                           [XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT],
        int16_t first_x, int16_t first_z,
        XgWorldTerrainWaterAnchor *anchors, uint32_t anchor_capacity,
        uint32_t *out_anchor_count) {
    bool seen[XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY] = {false};
    uint32_t anchor_count = 0u;

    if (anchors == NULL || out_anchor_count == NULL)
        return XG_WORLD_TERRAIN_WATER_OK;
    for (uint32_t tile = 0u; tile < XG_WORLD_TERRAIN_WATER_TILE_COUNT;
         ++tile) {
        const uint32_t tile_row = tile / 5u;
        const uint32_t tile_column = tile % 5u;
        const int16_t tile_x = wrap_i16(
            (int32_t)first_x + (int32_t)tile_column * 0x800);
        const int16_t tile_z = wrap_i16(
            (int32_t)first_z - (int32_t)tile_row * 0x800);

        if (!source->tiles[tile].active) continue;
        for (uint32_t quadrant = 0u;
             quadrant < XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT; ++quadrant) {
            XgHost3dVector grid[XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT];
            const int16_t origin_x = wrap_i16(
                (int32_t)tile_x + ((quadrant & 1u) != 0u ? 0x400 : 0));
            const int16_t origin_z = wrap_i16(
                (int32_t)tile_z - ((quadrant & 2u) != 0u ? 0x400 : 0));

            if (!selected[tile][quadrant]) continue;
            make_grid(source, source->tiles[tile].samples[quadrant],
                      origin_x, origin_z, grid);
            for (uint32_t local_z = 0u;
                 local_z < XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE; ++local_z) {
                for (uint32_t local_x = 0u;
                     local_x < XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE; ++local_x) {
                    const uint32_t grid_index =
                        source->tiles[tile].grid_index;
                    const uint32_t grid_x =
                        ((quadrant & 1u) != 0u ? 8u : 0u) + local_x;
                    const uint32_t grid_z =
                        ((quadrant & 2u) != 0u ? 8u : 0u) + local_z;
                    const uint32_t global_x =
                        (grid_index % 9u) * 16u + grid_x;
                    const uint32_t global_z =
                        (grid_index / 9u) * 16u + grid_z;
                    const uint32_t global_vertex = global_z * 145u + global_x;
                    XgHost3dProject4Input input = {0};
                    XgHost3dRotTransPers4Output output;
                    const XgHost3dProjectedVertex *projected;
                    XgWorldTerrainWaterAnchor *anchor;

                    if (global_vertex >= XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY ||
                        seen[global_vertex])
                        continue;
                    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
                        input.vertices[vertex] =
                            grid[local_z * XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE +
                                 local_x];
                    input.projection = *projection;
                    if (!xg_host_3d_rot_trans_pers4(&input, &output))
                        return XG_WORLD_TERRAIN_WATER_BUILD_FAILED;
                    if ((int32_t)output.rtpt_flags < 0) continue;
                    if (anchor_count == anchor_capacity)
                        return XG_WORLD_TERRAIN_WATER_CAPACITY_EXCEEDED;
                    projected = &output.vertices[0];
                    anchor = &anchors[anchor_count++];
                    *anchor = (XgWorldTerrainWaterAnchor){
                        .material = source->material,
                        .vertex = {
                            .x = (int32_t)projected->x * INT32_C(65536),
                            .y = (int32_t)projected->y * INT32_C(65536),
                            .native_view_x = projected->native_view_x_16_16,
                            .native_view_y = projected->native_view_y_16_16,
                            .native_view_position =
                                projected->native_view_position != 0u,
                            .projective_view_x = projected->projective_view_x,
                            .projective_view_y = projected->projective_view_y,
                            .projective_view_z = projected->projective_view_z,
                            .projective_offset_x =
                                projected->projective_offset_x_16_16,
                            .projective_offset_y =
                                projected->projective_offset_y_16_16,
                            .projective_native_offset_x =
                                projected->projective_native_offset_x_16_16,
                            .projective_native_offset_y =
                                projected->projective_native_offset_y_16_16,
                            .projective_distance = projected->projective_distance,
                            .projective_position =
                                projected->projective_position != 0u,
                            .interpolation_group_id = UINT32_C(0x63000000),
                            .interpolation_vertex_id = global_vertex,
                            .interpolation_vertex_identity_valid = true,
                        },
                    };
                    seen[global_vertex] = true;
                }
            }
        }
    }
    *out_anchor_count = anchor_count;
    return XG_WORLD_TERRAIN_WATER_OK;
}

XgWorldTerrainWaterResult xg_world_terrain_water_append_temporal_tile_anchors(
        const XgWorldTerrainWaterTileSource *tiles, uint32_t tile_count,
        const XgWorldTerrainWaterSource *current,
        XgWorldTerrainWaterAnchor *anchors, uint32_t anchor_capacity,
        uint32_t *in_out_anchor_count) {
    bool seen[XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY] = {false};
    XgHost3dMatrix transform;
    XgHost3dProjection projection = {0};
    const uint32_t current_grid = current != NULL
        ? current->tiles[0].grid_index : UINT32_MAX;
    int16_t first_x;
    int16_t first_z;
    uint32_t anchor_count;

    if (tiles == NULL || tile_count == 0u || current == NULL || anchors == NULL ||
        in_out_anchor_count == NULL ||
        anchor_capacity < XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY ||
        *in_out_anchor_count > anchor_capacity || current_grid >= 81u ||
        current->projection_distance == 0u ||
        !material_template_is_valid(&current->material) ||
        !xg_host_3d_comp_matrix(
            &current->camera, &current->local, &transform))
        return XG_WORLD_TERRAIN_WATER_INVALID_ARGUMENT;
    anchor_count = *in_out_anchor_count;
    for (uint32_t index = 0u; index < anchor_count; ++index) {
        const XgRenderIrVertex *vertex = &anchors[index].vertex;

        if (vertex->interpolation_group_id == UINT32_C(0x63000000) &&
            vertex->interpolation_vertex_id <
                XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY)
            seen[vertex->interpolation_vertex_id] = true;
    }
    memcpy(projection.rotation, transform.rotation, sizeof(projection.rotation));
    memcpy(projection.translation, transform.translation,
           sizeof(projection.translation));
    projection.screen_offset_x = current->screen_offset_x;
    projection.screen_offset_y = current->screen_offset_y;
    projection.projection_distance = current->projection_distance;
    first_x = wrap_i16(-0x1000 -
        ((uint32_t)shift_right_floor(current->position_x, 12u) & 0x7ffu));
    first_z = wrap_i16(0x1000 +
        ((uint32_t)shift_right_floor(current->position_z, 12u) & 0x7ffu));

    for (uint32_t tile = 0u; tile < tile_count; ++tile) {
        const XgWorldTerrainWaterTileSource *tile_source =
            &tiles[tile];
        const uint32_t grid_index = tile_source->grid_index;
        const int32_t column_delta = (int32_t)(grid_index % 9u) -
            (int32_t)(current_grid % 9u);
        const int32_t row_delta = (int32_t)(grid_index / 9u) -
            (int32_t)(current_grid / 9u);

        if (!tile_source->active || !tile_source->has_data || grid_index >= 81u)
            continue;
        for (uint32_t quadrant = 0u;
             quadrant < XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT; ++quadrant) {
            XgHost3dVector grid[XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT];
            const int16_t origin_x = wrap_i16(
                (int32_t)first_x + column_delta * 0x800 +
                ((quadrant & 1u) != 0u ? 0x400 : 0));
            const int16_t origin_z = wrap_i16(
                (int32_t)first_z - row_delta * 0x800 -
                ((quadrant & 2u) != 0u ? 0x400 : 0));

            make_grid(current, tile_source->samples[quadrant],
                      origin_x, origin_z, grid);
            for (uint32_t local_z = 0u;
                 local_z < XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE; ++local_z) {
                for (uint32_t local_x = 0u;
                     local_x < XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE; ++local_x) {
                    const uint32_t grid_x =
                        ((quadrant & 1u) != 0u ? 8u : 0u) + local_x;
                    const uint32_t grid_z =
                        ((quadrant & 2u) != 0u ? 8u : 0u) + local_z;
                    const uint32_t global_x =
                        (grid_index % 9u) * 16u + grid_x;
                    const uint32_t global_z =
                        (grid_index / 9u) * 16u + grid_z;
                    const uint32_t global_vertex = global_z * 145u + global_x;
                    XgHost3dProject4Input input = {0};
                    XgHost3dRotTransPers4Output output;
                    const XgHost3dProjectedVertex *projected;

                    if (global_vertex >=
                            XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY ||
                        seen[global_vertex])
                        continue;
                    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
                        input.vertices[vertex] =
                            grid[local_z * XG_WORLD_TERRAIN_WATER_SAMPLE_SIDE +
                                 local_x];
                    input.projection = projection;
                    if (!xg_host_3d_rot_trans_pers4(&input, &output))
                        return XG_WORLD_TERRAIN_WATER_BUILD_FAILED;
                    if (anchor_count == anchor_capacity)
                        return XG_WORLD_TERRAIN_WATER_CAPACITY_EXCEEDED;
                    projected = &output.vertices[0];
                    anchors[anchor_count++] = (XgWorldTerrainWaterAnchor){
                        .material = current->material,
                        .vertex = {
                            .x = (int32_t)projected->x * INT32_C(65536),
                            .y = (int32_t)projected->y * INT32_C(65536),
                            .native_view_x = projected->native_view_x_16_16,
                            .native_view_y = projected->native_view_y_16_16,
                            .native_view_position =
                                projected->native_view_position != 0u,
                            .projective_view_x = projected->projective_view_x,
                            .projective_view_y = projected->projective_view_y,
                            .projective_view_z = projected->projective_view_z,
                            .projective_offset_x =
                                projected->projective_offset_x_16_16,
                            .projective_offset_y =
                                projected->projective_offset_y_16_16,
                            .projective_native_offset_x =
                                projected->projective_native_offset_x_16_16,
                            .projective_native_offset_y =
                                projected->projective_native_offset_y_16_16,
                            .projective_distance = projected->projective_distance,
                            .projective_position =
                                projected->projective_position != 0u,
                            .interpolation_group_id = UINT32_C(0x63000000),
                            .interpolation_vertex_id = global_vertex,
                            .interpolation_vertex_identity_valid = true,
                        },
                    };
                    seen[global_vertex] = true;
                }
            }
        }
    }
    *in_out_anchor_count = anchor_count;
    return XG_WORLD_TERRAIN_WATER_OK;
}

XgWorldTerrainWaterResult xg_world_terrain_water_append_temporal_anchors(
        const XgWorldTerrainWaterSource *previous,
        const XgWorldTerrainWaterSource *current,
        XgWorldTerrainWaterAnchor *anchors, uint32_t anchor_capacity,
        uint32_t *in_out_anchor_count) {
    if (previous == NULL) return XG_WORLD_TERRAIN_WATER_INVALID_ARGUMENT;
    return xg_world_terrain_water_append_temporal_tile_anchors(
        previous->tiles, XG_WORLD_TERRAIN_WATER_TILE_COUNT, current,
        anchors, anchor_capacity, in_out_anchor_count);
}

static XgWorldTerrainWaterResult xg_world_terrain_water_build_internal(
    const XgWorldTerrainWaterSource *source,
    XgWorldTerrainWaterRecord *records,
    uint32_t record_capacity,
    uint32_t *out_record_count,
    XgWorldTerrainWaterAnchor *anchors, uint32_t anchor_capacity,
    uint32_t *out_anchor_count) {
    XgHost3dMatrix transform;
    XgHost3dProjection projection = { 0 };
    uint32_t bucket_heads[TERRAIN_ORDERING_BUCKET_COUNT];
    bool selected[XG_WORLD_TERRAIN_WATER_TILE_COUNT]
                 [XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT] = { { false } };
    int16_t first_x;
    int16_t first_z;
    uint32_t record_count = 0u;
    uint32_t tile;
    uint32_t index;

    if (source == NULL || records == NULL || out_record_count == NULL ||
        ((anchors == NULL) != (out_anchor_count == NULL)))
        return XG_WORLD_TERRAIN_WATER_INVALID_ARGUMENT;
    memset(&build_diagnostics, 0, sizeof(build_diagnostics));
    *out_record_count = 0u;
    if (out_anchor_count != NULL) *out_anchor_count = 0u;
    if (record_capacity < (unculled_build
            ? XG_WORLD_TERRAIN_WATER_UNCULLED_CAPACITY
            : XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY))
        return XG_WORLD_TERRAIN_WATER_CAPACITY_EXCEEDED;
    if (source->projection_distance == 0u ||
        source->screen_x_cull_margin < 0 ||
        source->screen_x_cull_margin > (INT32_MAX - 320) / 2 ||
        !material_template_is_valid(&source->material))
        return XG_WORLD_TERRAIN_WATER_INVALID_SOURCE;
    for (tile = 0u; tile < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++tile) {
        uint32_t quadrant;

        if (!source->tiles[tile].active) continue;
        if (source->tiles[tile].terrain_id >= 256u ||
            source->tiles[tile].grid_index >= 81u)
            return XG_WORLD_TERRAIN_WATER_INVALID_SOURCE;
        for (quadrant = 0u; quadrant < 4u; ++quadrant) {
            uint32_t sample;

            if (!source->tiles[tile].has_data) continue;
            for (sample = 0u; sample < 81u; ++sample) {
                if (((source->tiles[tile].samples[quadrant][sample] >> 8u) &
                     7u) >= XG_WORLD_TERRAIN_WATER_PAGE_COUNT)
                    return XG_WORLD_TERRAIN_WATER_INVALID_SOURCE;
            }
        }
    }
    if (!xg_host_3d_comp_matrix(&source->camera, &source->local, &transform))
        return XG_WORLD_TERRAIN_WATER_BUILD_FAILED;
    memcpy(projection.rotation, transform.rotation, sizeof(projection.rotation));
    memcpy(projection.translation, transform.translation,
           sizeof(projection.translation));
    projection.screen_offset_x = source->screen_offset_x;
    projection.screen_offset_y = source->screen_offset_y;
    projection.projection_distance = source->projection_distance;
    if (!compute_fog(source->fog_mode, source->projection_distance,
                     &projection.depth_cue_a, &projection.depth_cue_b))
        return XG_WORLD_TERRAIN_WATER_INVALID_SOURCE;

    first_x = wrap_i16(-0x1000 -
        ((uint32_t)shift_right_floor(source->position_x, 12u) & 0x7ffu));
    first_z = wrap_i16(0x1000 +
        ((uint32_t)shift_right_floor(source->position_z, 12u) & 0x7ffu));
    for (tile = 0u; tile < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++tile) {
        uint32_t quadrant;

        if (!source->tiles[tile].active) continue;
        ++build_diagnostics.active_tiles;
        if (!temporal_coverage_enabled)
            select_tile_quadrants(source, tile, selected[tile]);
        else
            for (quadrant = 0u; quadrant < 4u; ++quadrant)
                selected[tile][quadrant] = true;
        for (quadrant = 0u; quadrant < 4u; ++quadrant) {
            if (selected[tile][quadrant])
                ++build_diagnostics.selected_quadrants;
            else
                ++build_diagnostics.rejected_quadrants;
            if (selected[tile][quadrant] && !source->tiles[tile].has_data)
                return XG_WORLD_TERRAIN_WATER_INVALID_SOURCE;
        }
    }

    for (index = 0u; index < TERRAIN_ORDERING_BUCKET_COUNT; ++index)
        bucket_heads[index] = UINT32_MAX;
    if (anchors != NULL) {
        const XgWorldTerrainWaterResult anchor_result =
            build_interpolation_anchors(
                source, &projection, selected, first_x, first_z,
                anchors, anchor_capacity, out_anchor_count);

        if (anchor_result != XG_WORLD_TERRAIN_WATER_OK)
            return anchor_result;
    }
    for (tile = 0u; tile < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++tile) {
        const uint32_t tile_row = tile / 5u;
        const uint32_t tile_column = tile % 5u;
        const int16_t tile_x = wrap_i16(
            (int32_t)first_x + (int32_t)tile_column * 0x800);
        const int16_t tile_z = wrap_i16(
            (int32_t)first_z - (int32_t)tile_row * 0x800);
        uint32_t quadrant;

        if (!source->tiles[tile].active) continue;
        for (quadrant = 0u; quadrant < 4u; ++quadrant) {
            XgHost3dVector grid[XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT];
            const int16_t origin_x = wrap_i16(
                (int32_t)tile_x + ((quadrant & 1u) != 0u ? 0x400 : 0));
            const int16_t origin_z = wrap_i16(
                (int32_t)tile_z - ((quadrant & 2u) != 0u ? 0x400 : 0));
            uint32_t cell_z;

            if (!selected[tile][quadrant]) continue;
            make_grid(source, source->tiles[tile].samples[quadrant],
                      origin_x, origin_z, grid);
            for (cell_z = 0u; cell_z < 8u; ++cell_z) {
                uint32_t cell_x;

                for (cell_x = 0u; cell_x < 8u; ++cell_x) {
                    XgWorldTerrainWaterResult result;

                    if (record_count >= (unculled_build
                            ? record_capacity : TERRAIN_PACKET_STOP_COUNT)) {
                        ++build_diagnostics.packet_limit_stops;
                        reconcile_shared_vertices(records, record_count);
                        *out_record_count = record_count;
                        return XG_WORLD_TERRAIN_WATER_OK;
                    }
                    result = emit_cell(
                        source, &projection, grid,
                        source->tiles[tile].samples[quadrant]
                            [cell_z * 9u + cell_x],
                        tile, quadrant, cell_x, cell_z, records,
                        &record_count, bucket_heads);
                    if (result != XG_WORLD_TERRAIN_WATER_OK) return result;
                }
            }
        }
    }
    reconcile_shared_vertices(records, record_count);
    *out_record_count = record_count;
    return XG_WORLD_TERRAIN_WATER_OK;
}

XgWorldTerrainWaterResult xg_world_terrain_water_build(
        const XgWorldTerrainWaterSource *source,
        XgWorldTerrainWaterRecord *records,
        uint32_t record_capacity,
        uint32_t *out_record_count) {
    return xg_world_terrain_water_build_internal(
        source, records, record_capacity, out_record_count,
        NULL, 0u, NULL);
}

XgWorldTerrainWaterResult xg_world_terrain_water_build_unculled(
        const XgWorldTerrainWaterSource *source,
        XgWorldTerrainWaterRecord *records,
        uint32_t record_capacity,
        uint32_t *out_record_count) {
    XgWorldTerrainWaterResult result;

    unculled_build = true;
    result = xg_world_terrain_water_build_internal(
        source, records, record_capacity, out_record_count,
        NULL, 0u, NULL);
    unculled_build = false;
    return result;
}

static bool native_ram_range_is_valid(uint32_t address, uint32_t size,
                                      uint32_t alignment) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint64_t physical = address & UINT32_C(0x1fffffff);

    return size != 0u && alignment != 0u &&
        (address & (alignment - 1u)) == 0u &&
        (segment == 0u || segment == UINT32_C(0x80000000) ||
         segment == UINT32_C(0xa0000000)) &&
        physical + size <= UINT64_C(0x200000) &&
        (uint64_t)address + size - 1u <= UINT32_MAX;
}

static void native_scratch_write_u16(
    XgWorldTerrainWaterNativeScratch *scratch, uint32_t offset,
    uint16_t value) {
    const uint32_t index = offset / 4u;
    const uint32_t shift = (offset & 2u) * 8u;
    const uint32_t mask = UINT32_C(0xffff) << shift;

    scratch->values[index] =
        (scratch->values[index] & ~mask) | ((uint32_t)value << shift);
    scratch->write_masks[index] |= mask;
}

static void native_scratch_write_u32(
    XgWorldTerrainWaterNativeScratch *scratch, uint32_t offset,
    uint32_t value) {
    const uint32_t index = offset / 4u;

    scratch->values[index] = value;
    scratch->write_masks[index] = UINT32_MAX;
}

static void native_scratch_write_matrix(
    XgWorldTerrainWaterNativeScratch *scratch, uint32_t offset,
    const XgHost3dMatrix *matrix) {
    native_scratch_write_u32(
        scratch, offset,
        (uint16_t)matrix->rotation[0][0] |
            ((uint32_t)(uint16_t)matrix->rotation[0][1] << 16u));
    native_scratch_write_u32(
        scratch, offset + 4u,
        (uint16_t)matrix->rotation[0][2] |
            ((uint32_t)(uint16_t)matrix->rotation[1][0] << 16u));
    native_scratch_write_u32(
        scratch, offset + 8u,
        (uint16_t)matrix->rotation[1][1] |
            ((uint32_t)(uint16_t)matrix->rotation[1][2] << 16u));
    native_scratch_write_u32(
        scratch, offset + 12u,
        (uint16_t)matrix->rotation[2][0] |
            ((uint32_t)(uint16_t)matrix->rotation[2][1] << 16u));
    native_scratch_write_u32(
        scratch, offset + 16u,
        (uint16_t)matrix->rotation[2][2] | ((uint32_t)matrix->pad << 16u));
    native_scratch_write_u32(
        scratch, offset + 20u, (uint32_t)matrix->translation[0]);
    native_scratch_write_u32(
        scratch, offset + 24u, (uint32_t)matrix->translation[1]);
    native_scratch_write_u32(
        scratch, offset + 28u, (uint32_t)matrix->translation[2]);
}

static bool native_build_scratch(
    const XgWorldTerrainWaterSource *source,
    XgWorldTerrainWaterNativeScratch *scratch) {
    XgHost3dMatrix composed;
    XgHost3dVector final_grid[XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT];
    int32_t x_origin_word;
    int32_t z_origin_word;
    int16_t first_x;
    int16_t first_z;
    uint32_t last_active = UINT32_MAX;
    uint32_t last_grid_tile = UINT32_MAX;
    uint32_t last_grid_quadrant = UINT32_MAX;
    uint32_t index;

    if (source == NULL || scratch == NULL ||
        !xg_host_3d_comp_matrix(&source->camera, &source->local, &composed))
        return false;
    memset(scratch, 0, sizeof(*scratch));

    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_CLUT_COUNT; ++index)
        native_scratch_write_u16(
            scratch, 0x288u + index * 2u, source->cluts[index]);
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_PAGE_COUNT - 1u; ++index)
        native_scratch_write_u16(
            scratch, 0x308u + index * 2u, source->tpages[index]);

    x_origin_word = -0x1000 -
        (int32_t)((uint32_t)shift_right_floor(source->position_x, 12u) &
                  0x7ffu);
    z_origin_word = -0x1000 -
        (int32_t)((uint32_t)shift_right_floor(source->position_z, 12u) &
                  0x7ffu);
    first_x = wrap_i16(x_origin_word);
    first_z = wrap_i16(-z_origin_word);
    native_scratch_write_u32(scratch, 0x318u, (uint32_t)x_origin_word);
    native_scratch_write_u32(scratch, 0x320u, (uint32_t)z_origin_word);
    native_scratch_write_u16(
        scratch, 0x328u, wrap_i16((int32_t)first_x + 0x2800));
    native_scratch_write_u16(
        scratch, 0x32cu, wrap_i16((int32_t)first_z - 0x2800));

    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++index) {
        bool selected[XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT];
        uint32_t quadrant;

        if (!source->tiles[index].active) continue;
        last_active = index;
        select_tile_quadrants(source, index, selected);
        for (quadrant = 0u;
             quadrant < XG_WORLD_TERRAIN_WATER_QUADRANT_COUNT; ++quadrant) {
            if (!selected[quadrant]) continue;
            last_grid_tile = index;
            last_grid_quadrant = quadrant;
        }
    }

    if (last_active != UINT32_MAX) {
        const uint32_t row = last_active / 5u;
        const uint32_t column = last_active % 5u;
        const int16_t tile_x = wrap_i16(
            (int32_t)first_x + (int32_t)column * 0x800);
        const int16_t tile_z = wrap_i16(
            (int32_t)first_z - (int32_t)row * 0x800);

        native_scratch_write_u16(
            scratch, 0x330u, wrap_i16((int32_t)tile_x + 0x400));
        native_scratch_write_u16(scratch, 0x334u, tile_z);
        native_scratch_write_u16(scratch, 0x338u, tile_x);
        native_scratch_write_u16(
            scratch, 0x33cu, wrap_i16((int32_t)tile_z - 0x400));
        native_scratch_write_u16(
            scratch, 0x340u, wrap_i16((int32_t)tile_x + 0x400));
        native_scratch_write_u16(
            scratch, 0x344u, wrap_i16((int32_t)tile_z - 0x400));
    }

    if (last_grid_tile != UINT32_MAX) {
        const uint32_t row = last_grid_tile / 5u;
        const uint32_t column = last_grid_tile % 5u;
        const int16_t origin_x = wrap_i16(
            (int32_t)first_x + (int32_t)column * 0x800 +
            ((last_grid_quadrant & 1u) != 0u ? 0x400 : 0));
        const int16_t origin_z = wrap_i16(
            (int32_t)first_z - (int32_t)row * 0x800 -
            ((last_grid_quadrant & 2u) != 0u ? 0x400 : 0));

        make_grid(source,
                  source->tiles[last_grid_tile].samples[last_grid_quadrant],
                  origin_x, origin_z, final_grid);
        for (index = 0u; index < XG_WORLD_TERRAIN_WATER_SAMPLE_COUNT;
             ++index) {
            native_scratch_write_u32(
                scratch, index * 8u,
                (uint16_t)final_grid[index].x |
                    ((uint32_t)(uint16_t)final_grid[index].y << 16u));
            native_scratch_write_u16(
                scratch, index * 8u + 4u,
                (uint16_t)final_grid[index].z);
        }
    }

    native_scratch_write_matrix(scratch, 0x350u, &source->local);
    native_scratch_write_matrix(scratch, 0x370u, &composed);
    return true;
}

static XgWorldTerrainWaterNativeResult native_capture_result(
    XgWorldTerrainWaterCaptureResult result) {
    switch (result) {
    case XG_WORLD_TERRAIN_WATER_CAPTURE_OK:
        return XG_WORLD_TERRAIN_WATER_NATIVE_OK;
    case XG_WORLD_TERRAIN_WATER_CAPTURE_UNAUTHENTICATED:
        return XG_WORLD_TERRAIN_WATER_NATIVE_UNAUTHENTICATED;
    case XG_WORLD_TERRAIN_WATER_CAPTURE_READ_FAILED:
        return XG_WORLD_TERRAIN_WATER_NATIVE_READ_FAILED;
    case XG_WORLD_TERRAIN_WATER_CAPTURE_SOURCE_MISMATCH:
        return XG_WORLD_TERRAIN_WATER_NATIVE_SOURCE_MISMATCH;
    case XG_WORLD_TERRAIN_WATER_CAPTURE_FORBIDDEN_RANGE:
        return XG_WORLD_TERRAIN_WATER_NATIVE_FORBIDDEN_RANGE;
    case XG_WORLD_TERRAIN_WATER_CAPTURE_INVALID_ARGUMENT:
    default:
        return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_ARGUMENT;
    }
}

XgWorldTerrainWaterNativeResult xg_world_terrain_water_native_prepare(
    const XgWorldTerrainWaterNativeRequest *request,
    const XgWorldTerrainWaterAuthenticatedReader *reader,
    XgWorldTerrainWaterRecord *records, uint32_t record_capacity,
    XgWorldTerrainWaterAnchor *anchors, uint32_t anchor_capacity,
    XgWorldTerrainWaterSource *out_source,
    XgWorldTerrainWaterNativePreparation *out_preparation) {
    enum {
        CONTEXT_OT_OFFSET = 0x70,
        CONTEXT_PACKET_OFFSET = 0x74,
    };
    XgWorldTerrainWaterNativePreparation preparation = { 0 };
    XgWorldTerrainWaterCapture capture;
    uint32_t last_record_by_bucket[
        XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT];
    uint32_t context;
    uint32_t context_ot;
    uint32_t context_packets;
    uint32_t index;
    XgWorldTerrainWaterCaptureResult capture_result;
    XgWorldTerrainWaterResult build_result;

    if (out_preparation == NULL)
        return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_ARGUMENT;
    memset(out_preparation, 0, sizeof(*out_preparation));
    if (request == NULL || reader == NULL || records == NULL || anchors == NULL ||
        out_source == NULL ||
        reader->read_u16 == NULL || reader->read_u32 == NULL ||
        record_capacity < XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY ||
        anchor_capacity < XG_WORLD_TERRAIN_WATER_ANCHOR_CAPACITY ||
        request->entry_pc != XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC ||
        request->position_address !=
            XG_WORLD_TERRAIN_WATER_NATIVE_POSITION_ADDRESS ||
        !native_ram_range_is_valid(request->ot_base, 4u, 4u) ||
        !native_ram_range_is_valid(request->packet_base, 4u, 4u))
        return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_ARGUMENT;

    capture_result = xg_world_terrain_water_source_capture(
        &request->capture, reader, &capture);
    if (capture_result != XG_WORLD_TERRAIN_WATER_CAPTURE_OK)
        return native_capture_result(capture_result);
    if (!capture.authenticated || !capture.sealed || !reader->authenticated ||
        request->capture.authentication_generation == 0u ||
        reader->authentication_generation !=
            request->capture.authentication_generation)
        return XG_WORLD_TERRAIN_WATER_NATIVE_UNAUTHENTICATED;

    if (!reader->read_u32(reader->context,
                          XG_WORLD_TERRAIN_WATER_NATIVE_CONTEXT_ADDRESS,
                          &context))
        return XG_WORLD_TERRAIN_WATER_NATIVE_READ_FAILED;
    if (!native_ram_range_is_valid(
            context, CONTEXT_PACKET_OFFSET + 4u, 4u))
        return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_OUTPUT;
    if (!reader->read_u32(reader->context, context + CONTEXT_OT_OFFSET,
                          &context_ot) ||
        !reader->read_u32(reader->context, context + CONTEXT_PACKET_OFFSET,
                          &context_packets))
        return XG_WORLD_TERRAIN_WATER_NATIVE_READ_FAILED;
    if (context_ot != request->ot_base ||
        context_packets != request->packet_base)
        return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_OUTPUT;

    build_result = xg_world_terrain_water_build_internal(
        &capture.source, records, record_capacity, &preparation.record_count,
        anchors, anchor_capacity, &preparation.anchor_count);
    if (build_result != XG_WORLD_TERRAIN_WATER_OK)
        return XG_WORLD_TERRAIN_WATER_NATIVE_BUILD_FAILED;
    if (preparation.record_count > XG_WORLD_TERRAIN_WATER_RECORD_CAPACITY ||
        !native_ram_range_is_valid(
            request->ot_base,
            XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT * 4u, 4u) ||
        !native_ram_range_is_valid(
            request->packet_base,
            preparation.record_count == 0u
                ? 4u
                : preparation.record_count *
                    XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE,
            4u))
        return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_OUTPUT;

    for (index = 0u;
         index < XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT; ++index)
        last_record_by_bucket[index] = UINT32_MAX;
    for (index = 0u; index < preparation.record_count; ++index) {
        const XgWorldTerrainWaterRecord *record = &records[index];
        const uint32_t bucket = record->ordering_bucket;
        uint32_t predecessor;

        if (record->allocation_ordinal != index ||
            bucket >= XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT ||
            record->primitive.triangle_count != 1u ||
            record->primitive.triangles[0].split_index != 0u ||
            record->primitive.triangles[0].split_count != 1u ||
            record->primitive.material.shading != XG_RENDER_IR_SHADING_FLAT ||
            !record->primitive.material.textured ||
            record->primitive.material.raw_texture ||
            record->primitive.material.semi_transparent ||
            record->primitive.triangles[0].vertices[0].r != 0x80u ||
            record->primitive.triangles[0].vertices[0].g != 0x80u ||
            record->primitive.triangles[0].vertices[0].b != 0x80u)
            return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_OUTPUT;
        predecessor = last_record_by_bucket[bucket];
        if ((predecessor == UINT32_MAX) !=
                record->ordering_predecessor_is_external ||
            record->ordering_predecessor_record != predecessor)
            return XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_OUTPUT;
        last_record_by_bucket[bucket] = index;
    }
    if (!native_build_scratch(&capture.source, &preparation.scratch))
        return XG_WORLD_TERRAIN_WATER_NATIVE_BUILD_FAILED;

    preparation.authentication_generation =
        request->capture.authentication_generation;
    preparation.ot_base = request->ot_base;
    preparation.packet_base = request->packet_base;
    preparation.position_address = request->position_address;
    preparation.final_count = preparation.record_count;
    preparation.continuation_pc = request->capture.caller_return;
    preparation.authenticated_read_count =
        capture.authenticated_read_count + 3u;
    preparation.authenticated_read_bytes =
        capture.authenticated_read_bytes + 12u;
    preparation.captured_resource_count = capture.captured_resource_count;
    if (preparation.authenticated_read_count >
            XG_WORLD_TERRAIN_WATER_NATIVE_MAX_AUTHENTICATED_READS ||
        preparation.authenticated_read_bytes >
            XG_WORLD_TERRAIN_WATER_NATIVE_MAX_AUTHENTICATED_BYTES)
        return XG_WORLD_TERRAIN_WATER_NATIVE_FORBIDDEN_RANGE;
    preparation.authenticated = true;
    preparation.sealed = true;
    *out_source = capture.source;
    *out_preparation = preparation;
    return XG_WORLD_TERRAIN_WATER_NATIVE_OK;
}
