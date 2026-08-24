#include "xg_world_entity_shadows_source_capture.h"

#include <limits.h>
#include <string.h>

#define SHADOW_PRODUCER_CALLSITE XG_WORLD_ENTITY_SHADOWS_PRODUCER_CALLSITE
#define SHADOW_PENDING_COUNT XG_WORLD_ENTITY_SHADOWS_PENDING_COUNT_ADDRESS
#define SHADOW_PENDING_LIST_POINTER UINT32_C(0x8009d30c)
#define SHADOW_CAMERA_MATRIX UINT32_C(0x8009c808)
#define SHADOW_CAMERA_ORIGIN_X UINT32_C(0x8009be28)
#define SHADOW_CAMERA_ORIGIN_Z UINT32_C(0x8009be30)
#define SHADOW_BILLBOARD_MATRIX UINT32_C(0x8009a180)
#define SHADOW_TYPE2_ANGLE UINT32_C(0x8006ee66)
#define SHADOW_TRIG_TABLE UINT32_C(0x800523f0)
#define SHADOW_TERRAIN_POINTER_TABLE UINT32_C(0x8009c184)
#define SHADOW_TERRAIN_MAP_WIDTH UINT32_C(0x8009d160)
#define SHADOW_TERRAIN_SELECT_0 UINT32_C(0x8009b244)
#define SHADOW_TERRAIN_SELECT_1 UINT32_C(0x8009b24c)
#define SHADOW_TERRAIN_SELECT_2 UINT32_C(0x8009b254)
#define SHADOW_TERRAIN_SELECT_3 UINT32_C(0x8009b25c)

enum {
  SHADOW_TERRAIN_POINTER_COUNT = 0x100u,
  SHADOW_PENDING_LIST_BYTES = 0x80u,
  SHADOW_TERRAIN_CHUNK_BYTES = 0x710u,
};

typedef struct ShadowCaptureAccess {
  const XgWorldEntityShadowsAuthenticatedReader *reader;
  uint32_t pending_list_address;
  uint32_t trig_address;
  uint32_t read_count;
  uint32_t read_bytes;
} ShadowCaptureAccess;

typedef struct ShadowTerrainGlobals {
  int32_t map_width;
  int32_t select[4];
} ShadowTerrainGlobals;

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

static int32_t shift_right_trunc_i32(int32_t value, unsigned bits) {
  if (value < 0)
    value = wrap_i32((uint32_t)value + ((UINT32_C(1) << bits) - 1u));
  return shift_right_floor_i32(value, bits);
}

static int16_t low_s16(uint32_t value) { return (int16_t)(uint16_t)value; }

static int8_t bits_s8(uint8_t value) {
  if (value <= INT8_MAX)
    return (int8_t)value;
  return (int8_t)(-1 - (int32_t)(UINT8_MAX - value));
}

static bool address_in_aligned_range(uint32_t address, uint32_t start,
                                     uint32_t size) {
  return address >= start && address <= start + size - 4u &&
         ((address - start) & 3u) == 0u;
}

static bool pending_word_is_allowed(const ShadowCaptureAccess *access,
                                    uint32_t address) {
  if (access->pending_list_address == 0u ||
      address < access->pending_list_address ||
      address > access->pending_list_address + SHADOW_PENDING_LIST_BYTES - 4u)
    return false;
  return ((address - access->pending_list_address) & 7u) == 0u;
}

static bool pending_z_is_allowed(const ShadowCaptureAccess *access,
                                 uint32_t address) {
  if (access->pending_list_address == 0u ||
      address < access->pending_list_address ||
      address > access->pending_list_address + SHADOW_PENDING_LIST_BYTES - 2u)
    return false;
  return ((address - access->pending_list_address) & 7u) == 4u;
}

static bool u32_read_is_allowed(const ShadowCaptureAccess *access,
                                uint32_t address) {
  return address == SHADOW_PENDING_COUNT ||
         address == SHADOW_PENDING_LIST_POINTER ||
         address == SHADOW_CAMERA_ORIGIN_X ||
         address == SHADOW_CAMERA_ORIGIN_Z ||
         address == SHADOW_TERRAIN_MAP_WIDTH ||
         address == SHADOW_TERRAIN_SELECT_0 ||
         address == SHADOW_TERRAIN_SELECT_1 ||
         address == SHADOW_TERRAIN_SELECT_2 ||
         address == SHADOW_TERRAIN_SELECT_3 ||
         address == access->trig_address ||
         address_in_aligned_range(address, SHADOW_CAMERA_MATRIX, 0x20u) ||
         address_in_aligned_range(address, SHADOW_BILLBOARD_MATRIX, 0x20u) ||
         address_in_aligned_range(address, SHADOW_TERRAIN_POINTER_TABLE,
                                  SHADOW_TERRAIN_POINTER_COUNT * 4u) ||
         pending_word_is_allowed(access, address);
}

