#include "xg_world_entity_shadows.h"

#include "xg_render_quad_builder.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(XgWorldEntityShadowPendingEntry) == 8u,
               "world shadow list entries must have the guest stride");
_Static_assert(offsetof(XgWorldEntityShadowPendingEntry, type) == 2u,
               "world shadow type must match the guest list");
_Static_assert(offsetof(XgWorldEntityShadowPendingEntry, z) == 4u,
               "world shadow z must match the guest list");
_Static_assert(XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE ==
                   XG_WORLD_ENTITY_SHADOW_PACKET_WORD_COUNT * 4u,
               "world shadow packet words must match the guest stride");
_Static_assert(XG_WORLD_ENTITY_SHADOW_PACKET_BUFFER_BYTES ==
                   XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY *
                       XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE,
               "world shadow packet buffer must contain every list slot");

static int32_t wrap_i32(uint32_t value) {
  if (value <= INT32_MAX)
    return (int32_t)value;
  return -1 - (int32_t)(UINT32_MAX - value);
}

static int32_t shift_right_floor_i32(int32_t value, unsigned bits) {
  uint32_t magnitude;

  if (value >= 0)
    return value / (int32_t)(UINT32_C(1) << bits);
  magnitude = (uint32_t)(-(value + 1)) + 1u;
  return -(int32_t)((magnitude + ((UINT32_C(1) << bits) - 1u)) >> bits);
}

