#include "xg_world_entity_shadows.h"
#include "xg_world_entity_shadows_source_capture.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__,         \
              #condition);                                                     \
      return 0;                                                                \
    }                                                                          \
  } while (0)

#define TEST_LIST UINT32_C(0x80100000)
#define TEST_TERRAIN UINT32_C(0x80110000)

typedef struct TestReader {
  uint32_t callback_read_count;
  uint32_t pending_count;
  bool allow_list;
  bool allow_terrain;
  bool touched_render_output;
} TestReader;

static uint32_t pack_s16(int16_t low, int16_t high) {
  return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static bool render_output_address(uint32_t address) {
  return address == UINT32_C(0x8009be14) || address == UINT32_C(0x8009be18) ||
         address == UINT32_C(0x8009be3c) ||
         (address >= UINT32_C(0x1f800000) && address < UINT32_C(0x1f800180)) ||
         (address >= UINT32_C(0x80120000) && address < UINT32_C(0x80121000));
}

static void note_read(TestReader *reader, uint32_t address) {
  ++reader->callback_read_count;
  reader->touched_render_output |= render_output_address(address);
}

static bool read_u8(void *context, uint32_t address, uint8_t *out_value) {
  TestReader *reader = context;

  if (reader == NULL || out_value == NULL)
    return false;
  note_read(reader, address);
  if (address < TEST_TERRAIN || address >= TEST_TERRAIN + 0x710u)
    return false;
  *out_value = 0u;
  return true;
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
  TestReader *reader = context;

  if (reader == NULL || out_value == NULL)
    return false;
  note_read(reader, address);
  if (address == UINT32_C(0x8006ee66) ||
      (address >= TEST_LIST + 4u && address < TEST_LIST + 3u * 8u &&
       ((address - TEST_LIST) & 7u) == 4u)) {
    *out_value = 0u;
    return true;
  }
  return false;
}

static bool matrix_word(uint32_t address, uint32_t base, int32_t translation_z,
                        uint32_t *out_value) {
  static const uint32_t identity[5] = {
      0x00001000u, 0x00000000u, 0x00001000u, 0x00000000u, 0x00001000u,
  };
  uint32_t index;

  if (address < base || address >= base + 0x20u || (address & 3u) != 0u)
    return false;
  index = (address - base) / 4u;
  *out_value = index < 5u    ? identity[index]
               : index == 7u ? (uint32_t)translation_z
                             : 0u;
  return true;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
  static const int16_t type[3] = {0, 1, 2};
  TestReader *reader = context;

  if (reader == NULL || out_value == NULL)
    return false;
  note_read(reader, address);
  if (matrix_word(address, UINT32_C(0x8009c808), 512, out_value) ||
      matrix_word(address, UINT32_C(0x8009a180), 0, out_value))
    return true;
  if (address >= TEST_LIST && address < TEST_LIST + 3u * 8u &&
      (address & 3u) == 0u) {
    const uint32_t index = (address - TEST_LIST) / 8u;

    *out_value =
        ((address - TEST_LIST) & 4u) == 0u ? pack_s16(0, type[index]) : 0u;
    return true;
  }
  switch (address) {
  case UINT32_C(0x8009be38):
    *out_value = reader->pending_count;
    return true;
  case UINT32_C(0x8009d30c):
    *out_value = TEST_LIST;
    return true;
  case UINT32_C(0x8009be28):
  case UINT32_C(0x8009be30):
    *out_value = 0u;
    return true;
  case UINT32_C(0x8009d160):
    *out_value = 8u;
    return true;
  case UINT32_C(0x8009b244):
    *out_value = 1u;
    return true;
  case UINT32_C(0x8009b24c):
  case UINT32_C(0x8009b254):
  case UINT32_C(0x8009b25c):
    *out_value = 0u;
    return true;
  case UINT32_C(0x8009c184):
    *out_value = TEST_TERRAIN;
    return true;
  case UINT32_C(0x800523f0):
    *out_value = UINT32_C(0x10000000);
    return true;
  default:
    return false;
  }
}

static bool authorize_source_range(void *context,
                                   XgWorldEntityShadowsSourceRangeKind kind,
                                   uint32_t address, uint32_t size) {
  TestReader *reader = context;

  if (kind == XG_WORLD_ENTITY_SHADOWS_SOURCE_PENDING_LIST)
    return reader->allow_list && address == TEST_LIST && size == 0x80u;
  if (kind == XG_WORLD_ENTITY_SHADOWS_SOURCE_TERRAIN_CHUNK)
    return reader->allow_terrain && address == TEST_TERRAIN && size == 0x710u;
  return false;
}

static XgWorldEntityShadowsCaptureResult
capture_source(TestReader *context, XgWorldEntityShadowsCapture *capture) {
  const XgWorldEntityShadowsCaptureRequest request = {
      .authentication_generation = 9u,
      .producer_callsite = UINT32_C(0x80071ab8),
      .screen_offset_x = 160 << 16,
      .screen_offset_y = 120 << 16,
      .projection_distance = 256u,
      .raster =
          {
              .draw_area_right = 319u,
              .draw_area_bottom = 239u,
          },
      .projection_state_authenticated = true,
  };
  const XgWorldEntityShadowsAuthenticatedReader reader = {
      .context = context,
      .read_u8 = read_u8,
      .read_u16 = read_u16,
      .read_u32 = read_u32,
      .authorize_source_range = authorize_source_range,
      .authentication_generation = 9u,
      .authenticated = true,
  };

  return xg_world_entity_shadows_source_capture(&request, &reader, capture);
}

static int test_capture_build_material_stride_and_side_effects(void) {
  static const uint16_t expected_minimum_depth[3] = {496u, 488u, 440u};
  static const uint32_t expected_bucket[3] = {31u, 30u, 27u};
  static const int16_t expected_x[3][4] = {
      {152, 167, 151, 168},
      {148, 171, 147, 172},
      {149, 170, 146, 173},
  };
  TestReader reader = {
      .pending_count = 3u, .allow_list = true, .allow_terrain = true};
  XgWorldEntityShadowsCapture capture;
  XgWorldEntityShadowRecord records[3];
  XgWorldEntityShadowsSideEffects effects;
  uint32_t count = 0u;
  uint32_t index;

  CHECK(capture_source(&reader, &capture) ==
        XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK);
  CHECK(capture.authenticated && capture.sealed);
  CHECK(capture.authentication_generation == 9u);
  CHECK(capture.producer_callsite ==
        XG_WORLD_ENTITY_SHADOWS_PRODUCER_CALLSITE);
  CHECK(capture.authenticated_read_count == 51u);
  CHECK(capture.authenticated_read_bytes == 151u);
  CHECK(reader.callback_read_count == 51u);
  CHECK(!reader.touched_render_output);
  CHECK(capture.pending_list_address == TEST_LIST);
  CHECK(capture.source.pending.count == 3u);
  CHECK(capture.source.entries[0].terrain_normal.x == 0);
  CHECK(capture.source.entries[0].terrain_normal.y == -4096);
  CHECK(capture.source.entries[0].terrain_normal.z == 0);
  CHECK(capture.source.entries[0].terrain_height_fixed == 0);
  CHECK(capture.source.has_type2);
  CHECK(capture.source.type2_sine == 0);
  CHECK(capture.source.type2_cosine == 4096);

  CHECK(xg_world_entity_shadows_build(&capture.source, records, 3u, &count,
                                      &effects) == XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(count == 3u);
  for (index = 0u; index < count; ++index) {
    CHECK(records[index].accepted);
    CHECK(records[index].cull == XG_WORLD_ENTITY_SHADOW_CULL_NONE);
    CHECK(records[index].source_index == index);
    CHECK(records[index].packet_offset == index * 0x28u);
    CHECK(records[index].packet_word_write_mask ==
          XG_WORLD_ENTITY_SHADOW_PACKET_ACCEPTED_WRITE_MASK);
    CHECK(records[index].ordering_table_written);
    CHECK(records[index].minimum_depth == expected_minimum_depth[index]);
    CHECK(records[index].ordering_bucket == expected_bucket[index]);
    CHECK(records[index].material_word == UINT32_C(0x2e484040));
    CHECK(records[index].clut == 0x7f92u);
    CHECK(records[index].tpage == 0x001eu);
    CHECK(records[index].uv[0] == 0xf080u);
    CHECK(records[index].uv[3] == 0xff8fu);
    CHECK(records[index].primitive.triangle_count == 2u);
    CHECK(records[index].primitive.material.clut_x == 288u);
    CHECK(records[index].primitive.material.clut_y == 510u);
    CHECK(records[index].primitive.material.blend_mode ==
          XG_RENDER_IR_BLEND_AVERAGE);
    CHECK(records[index].primitive.triangles[0].vertices[0].r == 0x40u);
    CHECK(records[index].primitive.triangles[0].vertices[0].g == 0x40u);
    CHECK(records[index].primitive.triangles[0].vertices[0].b == 0x48u);
    CHECK(records[index].vertices[0].z == 512u + (index == 0u   ? 16u
                                                  : index == 1u ? 24u
                                                                : 72u));
    CHECK(records[index].vertices[3].z == expected_minimum_depth[index]);
    CHECK(records[index].vertices[0].x == expected_x[index][0]);
    CHECK(records[index].vertices[1].x == expected_x[index][1]);
    CHECK(records[index].vertices[2].x == expected_x[index][2]);
    CHECK(records[index].vertices[3].x == expected_x[index][3]);
    CHECK(records[index].vertices[0].y == 120);
    CHECK(records[index].vertices[3].y == 120);
  }
  CHECK(effects.pending_count_before == 3u);
  CHECK(effects.pending_count_after == 0u);
  CHECK(effects.pending_count_written);
  CHECK(effects.pending_entries_unchanged);
  CHECK(effects.packet_slots_advanced == 3u);
  CHECK(effects.packet_bytes_advanced == 0x78u);
  CHECK(effects.ordering_insertions == 3u);
  return 1;
}

static int test_culls_capacity_and_capture_gate(void) {
  TestReader reader = {
      .pending_count = 3u, .allow_list = true, .allow_terrain = true};
  XgWorldEntityShadowsCapture capture;
  XgWorldEntityShadowsSource empty;
  XgWorldEntityShadowsSource mixed;
  XgWorldEntityShadowRecord records[3];
  XgWorldEntityShadowsSideEffects effects;
  uint32_t count = 99u;
  uint32_t index;

  CHECK(capture_source(&reader, &capture) ==
        XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK);
  empty = capture.source;
  empty.pending.count = 0u;
  CHECK(xg_world_entity_shadows_build(&empty, NULL, 0u, &count, &effects) ==
        XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(count == 0u);
  CHECK(!effects.pending_count_written);
  CHECK(effects.pending_entries_unchanged);
  CHECK(effects.packet_slots_advanced == 0u);

  CHECK(xg_world_entity_shadows_build(&capture.source, records, 2u, &count,
                                      &effects) ==
        XG_WORLD_ENTITY_SHADOWS_CAPACITY_EXCEEDED);
  CHECK(count == 0u);
  CHECK(effects.packet_slots_advanced == 0u);

  capture.source.camera.translation[2] = 5000;
  CHECK(xg_world_entity_shadows_build(&capture.source, records, 3u, &count,
                                      &effects) == XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(effects.packet_slots_advanced == 0u);
  CHECK(effects.packet_bytes_advanced == 0u);
  CHECK(effects.ordering_insertions == 0u);
  for (index = 0u; index < 3u; ++index) {
    CHECK(!records[index].accepted);
    CHECK(records[index].cull == XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH);
    CHECK(records[index].minimum_depth >= 0x1000u);
    CHECK(records[index].packet_offset == 0u);
    CHECK(records[index].packet_word_write_mask ==
          XG_WORLD_ENTITY_SHADOW_PACKET_GEOMETRY_WRITE_MASK);
    CHECK(!records[index].ordering_table_written);
  }

  CHECK(capture_source(&reader, &capture) ==
        XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK);
  capture.source.pending.count = 1u;
  capture.source.camera.translation[2] = 0;
  CHECK(xg_world_entity_shadows_build(&capture.source, records, 1u, &count,
                                      &effects) == XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(records[0].cull == XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS);
  CHECK((int32_t)records[0].rtpt_flags < 0);
  CHECK(!records[0].accepted);
  CHECK(records[0].source_index == 0u);
  CHECK(records[0].primitive.triangle_count == 2u);
  CHECK(records[0].primitive.material.clut_x == 288u);
  CHECK(!records[0].primitive.triangles[0].vertices[0].projective_position);
  CHECK(records[0].primitive.triangles[0].vertices[0].x ==
        (int32_t)records[0].vertices[0].x * 65536);
  CHECK(records[0].packet_offset == 0u);
  CHECK(records[0].packet_word_write_mask == 0u);
  CHECK(!records[0].ordering_table_written);
  CHECK(effects.packet_slots_advanced == 0u);
  CHECK(effects.packet_bytes_advanced == 0u);
  CHECK(effects.ordering_insertions == 0u);

  CHECK(capture_source(&reader, &capture) ==
        XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK);
  mixed = capture.source;
  mixed.pending.count = 2u;
  mixed.pending.entries[0].x = INT16_MAX;
  mixed.entries[0].pending.x = INT16_MAX;
  CHECK(xg_world_entity_shadows_build(&mixed, records, 2u, &count, &effects) ==
        XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(count == 2u);
  CHECK(records[0].cull == XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS);
  CHECK(records[0].packet_offset == 0u);
  CHECK(records[0].packet_word_write_mask == 0u);
  CHECK(records[1].accepted);
  CHECK(records[1].packet_offset == 0u);
  CHECK(records[1].packet_word_write_mask ==
        XG_WORLD_ENTITY_SHADOW_PACKET_ACCEPTED_WRITE_MASK);
  CHECK(effects.packet_slots_advanced == 1u);
  CHECK(effects.packet_bytes_advanced == 0x28u);

  mixed = capture.source;
  mixed.pending.count = 2u;
  mixed.pending.entries[0].z = -5000;
  mixed.entries[0].pending.z = -5000;
  CHECK(xg_world_entity_shadows_build(&mixed, records, 2u, &count, &effects) ==
        XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(records[0].cull == XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH);
  CHECK(records[0].packet_offset == 0u);
  CHECK(records[0].packet_word_write_mask ==
        XG_WORLD_ENTITY_SHADOW_PACKET_GEOMETRY_WRITE_MASK);
  CHECK(records[1].accepted);
  CHECK(records[1].packet_offset == 0u);
  CHECK(records[1].packet_word_write_mask ==
        XG_WORLD_ENTITY_SHADOW_PACKET_ACCEPTED_WRITE_MASK);
  CHECK(effects.packet_slots_advanced == 1u);

  reader.allow_terrain = false;
  CHECK(capture_source(&reader, &capture) ==
        XG_WORLD_ENTITY_SHADOWS_CAPTURE_FORBIDDEN_RANGE);
  CHECK(!reader.touched_render_output);
  return 1;
}

static int test_authenticated_native_preparation_fails_closed(void) {
  TestReader reader = {
      .pending_count = 3u, .allow_list = true, .allow_terrain = true};
  XgWorldEntityShadowsCapture capture;
  XgWorldEntityShadowsCapture rejected;
  XgWorldEntityShadowRecord records[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
  XgWorldEntityShadowsNativePreparation preparation;

  CHECK(capture_source(&reader, &capture) ==
        XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK);
  CHECK(xg_world_entity_shadows_prepare_native_cutover(
            &capture, 9u, records, XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY,
            &preparation) == XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(preparation.authenticated && preparation.sealed);
  CHECK(preparation.authentication_generation == 9u);
  CHECK(preparation.record_count == 3u);
  CHECK(preparation.accepted_record_count == 3u);
  CHECK(preparation.continuation_pc ==
        XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC);
  CHECK(preparation.side_effects.pending_count_written);
  CHECK(preparation.side_effects.pending_count_after == 0u);
  CHECK(preparation.side_effects.pending_entries_unchanged);
  CHECK(preparation.side_effects.packet_bytes_advanced == 0x78u);

  memset(&preparation, 0xa5, sizeof(preparation));
  CHECK(xg_world_entity_shadows_prepare_native_cutover(
            &capture, 10u, records, XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY,
            &preparation) == XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE);
  CHECK(preparation.record_count == 0u);
  CHECK(!preparation.authenticated && !preparation.sealed);
  CHECK(preparation.continuation_pc == 0u);

  rejected = capture;
  rejected.sealed = false;
  memset(&preparation, 0xa5, sizeof(preparation));
  CHECK(xg_world_entity_shadows_prepare_native_cutover(
            &rejected, 9u, records, XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY,
            &preparation) == XG_WORLD_ENTITY_SHADOWS_INVALID_SOURCE);
  CHECK(preparation.record_count == 0u);
  CHECK(!preparation.authenticated && !preparation.sealed);
  CHECK(preparation.continuation_pc == 0u);

  reader = (TestReader){.pending_count = 0u,
                        .allow_list = true,
                        .allow_terrain = true};
  CHECK(capture_source(&reader, &capture) ==
        XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK);
  CHECK(capture.authenticated_read_count == 1u);
  CHECK(capture.source.pending.count == 0u);
  CHECK(xg_world_entity_shadows_prepare_native_cutover(
            &capture, 9u, NULL, 0u, &preparation) ==
        XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(preparation.record_count == 0u);
  CHECK(preparation.accepted_record_count == 0u);
  CHECK(preparation.continuation_pc ==
        XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC);
  CHECK(!preparation.side_effects.pending_count_written);
  CHECK(preparation.side_effects.pending_count_after == 0u);
  CHECK(preparation.side_effects.packet_bytes_advanced == 0u);
  return 1;
}

static int test_enqueue_wraps_without_capacity_rejection(void) {
  XgWorldEntityShadowPendingList before = {0};
  XgWorldEntityShadowPendingList after;
  const int32_t position[3] = {-4097, 123, 8191};

  before.count = 15u;
  before.entries[15].pad = 0xbeefu;
  CHECK(xg_world_entity_shadows_enqueue(&before, 2, position, &after) ==
        XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(after.entries[15].x == -2);
  CHECK(after.entries[15].type == 2);
  CHECK(after.entries[15].z == 1);
  CHECK(after.entries[15].pad == 0xbeefu);
  CHECK(after.count == 0u);

  CHECK(xg_world_entity_shadows_enqueue(&after, 1, position, &before) ==
        XG_WORLD_ENTITY_SHADOWS_OK);
  CHECK(before.entries[0].x == -2);
  CHECK(before.entries[0].type == 1);
  CHECK(before.count == 1u);
  return 1;
}

int main(void) {
  return test_capture_build_material_stride_and_side_effects() &&
                 test_culls_capacity_and_capture_gate() &&
                 test_authenticated_native_preparation_fails_closed() &&
                 test_enqueue_wraps_without_capacity_rejection()
             ? 0
             : 1;
}