static XgWorldEntityShadowsCaptureResult
read_u32(ShadowCaptureAccess *access, uint32_t address, uint32_t *out_value) {
  if (access->read_count >= XG_WORLD_ENTITY_SHADOWS_MAX_AUTHENTICATED_READS ||
      !u32_read_is_allowed(access, address))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_FORBIDDEN_RANGE;
  if (!access->reader->read_u32(access->reader->context, address, out_value))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_READ_FAILED;
  ++access->read_count;
  access->read_bytes += 4u;
  return XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK;
}

static XgWorldEntityShadowsCaptureResult
read_u16(ShadowCaptureAccess *access, uint32_t address, uint16_t *out_value) {
  if (access->read_count >= XG_WORLD_ENTITY_SHADOWS_MAX_AUTHENTICATED_READS ||
      (address != SHADOW_TYPE2_ANGLE && !pending_z_is_allowed(access, address)))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_FORBIDDEN_RANGE;
  if (!access->reader->read_u16(access->reader->context, address, out_value))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_READ_FAILED;
  ++access->read_count;
  access->read_bytes += 2u;
  return XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK;
}

static XgWorldEntityShadowsCaptureResult
read_terrain_u8(ShadowCaptureAccess *access, uint32_t chunk_address,
                uint32_t address, uint8_t *out_value) {
  if (access->read_count >= XG_WORLD_ENTITY_SHADOWS_MAX_AUTHENTICATED_READS ||
      address < chunk_address ||
      address >= chunk_address + SHADOW_TERRAIN_CHUNK_BYTES)
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_FORBIDDEN_RANGE;
  if (!access->reader->read_u8(access->reader->context, address, out_value))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_READ_FAILED;
  ++access->read_count;
  ++access->read_bytes;
  return XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK;
}

static void parse_matrix_word(XgHost3dMatrix *matrix, uint32_t index,
                              uint32_t word) {
  if (index == 0u) {
    matrix->rotation[0][0] = low_s16(word);
    matrix->rotation[0][1] = low_s16(word >> 16u);
  } else if (index == 1u) {
    matrix->rotation[0][2] = low_s16(word);
    matrix->rotation[1][0] = low_s16(word >> 16u);
  } else if (index == 2u) {
    matrix->rotation[1][1] = low_s16(word);
    matrix->rotation[1][2] = low_s16(word >> 16u);
  } else if (index == 3u) {
    matrix->rotation[2][0] = low_s16(word);
    matrix->rotation[2][1] = low_s16(word >> 16u);
  } else if (index == 4u) {
    matrix->rotation[2][2] = low_s16(word);
    matrix->pad = (uint16_t)(word >> 16u);
  } else {
    matrix->translation[index - 5u] = wrap_i32(word);
  }
}

static bool raster_is_valid(const XgWorldEntityShadowsRasterState *raster) {
  return raster->draw_area_left <= raster->draw_area_right &&
         raster->draw_area_top <= raster->draw_area_bottom &&
         raster->draw_area_right <= 1023u &&
         raster->draw_area_bottom <= 1023u && raster->draw_offset_x >= -1024 &&
         raster->draw_offset_x <= 1023 && raster->draw_offset_y >= -1024 &&
         raster->draw_offset_y <= 1023;
}

static void apply_material(const XgWorldEntityShadowsRasterState *raster,
                           XgRenderIrMaterialState *material) {
  *material = (XgRenderIrMaterialState){
      .tpage = 0x001eu,
      .texture_page_x = 14u,
      .texture_page_y = 1u,
      .clut_x = 288u,
      .clut_y = 510u,
      .draw_area_left = raster->draw_area_left,
      .draw_area_top = raster->draw_area_top,
      .draw_area_right = raster->draw_area_right,
      .draw_area_bottom = raster->draw_area_bottom,
      .draw_offset_x = raster->draw_offset_x,
      .draw_offset_y = raster->draw_offset_y,
      .texture_depth = XG_RENDER_IR_TEXTURE_4_BIT,
      .shading = XG_RENDER_IR_SHADING_FLAT,
      .textured = true,
      .semi_transparent = true,
      .blend_mode = XG_RENDER_IR_BLEND_AVERAGE,
      .dither = raster->dither,
      .mask_set = raster->mask_set,
      .mask_check = raster->mask_check,
  };
}