static int16_t wrap_i16(int32_t value) {
  const uint16_t low = (uint16_t)(uint32_t)value;

  if (low <= INT16_MAX)
    return (int16_t)low;
  return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static void rotate_matrix_y(XgHost3dMatrix *matrix, int16_t sine,
                            int16_t cosine) {
  int16_t row0[3];
  int16_t row2[3];
  uint32_t column;

  memcpy(row0, matrix->rotation[0], sizeof(row0));
  memcpy(row2, matrix->rotation[2], sizeof(row2));
  for (column = 0u; column < 3u; ++column) {
    const uint32_t upper =
        (uint32_t)(int32_t)cosine * (uint32_t)(int32_t)row0[column] +
        (uint32_t)(int32_t)sine * (uint32_t)(int32_t)row2[column];
    const uint32_t lower =
        (uint32_t)(int32_t)cosine * (uint32_t)(int32_t)row2[column] -
        (uint32_t)(int32_t)sine * (uint32_t)(int32_t)row0[column];

    matrix->rotation[0][column] =
        wrap_i16(shift_right_floor_i32(wrap_i32(upper), 12u));
    matrix->rotation[2][column] =
        wrap_i16(shift_right_floor_i32(wrap_i32(lower), 12u));
  }
}

static bool multiply_rotation(const XgHost3dMatrix *left,
                              const XgHost3dMatrix *right,
                              XgHost3dMatrix *output) {
  XgHost3dMatrix result = {0};
  uint32_t column;

  for (column = 0u; column < 3u; ++column) {
    const XgHost3dVector input = {
        right->rotation[0][column],
        right->rotation[1][column],
        right->rotation[2][column],
        0u,
    };
    XgHost3dVector transformed;
    uint32_t flags;

    if (!xg_host_3d_rtir(left, &input, &transformed, &flags))
      return false;
    result.rotation[0][column] = transformed.x;
    result.rotation[1][column] = transformed.y;
    result.rotation[2][column] = transformed.z;
  }
  *output = result;
  return true;
}

static bool make_terrain_matrix(const XgWorldEntityShadowSourceEntry *entry,
                                XgHost3dMatrix *matrix) {
  const XgHost3dLongVector world_z = {0, 0, 0x1000};
  XgHost3dLongVector tangent_x;
  XgHost3dLongVector tangent_z;
  XgHost3dLongVector normalized_x;
  XgHost3dLongVector normalized_z;
  uint32_t flags;

  if (!xg_host_3d_op12(&world_z, &entry->terrain_normal, &tangent_x, &flags) ||
      !xg_host_3d_vector_normal(&tangent_x, &normalized_x) ||
      !xg_host_3d_op12(&entry->terrain_normal, &normalized_x, &tangent_z,
                       &flags) ||
      !xg_host_3d_vector_normal(&tangent_z, &normalized_z))
    return false;

  memset(matrix, 0, sizeof(*matrix));
  matrix->rotation[0][0] = wrap_i16(normalized_x.x);
  matrix->rotation[0][1] = wrap_i16(normalized_x.y);
  matrix->rotation[0][2] = wrap_i16(normalized_x.z);
  matrix->rotation[1][0] = wrap_i16(entry->terrain_normal.x);
  matrix->rotation[1][1] = wrap_i16(entry->terrain_normal.y);
  matrix->rotation[1][2] = wrap_i16(entry->terrain_normal.z);
  matrix->rotation[2][0] = wrap_i16(normalized_z.x);
  matrix->rotation[2][1] = wrap_i16(normalized_z.y);
  matrix->rotation[2][2] = wrap_i16(normalized_z.z);
  return true;
}

static bool material_is_valid(const XgRenderIrMaterialState *material) {
  return material->tpage == 0x001eu && material->texture_page_x == 14u &&
         material->texture_page_y == 1u && material->clut_x == 288u &&
         material->clut_y == 510u &&
         material->texture_depth == XG_RENDER_IR_TEXTURE_4_BIT &&
         material->texture_window_mask_x == 0u &&
         material->texture_window_mask_y == 0u &&
         material->texture_window_offset_x == 0u &&
         material->texture_window_offset_y == 0u &&
         material->shading == XG_RENDER_IR_SHADING_FLAT && material->textured &&
         !material->raw_texture && material->semi_transparent &&
         material->blend_mode == XG_RENDER_IR_BLEND_AVERAGE;
}

XgWorldEntityShadowsResult
xg_world_entity_shadows_enqueue(const XgWorldEntityShadowPendingList *before,
                                int16_t type, const int32_t position_fixed[3],
                                XgWorldEntityShadowPendingList *after) {
  uint32_t index;

  if (before == NULL || position_fixed == NULL || after == NULL)
    return XG_WORLD_ENTITY_SHADOWS_INVALID_ARGUMENT;
  if (before->count >= XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY)
    return XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE;

  index = before->count;
  *after = *before;
  after->entries[index].x =
      wrap_i16(shift_right_floor_i32(position_fixed[0], 12u));
  after->entries[index].type = type;
  after->entries[index].z =
      wrap_i16(shift_right_floor_i32(position_fixed[2], 12u));
  after->count = (index + 1u) & 0x0fu;
  return XG_WORLD_ENTITY_SHADOWS_OK;
}

XgWorldEntityShadowsResult xg_world_entity_shadows_build(
    const XgWorldEntityShadowsSource *source,
    XgWorldEntityShadowRecord *records, uint32_t record_capacity,
    uint32_t *out_record_count,
    XgWorldEntityShadowsSideEffects *out_side_effects) {
  static const XgHost3dVector shadow_vertices[4] = {
      {-16, 0, 16, 0u},
      {16, 0, 16, 0u},
      {-16, 0, -16, 0u},
      {16, 0, -16, 0u},
  };
  static const uint16_t shadow_uv[4] = {
      0xf080u,
      0xf08fu,
      0xff80u,
      0xff8fu,
  };
  uint32_t index;
  uint32_t accepted_count = 0u;

  if (source == NULL || out_record_count == NULL || out_side_effects == NULL ||
      (source != NULL && source->pending.count != 0u && records == NULL))
    return XG_WORLD_ENTITY_SHADOWS_INVALID_ARGUMENT;
  *out_record_count = 0u;
  memset(out_side_effects, 0, sizeof(*out_side_effects));
  if (source->pending.count > XG_WORLD_ENTITY_SHADOW_RENDERABLE_CAPACITY ||
      source->projection_distance == 0u ||
      !material_is_valid(&source->material))
    return XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE;
  if (record_capacity < source->pending.count)
    return XG_WORLD_ENTITY_SHADOWS_CAPACITY_EXCEEDED;
  if (source->pending.count != 0u)
    memset(records, 0, sizeof(*records) * source->pending.count);

  for (index = 0u; index < source->pending.count; ++index) {
    const XgWorldEntityShadowSourceEntry *entry = &source->entries[index];
    XgWorldEntityShadowRecord candidate = {0};
    XgHost3dMatrix local;
    XgHost3dMatrix type2_billboard;
    XgHost3dMatrix multiplied;
    XgHost3dMatrix transformed;
    XgHost3dLongVector position;
    XgHost3dLongVector translation;
    XgHost3dProject4Input projection = {0};
    XgHost3dRotTransPers4Output projected;
    XgRenderQuadSource quad = {0};
    uint32_t flags;
    uint32_t vertex;
    uint16_t minimum_depth;

    if (entry->pending.type < 0 || entry->pending.type > 2 ||
        entry->pending.x != source->pending.entries[index].x ||
        entry->pending.type != source->pending.entries[index].type ||
        entry->pending.z != source->pending.entries[index].z)
      return XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE;
    if (!make_terrain_matrix(entry, &local))
      return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;

    if (entry->pending.type == 1) {
      const XgHost3dLongVector scale = {0x1800, 0x1800, 0x1800};

      if (!xg_host_3d_scale_matrix(&local, &scale))
        return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    } else if (entry->pending.type == 2) {
      const XgHost3dLongVector scale = {0x1800, 0x1000, 0x4800};

      if (!source->has_type2)
        return XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE;
      type2_billboard = source->billboard;
      rotate_matrix_y(&type2_billboard, source->type2_sine,
                      source->type2_cosine);
      if (!multiply_rotation(&local, &type2_billboard, &multiplied))
        return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
      local = multiplied;
      if (!xg_host_3d_scale_matrix(&local, &scale))
        return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    }

    candidate.local_transform = local;
    if (!multiply_rotation(&source->camera, &local, &transformed))
      return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    position.x = shift_right_floor_i32(
        wrap_i32((uint32_t)((int32_t)entry->pending.x * 0x1000) -
                 (uint32_t)source->camera_origin_x_fixed),
        12u);
    position.y = shift_right_floor_i32(entry->terrain_height_fixed, 12u);
    position.z = shift_right_floor_i32(
        wrap_i32((uint32_t)source->camera_origin_z_fixed -
                 (uint32_t)((int32_t)entry->pending.z * 0x1000)),
        12u);
    if (!xg_host_3d_rt(&source->camera, &position, &translation, &flags))
      return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    transformed.translation[0] = translation.x;
    transformed.translation[1] = translation.y;
    transformed.translation[2] = translation.z;

    memcpy(projection.vertices, shadow_vertices, sizeof(projection.vertices));
    memcpy(projection.projection.rotation, transformed.rotation,
           sizeof(transformed.rotation));
    memcpy(projection.projection.translation, transformed.translation,
           sizeof(transformed.translation));
    projection.projection.screen_offset_x = source->screen_offset_x;
    projection.projection.screen_offset_y = source->screen_offset_y;
    projection.projection.projection_distance = source->projection_distance;
    if (!xg_host_3d_rot_trans_pers4(&projection, &projected))
      return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;

    candidate.source_index = index;
    candidate.transform = transformed;
    candidate.packet_offset =
        accepted_count * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE;
    candidate.material_word = UINT32_C(0x2e484040);
    candidate.clut = 0x7f92u;
    candidate.tpage = 0x001eu;
    candidate.type = entry->pending.type;
    candidate.rtpt_flags = projected.rtpt_flags;
    memcpy(candidate.uv, shadow_uv, sizeof(candidate.uv));
    memcpy(candidate.vertices, projected.vertices,
           sizeof(candidate.vertices));
    candidate.rtps_flags = projected.rtps_flags;
    quad.material = source->material;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
      quad.vertices[vertex] = (XgRenderQuadSourceVertex){
          .u = (uint8_t)shadow_uv[vertex],
          .v = (uint8_t)(shadow_uv[vertex] >> 8u),
          .red = 0x40u,
          .green = 0x40u,
          .blue = 0x48u,
      };
      xg_render_quad_set_projected_position(
          &quad.vertices[vertex], &projected.vertices[vertex]);
    }
    if (xg_render_quad_build_primitive(&quad, &candidate.primitive) !=
        XG_RENDER_QUAD_BUILDER_OK)
      return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    if ((int32_t)projected.rtpt_flags < 0) {
      candidate.cull = XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS;
      records[index] = candidate;
      continue;
    }

    candidate.packet_word_write_mask =
        XG_WORLD_ENTITY_SHADOW_PACKET_GEOMETRY_WRITE_MASK;
    minimum_depth = projected.vertices[0].z;
    for (vertex = 1u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
      if (projected.vertices[vertex].z < minimum_depth)
        minimum_depth = projected.vertices[vertex].z;
    }
    candidate.minimum_depth = minimum_depth;
    if (minimum_depth >= 0x1000u) {
      candidate.cull = XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH;
      records[index] = candidate;
      continue;
    }
    candidate.ordering_bucket = minimum_depth >> 4u;

    candidate.accepted = true;
    candidate.ordering_table_written = true;
    candidate.packet_word_write_mask =
        XG_WORLD_ENTITY_SHADOW_PACKET_ACCEPTED_WRITE_MASK;
    candidate.cull = XG_WORLD_ENTITY_SHADOW_CULL_NONE;
    ++accepted_count;
    ++out_side_effects->ordering_insertions;
    records[index] = candidate;
  }

  out_side_effects->pending_count_before = source->pending.count;
  out_side_effects->pending_count_after = 0u;
  out_side_effects->packet_slots_advanced = accepted_count;
  out_side_effects->packet_bytes_advanced =
      accepted_count * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE;
  out_side_effects->pending_count_written = source->pending.count != 0u;
  out_side_effects->pending_entries_unchanged = true;
  *out_record_count = source->pending.count;
  return XG_WORLD_ENTITY_SHADOWS_OK;
}
