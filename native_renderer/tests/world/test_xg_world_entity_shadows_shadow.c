#include "xg_world_entity_shadows_shadow.h"

#include "xg_world_entity_shadows.h"
#include "xg_world_entity_shadows_source_capture.h"

#include <stdint.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      return 0;                                                                \
  } while (0)

#define TEST_LIST UINT32_C(0x80100000)
#define TEST_TERRAIN UINT32_C(0x80110000)
#define TEST_PACKETS UINT32_C(0x80120000)
#define TEST_CONTEXT UINT32_C(0x80130000)
#define TEST_OT UINT32_C(0x80140000)
#define TEST_STACK UINT32_C(0x801ff000)

enum {
  TEST_RAM_BYTES = 2u * 1024u * 1024u,
  TEST_OT_BUCKETS = 0x100u,
};

typedef struct MaterializedInvocation {
  XgWorldEntityShadowRecord records[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
  uint32_t count;
  uint32_t accepted_count;
  uint32_t first_packet;
  uint32_t first_bucket;
} MaterializedInvocation;

static uint8_t test_ram[TEST_RAM_BYTES];
static uint32_t read_count;
static uint32_t write_count;

static uint32_t physical_address(uint32_t address) {
  return address & UINT32_C(0x1fffffff);
}

static int valid_range(uint32_t address, uint32_t size) {
  const uint32_t physical = physical_address(address);
  const uint32_t segment = address & UINT32_C(0xe0000000);

  return size != 0u &&
         (segment == 0u || segment == UINT32_C(0x80000000) ||
          segment == UINT32_C(0xa0000000)) &&
         physical < TEST_RAM_BYTES && size <= TEST_RAM_BYTES - physical;
}

static void put_u16(uint32_t address, uint16_t value) {
  const uint32_t physical = physical_address(address);

  test_ram[physical] = (uint8_t)value;
  test_ram[physical + 1u] = (uint8_t)(value >> 8u);
}

static void put_u32(uint32_t address, uint32_t value) {
  const uint32_t physical = physical_address(address);

  test_ram[physical] = (uint8_t)value;
  test_ram[physical + 1u] = (uint8_t)(value >> 8u);
  test_ram[physical + 2u] = (uint8_t)(value >> 16u);
  test_ram[physical + 3u] = (uint8_t)(value >> 24u);
}

static uint8_t get_u8(uint32_t address) {
  return test_ram[physical_address(address)];
}

static uint16_t get_u16(uint32_t address) {
  const uint32_t physical = physical_address(address);

  return (uint16_t)test_ram[physical] |
         ((uint16_t)test_ram[physical + 1u] << 8u);
}

static uint32_t get_u32(uint32_t address) {
  const uint32_t physical = physical_address(address);

  return test_ram[physical] | ((uint32_t)test_ram[physical + 1u] << 8u) |
         ((uint32_t)test_ram[physical + 2u] << 16u) |
         ((uint32_t)test_ram[physical + 3u] << 24u);
}

static uint8_t cpu_read_byte(uint32_t address) {
  ++read_count;
  return valid_range(address, 1u) ? get_u8(address) : 0u;
}

static uint16_t cpu_read_half(uint32_t address) {
  ++read_count;
  return valid_range(address, 2u) ? get_u16(address) : 0u;
}

static uint32_t cpu_read_word(uint32_t address) {
  ++read_count;
  return valid_range(address, 4u) ? get_u32(address) : 0u;
}

static void cpu_write_byte(uint32_t address, uint8_t value) {
  (void)address;
  (void)value;
  ++write_count;
}

static void cpu_write_half(uint32_t address, uint16_t value) {
  (void)address;
  (void)value;
  ++write_count;
}

static void cpu_write_word(uint32_t address, uint32_t value) {
  (void)address;
  (void)value;
  ++write_count;
}

static uint32_t pack_s16(int16_t low, int16_t high) {
  return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static void put_identity_matrix(uint32_t address, int32_t translation_z) {
  static const uint32_t identity[5] = {
      UINT32_C(0x00001000), 0u, UINT32_C(0x00001000), 0u,
      UINT32_C(0x00001000),
  };
  uint32_t index;

  for (index = 0u; index < 5u; ++index)
    put_u32(address + index * 4u, identity[index]);
  put_u32(address + 20u, 0u);
  put_u32(address + 24u, 0u);
  put_u32(address + 28u, (uint32_t)translation_z);
}

static void put_packet_template(uint32_t address) {
  put_u32(address, UINT32_C(0x09000000));
  put_u32(address + 4u, UINT32_C(0x2e484040));
  put_u32(address + 8u, 0u);
  put_u32(address + 12u, UINT32_C(0x7f92f080));
  put_u32(address + 16u, 0u);
  put_u32(address + 20u, UINT32_C(0x001ef08f));
  put_u32(address + 24u, 0u);
  put_u32(address + 28u, UINT32_C(0x0000ff80));
  put_u32(address + 32u, 0u);
  put_u32(address + 36u, UINT32_C(0x0000ff8f));
}

static void configure_guest(CPUState *cpu, GpuDrawState *draw,
                            int32_t camera_z, uint32_t pending_count) {
  uint32_t index;

  memset(test_ram, 0, sizeof(test_ram));
  memset(cpu, 0, sizeof(*cpu));
  memset(draw, 0, sizeof(*draw));
  read_count = 0u;
  write_count = 0u;

  put_u32(UINT32_C(0x8009be38), pending_count);
  put_u32(UINT32_C(0x8009d30c), TEST_LIST);
  put_u32(UINT32_C(0x8009be14), TEST_PACKETS);
  put_u32(UINT32_C(0x8009be18), TEST_PACKETS + 0x280u);
  put_u32(UINT32_C(0x8009d7f0), 0u);
  put_u32(UINT32_C(0x8009be3c), TEST_CONTEXT);
  put_u32(TEST_CONTEXT + 0x70u, TEST_OT);
  put_identity_matrix(UINT32_C(0x8009c808), camera_z);
  put_identity_matrix(UINT32_C(0x8009a180), 0);
  put_u32(UINT32_C(0x8009be28), 0u);
  put_u32(UINT32_C(0x8009be30), 0u);
  put_u32(UINT32_C(0x8009d160), 8u);
  put_u32(UINT32_C(0x8009b244), 1u);
  put_u32(UINT32_C(0x8009b24c), 0u);
  put_u32(UINT32_C(0x8009b254), 0u);
  put_u32(UINT32_C(0x8009b25c), 0u);
  put_u32(UINT32_C(0x8009c184), TEST_TERRAIN);
  put_u16(UINT32_C(0x8006ee66), 0u);
  put_u32(UINT32_C(0x800523f0), UINT32_C(0x10000000));
  for (index = 0u; index < pending_count; ++index) {
    put_u32(TEST_LIST + index * 8u, pack_s16(0, 0));
    put_u32(TEST_LIST + index * 8u + 4u, 0u);
  }
  for (index = 0u; index < XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY; ++index)
    put_packet_template(TEST_PACKETS +
                        index * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE);
  for (index = 0u; index < TEST_OT_BUCKETS; ++index)
    put_u32(TEST_OT + index * 4u, UINT32_C(0x00fff000) + index);

  cpu->pc = XG_WORLD_ENTITY_SHADOWS_SHADOW_ENTRY;
  cpu->gpr[29] = TEST_STACK;
  cpu->gpr[31] = XG_WORLD_ENTITY_SHADOWS_SHADOW_RETURN;
  cpu->gte_ctrl[24] = 160u << 16u;
  cpu->gte_ctrl[25] = 120u << 16u;
  cpu->gte_ctrl[26] = 256u;
  cpu->read_byte = cpu_read_byte;
  cpu->read_half = cpu_read_half;
  cpu->read_word = cpu_read_word;
  cpu->write_byte = cpu_write_byte;
  cpu->write_half = cpu_write_half;
  cpu->write_word = cpu_write_word;
  draw->right = 319u;
  draw->bottom = 239u;
}

static bool capture_read_u8(void *context, uint32_t address,
                            uint8_t *out_value) {
  (void)context;
  *out_value = cpu_read_byte(address);
  return valid_range(address, 1u);
}

static bool capture_read_u16(void *context, uint32_t address,
                             uint16_t *out_value) {
  (void)context;
  *out_value = cpu_read_half(address);
  return valid_range(address, 2u);
}

static bool capture_read_u32(void *context, uint32_t address,
                             uint32_t *out_value) {
  (void)context;
  *out_value = cpu_read_word(address);
  return valid_range(address, 4u);
}

static bool capture_authorize(
    void *context, XgWorldEntityShadowsSourceRangeKind kind, uint32_t address,
    uint32_t size) {
  (void)context;
  if (kind == XG_WORLD_ENTITY_SHADOWS_SOURCE_PENDING_LIST)
    return address == TEST_LIST && size == 0x80u && valid_range(address, size);
  if (kind == XG_WORLD_ENTITY_SHADOWS_SOURCE_TERRAIN_CHUNK)
    return address == TEST_TERRAIN && size == 0x710u &&
           valid_range(address, size);
  return false;
}

static uint32_t pack_xy(const XgHost3dProjectedVertex *vertex) {
  return (uint16_t)vertex->x | ((uint32_t)(uint16_t)vertex->y << 16u);
}

static int materialize_guest(CPUState *cpu, const GpuDrawState *draw,
                             uint64_t generation,
                             MaterializedInvocation *out) {
  XgWorldEntityShadowsCaptureRequest request = {
      .authentication_generation = generation,
      .producer_callsite = XG_WORLD_ENTITY_SHADOWS_SHADOW_CALLSITE,
      .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
      .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
      .projection_distance = (uint16_t)cpu->gte_ctrl[26],
      .raster =
          {
              .draw_area_left = draw->left,
              .draw_area_top = draw->top,
              .draw_area_right = draw->right,
              .draw_area_bottom = draw->bottom,
              .draw_offset_x = draw->offset_x,
              .draw_offset_y = draw->offset_y,
              .dither = draw->dither != 0u,
              .mask_set = draw->mask_set != 0u,
              .mask_check = draw->mask_check != 0u,
          },
      .projection_state_authenticated = true,
  };
  XgWorldEntityShadowsAuthenticatedReader reader = {
      .read_u8 = capture_read_u8,
      .read_u16 = capture_read_u16,
      .read_u32 = capture_read_u32,
      .authorize_source_range = capture_authorize,
      .authentication_generation = generation,
      .authenticated = true,
  };
  XgWorldEntityShadowsCapture capture;
  XgWorldEntityShadowsSideEffects effects;
  uint32_t index;

  memset(out, 0, sizeof(*out));
  if (xg_world_entity_shadows_source_capture(&request, &reader, &capture) !=
      XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK)
    return 0;
  if (xg_world_entity_shadows_build(
          &capture.source, out->records,
          XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY, &out->count, &effects) !=
      XG_WORLD_ENTITY_SHADOWS_OK)
    return 0;

  for (index = 0u; index < out->count; ++index) {
    const XgWorldEntityShadowRecord *record = &out->records[index];
    const uint32_t packet = TEST_PACKETS + record->packet_offset;
    uint32_t vertex;

    if (record->cull == XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS)
      continue;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
      put_u32(packet + 8u + vertex * 8u,
              pack_xy(&record->vertices[vertex]));
    }
    if (!record->accepted)
      continue;
    if (out->accepted_count == 0u) {
      out->first_packet = packet;
      out->first_bucket = record->ordering_bucket;
    }
    {
      const uint32_t ot_address = TEST_OT + record->ordering_bucket * 4u;
      const uint32_t ot_word = get_u32(ot_address);

      put_u32(packet, UINT32_C(0x09000000) |
                          (ot_word & UINT32_C(0x00ffffff)));
      put_u32(ot_address, (ot_word & UINT32_C(0xff000000)) |
                              (packet & UINT32_C(0x00ffffff)));
    }
    ++out->accepted_count;
  }
  put_u32(UINT32_C(0x8009be38), 0u);
  cpu->pc = XG_WORLD_ENTITY_SHADOWS_SHADOW_FINISH;
  cpu->gpr[29] = TEST_STACK - 0x68u;
  cpu->gpr[18] = TEST_PACKETS +
      out->accepted_count * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE;
  put_u32(cpu->gpr[29] + 0x64u,
          XG_WORLD_ENTITY_SHADOWS_SHADOW_RETURN);
  return 1;
}

static int test_success_observes_compact_packets(void) {
  CPUState cpu;
  GpuDrawState draw;
  MaterializedInvocation materialized;
  XgWorldEntityShadowsShadowSnapshot snapshot;

  configure_guest(&cpu, &draw, 512, 2u);
  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 1u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(materialize_guest(&cpu, &draw, 1u, &materialized));
  CHECK(materialized.accepted_count == 2u);
  CHECK(materialized.records[0].packet_offset == 0u);
  CHECK(materialized.records[1].packet_offset == 0x28u);
  CHECK(xg_world_entity_shadows_shadow_finish(&cpu) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(!snapshot.pending && !snapshot.blocked);
  CHECK(snapshot.begin_count == 1u && snapshot.completion_count == 1u);
  CHECK(snapshot.invocation_match_count == 1u);
  CHECK(snapshot.invocation_mismatch_count == 0u);
  CHECK(snapshot.accepted_packet_count == 2u);
  CHECK(snapshot.packet_match_count == 2u);
  CHECK(snapshot.last_mismatch_bits == 0u);

  configure_guest(&cpu, &draw, 512, 0u);
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 1u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(materialize_guest(&cpu, &draw, 1u, &materialized));
  CHECK(xg_world_entity_shadows_shadow_finish(&cpu) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.begin_count == 2u && snapshot.completion_count == 2u);
  CHECK(snapshot.invocation_match_count == 2u);
  CHECK(snapshot.accepted_packet_count == 2u);
  CHECK(write_count == 0u);
  return 1;
}

static int test_culls_do_not_advance_and_minimum_depth_writes_geometry(void) {
  CPUState cpu;
  GpuDrawState draw;
  MaterializedInvocation materialized;
  XgWorldEntityShadowsShadowSnapshot snapshot;

  configure_guest(&cpu, &draw, 5000, 1u);
  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 2u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(materialize_guest(&cpu, &draw, 2u, &materialized));
  CHECK(materialized.accepted_count == 0u);
  CHECK(materialized.records[0].cull ==
        XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH);
  CHECK(materialized.records[0].packet_offset == 0u);
  CHECK(cpu.gpr[18] == TEST_PACKETS);
  CHECK(get_u32(TEST_PACKETS + 8u) != 0u);
  CHECK(xg_world_entity_shadows_shadow_finish(&cpu) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.minimum_depth_cull_count == 1u);
  CHECK(snapshot.accepted_packet_count == 0u);
  CHECK(snapshot.partial_write_match_count == 1u);
  CHECK(snapshot.invocation_match_count == 1u);

  configure_guest(&cpu, &draw, 0, 1u);
  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 3u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(materialize_guest(&cpu, &draw, 3u, &materialized));
  CHECK(materialized.records[0].cull ==
        XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS);
  CHECK(materialized.records[0].packet_offset == 0u);
  CHECK(xg_world_entity_shadows_shadow_finish(&cpu) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.rtpt_cull_count == 1u);
  CHECK(snapshot.invocation_match_count == 1u);
  CHECK(snapshot.last_mismatch_bits == 0u);
  CHECK(write_count == 0u);
  return 1;
}

static int test_mismatch_categories_are_independent(void) {
  CPUState cpu;
  GpuDrawState draw;
  MaterializedInvocation materialized;
  XgWorldEntityShadowsShadowSnapshot snapshot;
  const uint32_t expected_bits =
      XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PENDING_COUNT |
      XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PENDING_LIST |
      XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_CURSOR |
      XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PAYLOAD |
      XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_GEOMETRY |
      XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_TAG |
      XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_OT;

  configure_guest(&cpu, &draw, 512, 1u);
  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 4u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(materialize_guest(&cpu, &draw, 4u, &materialized));
  CHECK(materialized.accepted_count == 1u);
  put_u32(UINT32_C(0x8009be38), 1u);
  put_u32(TEST_LIST, get_u32(TEST_LIST) ^ 1u);
  put_u32(materialized.first_packet,
          get_u32(materialized.first_packet) ^ 1u);
  put_u32(materialized.first_packet + 4u,
          get_u32(materialized.first_packet + 4u) ^ 1u);
  put_u32(materialized.first_packet + 8u,
          get_u32(materialized.first_packet + 8u) ^ 1u);
  put_u32(TEST_OT + materialized.first_bucket * 4u,
          get_u32(TEST_OT + materialized.first_bucket * 4u) ^ 1u);
  cpu.gpr[18] += 4u;
  CHECK(xg_world_entity_shadows_shadow_finish(&cpu) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.invocation_mismatch_count == 1u);
  CHECK((snapshot.last_mismatch_bits & expected_bits) == expected_bits);
  CHECK(snapshot.packet_mismatch_count == 1u);
  CHECK(snapshot.pending_count_mismatch_count == 1u);
  CHECK(snapshot.pending_list_mismatch_count == 1u);
  CHECK(snapshot.cursor_mismatch_count == 1u);
  CHECK(snapshot.payload_mismatch_count == 1u);
  CHECK(snapshot.geometry_mismatch_count == 1u);
  CHECK(snapshot.tag_mismatch_count == 1u);
  CHECK(snapshot.ot_mismatch_count == 1u);
  CHECK(write_count == 0u);
  return 1;
}

static int test_fail_closed_range_and_lifecycle(void) {
  CPUState cpu;
  GpuDrawState draw;
  MaterializedInvocation materialized;
  XgWorldEntityShadowsShadowSnapshot snapshot;

  configure_guest(&cpu, &draw, 512, 1u);
  put_u32(UINT32_C(0x8009d30c), UINT32_C(0x1f800000));
  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 5u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKED);
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.blocked);
  CHECK(snapshot.blocker ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_SOURCE_CAPTURE);

  configure_guest(&cpu, &draw, 512, 1u);
  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 6u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  xg_world_entity_shadows_shadow_lifecycle_invalidate();
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.blocked && !snapshot.pending);
  CHECK(snapshot.blocker ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_LIFECYCLE_INVALIDATED);

  configure_guest(&cpu, &draw, 512, 1u);
  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 7u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(materialize_guest(&cpu, &draw, 7u, &materialized));
  CHECK(xg_world_entity_shadows_shadow_finish(&cpu) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  xg_world_entity_shadows_shadow_lifecycle_invalidate();
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(!snapshot.blocked && snapshot.generation == 0u);

  configure_guest(&cpu, &draw, 512, 1u);
  CHECK(xg_world_entity_shadows_shadow_begin(&cpu, 8u, &draw) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  xg_world_entity_shadows_shadow_lifecycle_block();
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.blocked && !snapshot.pending);
  CHECK(snapshot.blocker ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_LIFECYCLE_BLOCKED);
  CHECK(write_count == 0u);
  return 1;
}

static int test_native_cutover_accounting(void) {
  XgWorldEntityShadowsShadowSnapshot snapshot;

  xg_world_entity_shadows_shadow_reset();
  CHECK(xg_world_entity_shadows_shadow_record_native_cutover(3u));
  CHECK(xg_world_entity_shadows_shadow_record_native_cutover(0u));
  CHECK(xg_world_entity_shadows_shadow_snapshot(&snapshot) ==
        XG_WORLD_ENTITY_SHADOWS_SHADOW_OK);
  CHECK(snapshot.native_cutover_count == 2u);
  CHECK(snapshot.native_primitive_count == 3u);
  return 1;
}

int main(void) {
  return test_success_observes_compact_packets() &&
                 test_culls_do_not_advance_and_minimum_depth_writes_geometry() &&
                 test_mismatch_categories_are_independent() &&
                 test_fail_closed_range_and_lifecycle() &&
                 test_native_cutover_accounting()
             ? 0
             : 1;
}