static bool branch_is_negative(int32_t left0, int32_t right0, int32_t left1,
                               int32_t right1) {
  const uint32_t sum =
      (uint32_t)left0 * (uint32_t)right0 + (uint32_t)left1 * (uint32_t)right1;

  return wrap_i32(sum) < 0;
}

static bool compute_terrain_normal(const uint8_t heights[5],
                                   const ShadowTerrainGlobals *globals,
                                   int32_t x_fixed, int32_t z_fixed,
                                   XgHost3dLongVector *normal) {
  const int32_t sample_x =
      (int32_t)((uint32_t)shift_right_trunc_i32(x_fixed, 3u) & 0xffffu);
  const int32_t sample_z =
      (int32_t)((uint32_t)shift_right_trunc_i32(z_fixed, 3u) & 0xffffu);
  const int32_t h0 = bits_s8(heights[0]);
  const int32_t h4 = bits_s8(heights[2]);
  const int32_t h24 = bits_s8(heights[3]);
  const int32_t h28 = bits_s8(heights[4]);
  XgHost3dLongVector first;
  XgHost3dLongVector second;
  XgHost3dLongVector cross;
  uint32_t flags;

  if ((heights[1] & 0x80u) == 0u) {
    if (branch_is_negative(sample_x - 0x10000, globals->select[0], -sample_z,
                           globals->select[1])) {
      first = (XgHost3dLongVector){-16, h0 - h4, 0};
      second = (XgHost3dLongVector){-16, h24 - h4, -16};
    } else {
      first = (XgHost3dLongVector){-16, h24 - h4, -16};
      second = (XgHost3dLongVector){0, h28 - h4, -16};
    }
  } else if (branch_is_negative(sample_x, globals->select[2], -sample_z,
                                globals->select[3])) {
    first = (XgHost3dLongVector){0, h24 - h0, -16};
    second = (XgHost3dLongVector){16, h28 - h0, -16};
  } else {
    first = (XgHost3dLongVector){16, h28 - h0, -16};
    second = (XgHost3dLongVector){16, h4 - h0, 0};
  }

  return xg_host_3d_op0(&first, &second, &cross, &flags) &&
         xg_host_3d_vector_normal(&cross, normal);
}

static bool compute_terrain_height(const uint8_t heights[5], int32_t x_fixed,
                                   int32_t z_fixed,
                                   const XgHost3dLongVector *normal,
                                   int32_t *height_fixed) {
  const int32_t sample_x =
      (int32_t)((uint32_t)shift_right_trunc_i32(x_fixed, 3u) & 0xffffu);
  const int32_t sample_z =
      wrap_i32(0u - ((uint32_t)shift_right_trunc_i32(z_fixed, 3u) & 0xffffu));
  const bool alternate_triangle = (heights[1] & 0x80u) == 0u;
  const int32_t plane_x = alternate_triangle ? 0x10000 : 0;
  const int32_t plane_y = (int32_t)(alternate_triangle ? bits_s8(heights[2])
                                                       : bits_s8(heights[0])) *
                          0x1000;
  const uint32_t product_x =
      (uint32_t)normal->x * ((uint32_t)sample_x - (uint32_t)plane_x);
  const uint32_t product_z = (uint32_t)normal->z * (uint32_t)sample_z;
  const int32_t numerator = wrap_i32(0u - product_x - product_z);
  int32_t quotient;
  int32_t plane_height;

  if (normal->y == 0 || (normal->y == -1 && numerator == INT32_MIN))
    return false;
  quotient = numerator / normal->y;
  plane_height = wrap_i32((uint32_t)quotient + (uint32_t)plane_y);
  *height_fixed = wrap_i32((uint32_t)plane_height << 3u);
  return true;
}

static bool terrain_lookup_address(int32_t fixed, int32_t *coarse,
                                   int32_t *cell, uint32_t *quadrant_bit) {
  int32_t local = shift_right_floor_i32(fixed, 12u) & 0x7ff;
  int32_t first;

  *quadrant_bit = 0u;
  if (local >= 0x400) {
    local -= 0x400;
    *quadrant_bit = 1u;
  }
  first = shift_right_floor_i32(local, 4u);
  if (first < 0)
    first = wrap_i32((uint32_t)first + 7u);
  *cell = shift_right_floor_i32(first, 3u);
  first = shift_right_floor_i32(fixed, 20u);
  if (first < 0)
    first = wrap_i32((uint32_t)first + 7u);
  *coarse = shift_right_floor_i32(first, 3u);
  return *cell >= 0 && *cell <= 8;
}

static XgWorldEntityShadowsCaptureResult
capture_terrain(ShadowCaptureAccess *access,
                const ShadowTerrainGlobals *globals,
                XgWorldEntityShadowSourceEntry *entry) {
  uint8_t heights[5];
  uint32_t x_quadrant;
  uint32_t z_quadrant;
  int32_t coarse_x;
  int32_t coarse_z;
  int32_t cell_x;
  int32_t cell_z;
  int64_t map_index;
  uint32_t table_address;
  uint32_t cell_offset;
  uint32_t word;
  int32_t x_fixed = (int32_t)entry->pending.x * 0x1000;
  int32_t z_fixed = (int32_t)entry->pending.z * 0x1000;
  XgWorldEntityShadowsCaptureResult result;

  if (!terrain_lookup_address(x_fixed, &coarse_x, &cell_x, &x_quadrant) ||
      !terrain_lookup_address(z_fixed, &coarse_z, &cell_z, &z_quadrant))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_SOURCE_MISMATCH;
  map_index = (int64_t)coarse_z * globals->map_width + coarse_x;
  if (map_index < 0 || map_index >= SHADOW_TERRAIN_POINTER_COUNT)
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_SOURCE_MISMATCH;
  table_address = SHADOW_TERRAIN_POINTER_TABLE + (uint32_t)map_index * 4u;
  result = read_u32(access, table_address, &word);
  if (result != XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK)
    return result;
  entry->terrain_chunk_address = word;
  if ((word & 3u) != 0u || word == 0u ||
      word > UINT32_MAX - SHADOW_TERRAIN_CHUNK_BYTES ||
      !access->reader->authorize_source_range(
          access->reader->context, XG_WORLD_ENTITY_SHADOWS_SOURCE_TERRAIN_CHUNK,
          word, SHADOW_TERRAIN_CHUNK_BYTES))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_FORBIDDEN_RANGE;

  cell_offset = (x_quadrant | (z_quadrant << 1u)) * 0x144u +
                (uint32_t)(cell_z * 9 + cell_x) * 4u;
  entry->terrain_cell_address = word + cell_offset;
#define READ_TERRAIN_BYTE(offset, output)                                      \
  do {                                                                         \
    result = read_terrain_u8(                                                  \
        access, word, entry->terrain_cell_address + (offset), (output));       \
    if (result != XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK)                          \
      return result;                                                           \
  } while (0)
  READ_TERRAIN_BYTE(0u, &heights[0]);
  READ_TERRAIN_BYTE(1u, &heights[1]);
  READ_TERRAIN_BYTE(4u, &heights[2]);
  READ_TERRAIN_BYTE(0x24u, &heights[3]);
  READ_TERRAIN_BYTE(0x28u, &heights[4]);
#undef READ_TERRAIN_BYTE

  memcpy(entry->terrain_heights, heights, sizeof(entry->terrain_heights));

  if (!compute_terrain_normal(heights, globals, x_fixed, z_fixed,
                              &entry->terrain_normal) ||
      !compute_terrain_height(heights, x_fixed, z_fixed, &entry->terrain_normal,
                              &entry->terrain_height_fixed))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_SOURCE_MISMATCH;
  return XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK;
}

XgWorldEntityShadowsCaptureResult xg_world_entity_shadows_source_capture(
    const XgWorldEntityShadowsCaptureRequest *request,
    const XgWorldEntityShadowsAuthenticatedReader *reader,
    XgWorldEntityShadowsCapture *out_capture) {
  XgWorldEntityShadowsCapture capture = {0};
  ShadowCaptureAccess access = {0};
  ShadowTerrainGlobals terrain = {0};
  XgWorldEntityShadowsCaptureResult result;
  uint32_t word;
  uint32_t index;

  if (out_capture == NULL)
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_INVALID_ARGUMENT;
  memset(out_capture, 0, sizeof(*out_capture));
  if (request == NULL || reader == NULL || reader->read_u8 == NULL ||
      reader->read_u16 == NULL || reader->read_u32 == NULL ||
      reader->authorize_source_range == NULL ||
      request->authentication_generation == 0u ||
      request->producer_callsite != SHADOW_PRODUCER_CALLSITE ||
      request->projection_distance == 0u ||
      !request->projection_state_authenticated ||
      !raster_is_valid(&request->raster))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_INVALID_ARGUMENT;
  if (!reader->authenticated ||
      reader->authentication_generation != request->authentication_generation)
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_UNAUTHENTICATED;

  access.reader = reader;
#define READ_U32(address, output)                                              \
  do {                                                                         \
    result = read_u32(&access, (address), (output));                           \
    if (result != XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK)                          \
      return result;                                                           \
  } while (0)
  READ_U32(SHADOW_PENDING_COUNT, &capture.source.pending.count);
  if (capture.source.pending.count > XG_WORLD_ENTITY_SHADOW_RENDERABLE_CAPACITY)
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_SOURCE_MISMATCH;

  capture.source.screen_offset_x = request->screen_offset_x;
  capture.source.screen_offset_y = request->screen_offset_y;
  capture.source.projection_distance = request->projection_distance;
  apply_material(&request->raster, &capture.source.material);
  if (capture.source.pending.count == 0u)
    goto sealed;

  READ_U32(SHADOW_PENDING_LIST_POINTER, &capture.pending_list_address);
  access.pending_list_address = capture.pending_list_address;
  if ((access.pending_list_address & 3u) != 0u ||
      access.pending_list_address == 0u ||
      access.pending_list_address > UINT32_MAX - SHADOW_PENDING_LIST_BYTES ||
      !reader->authorize_source_range(
          reader->context, XG_WORLD_ENTITY_SHADOWS_SOURCE_PENDING_LIST,
          access.pending_list_address, SHADOW_PENDING_LIST_BYTES))
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_FORBIDDEN_RANGE;

  for (index = 0u; index < 8u; ++index) {
    READ_U32(SHADOW_CAMERA_MATRIX + index * 4u, &word);
    parse_matrix_word(&capture.source.camera, index, word);
  }
  READ_U32(SHADOW_CAMERA_ORIGIN_X, &word);
  capture.source.camera_origin_x_fixed = wrap_i32(word);
  READ_U32(SHADOW_CAMERA_ORIGIN_Z, &word);
  capture.source.camera_origin_z_fixed = wrap_i32(word);
  READ_U32(SHADOW_TERRAIN_MAP_WIDTH, &word);
  terrain.map_width = wrap_i32(word);
  if (terrain.map_width <= 0 ||
      terrain.map_width > (int32_t)SHADOW_TERRAIN_POINTER_COUNT)
    return XG_WORLD_ENTITY_SHADOWS_CAPTURE_SOURCE_MISMATCH;
  READ_U32(SHADOW_TERRAIN_SELECT_0, &word);
  terrain.select[0] = wrap_i32(word);
  READ_U32(SHADOW_TERRAIN_SELECT_1, &word);
  terrain.select[1] = wrap_i32(word);
  READ_U32(SHADOW_TERRAIN_SELECT_2, &word);
  terrain.select[2] = wrap_i32(word);
  READ_U32(SHADOW_TERRAIN_SELECT_3, &word);
  terrain.select[3] = wrap_i32(word);

  for (index = 0u; index < capture.source.pending.count; ++index) {
    XgWorldEntityShadowPendingEntry *pending =
        &capture.source.pending.entries[index];
    XgWorldEntityShadowSourceEntry *entry = &capture.source.entries[index];
    const uint32_t address = access.pending_list_address + index * 8u;
    uint16_t half;

    READ_U32(address, &word);
    pending->x = low_s16(word);
    pending->type = low_s16(word >> 16u);
    result = read_u16(&access, address + 4u, &half);
    if (result != XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK)
      return result;
    pending->z = (int16_t)half;
    if (pending->type < 0 || pending->type > 2)
      return XG_WORLD_ENTITY_SHADOWS_CAPTURE_SOURCE_MISMATCH;
    entry->pending = *pending;
    capture.source.has_type2 |= pending->type == 2;
    result = capture_terrain(&access, &terrain, entry);
    if (result != XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK)
      return result;
  }

  if (capture.source.has_type2) {
    uint16_t angle;

    for (index = 0u; index < 8u; ++index) {
      READ_U32(SHADOW_BILLBOARD_MATRIX + index * 4u, &word);
      parse_matrix_word(&capture.source.billboard, index, word);
    }
    result = read_u16(&access, SHADOW_TYPE2_ANGLE, &angle);
    if (result != XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK)
      return result;
    access.trig_address = SHADOW_TRIG_TABLE + ((uint32_t)angle & 0xfffu) * 4u;
    READ_U32(access.trig_address, &word);
    capture.source.type2_sine = low_s16(word);
    capture.source.type2_cosine = low_s16(word >> 16u);
  }

sealed:
#undef READ_U32
  capture.authentication_generation = request->authentication_generation;
  capture.producer_callsite = request->producer_callsite;
  capture.authenticated_read_count = access.read_count;
  capture.authenticated_read_bytes = access.read_bytes;
  capture.authenticated = true;
  capture.sealed = true;
  *out_capture = capture;
  return XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK;
}

XgWorldEntityShadowsResult xg_world_entity_shadows_prepare_native_cutover(
    const XgWorldEntityShadowsCapture *capture,
    uint64_t authentication_generation, XgWorldEntityShadowRecord *records,
    uint32_t record_capacity,
    XgWorldEntityShadowsNativePreparation *out_preparation) {
  XgWorldEntityShadowsSideEffects effects;
  uint32_t record_count;
  uint32_t accepted_count = 0u;
  uint32_t index;
  XgWorldEntityShadowsResult result;

  if (out_preparation == NULL)
    return XG_WORLD_ENTITY_SHADOWS_INVALID_ARGUMENT;
  memset(out_preparation, 0, sizeof(*out_preparation));
  if (capture == NULL || authentication_generation == 0u)
    return XG_WORLD_ENTITY_SHADOWS_INVALID_ARGUMENT;
  if (!capture->authenticated || !capture->sealed ||
      capture->authentication_generation != authentication_generation ||
      capture->producer_callsite !=
          XG_WORLD_ENTITY_SHADOWS_PRODUCER_CALLSITE)
    return XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE;

  result = xg_world_entity_shadows_build(
      &capture->source, records, record_capacity, &record_count, &effects);
  if (result != XG_WORLD_ENTITY_SHADOWS_OK)
    return result;

  for (index = 0u; index < record_count; ++index) {
    const XgWorldEntityShadowRecord *record = &records[index];
    uint16_t expected_write_mask;

    if (record->accepted) {
      expected_write_mask =
          XG_WORLD_ENTITY_SHADOW_PACKET_ACCEPTED_WRITE_MASK;
      if (record->cull != XG_WORLD_ENTITY_SHADOW_CULL_NONE ||
          !record->ordering_table_written ||
          record->ordering_bucket >= XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT)
        return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
      ++accepted_count;
    } else if (record->cull ==
               XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH) {
      expected_write_mask =
          XG_WORLD_ENTITY_SHADOW_PACKET_GEOMETRY_WRITE_MASK;
      if (record->ordering_table_written)
        return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    } else if (record->cull == XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS) {
      expected_write_mask = 0u;
      if (record->ordering_table_written)
        return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    } else {
      return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
    }
    if (record->packet_word_write_mask != expected_write_mask)
      return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;
  }
  if (effects.pending_count_before != record_count ||
      effects.pending_count_after != 0u ||
      effects.packet_slots_advanced != accepted_count ||
      effects.packet_bytes_advanced !=
          accepted_count * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE ||
      effects.ordering_insertions != accepted_count ||
      effects.pending_count_written != (record_count != 0u) ||
      !effects.pending_entries_unchanged)
    return XG_WORLD_ENTITY_SHADOWS_BUILD_FAILED;

  *out_preparation = (XgWorldEntityShadowsNativePreparation){
      .side_effects = effects,
      .authentication_generation = authentication_generation,
      .record_count = record_count,
      .accepted_record_count = accepted_count,
      .continuation_pc = XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC,
      .authenticated = true,
      .sealed = true,
  };
  return XG_WORLD_ENTITY_SHADOWS_OK;
}
