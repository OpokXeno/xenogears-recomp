#include "xg_world_entity_shadows_shadow.h"

#include "xg_world_entity_shadows.h"
#include "xg_world_entity_shadows_source_capture.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define SHADOW_PENDING_COUNT XG_WORLD_ENTITY_SHADOWS_PENDING_COUNT_ADDRESS
#define SHADOW_PACKET_BASES XG_WORLD_ENTITY_SHADOWS_PACKET_BASES_ADDRESS
#define SHADOW_BUFFER_INDEX XG_WORLD_ENTITY_SHADOWS_BUFFER_INDEX_ADDRESS
#define SHADOW_CONTEXT XG_WORLD_ENTITY_SHADOWS_CONTEXT_ADDRESS

enum {
  SHADOW_STACK_FRAME_SIZE = 0x68u,
  SHADOW_SAVED_RA_OFFSET = 0x64u,
  SHADOW_PENDING_LIST_BYTES = 0x80u,
  SHADOW_PACKET_BUFFER_BYTES = XG_WORLD_ENTITY_SHADOW_PACKET_BUFFER_BYTES,
  SHADOW_PACKET_WORDS = XG_WORLD_ENTITY_SHADOW_PACKET_WORD_COUNT,
  SHADOW_OT_BUCKET_COUNT = XG_WORLD_ENTITY_SHADOW_OT_BUCKET_COUNT,
  SHADOW_RAM_BYTES = 2u * 1024u * 1024u,
};

typedef struct XgWorldEntityShadowsShadowState {
  XgWorldEntityShadowsShadowSnapshot snapshot;
  XgWorldEntityShadowsCapture capture;
  XgWorldEntityShadowRecord records[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
  XgWorldEntityShadowsSideEffects effects;
  uint32_t initial_pending_words[SHADOW_PENDING_LIST_BYTES / 4u];
  uint32_t initial_packet_words[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY]
                               [SHADOW_PACKET_WORDS];
  uint32_t expected_tags[XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY];
  uint32_t initial_ot_words[SHADOW_OT_BUCKET_COUNT];
  uint32_t expected_ot_words[SHADOW_OT_BUCKET_COUNT];
  bool ot_touched[SHADOW_OT_BUCKET_COUNT];
  uint32_t entry_stack_pointer;
  uint32_t pending_list_address;
  uint32_t initial_packet_cursor;
  uint32_t ot_base;
  uint32_t record_count;
  uint32_t accepted_count;
  uint32_t trailing_partial_index;
  bool has_trailing_cull;
  bool has_trailing_partial;
} XgWorldEntityShadowsShadowState;

static XgWorldEntityShadowsShadowState shadow_state;

static bool counter_can_add(uint64_t value, uint64_t amount) {
  return amount <= UINT64_MAX - value;
}

static bool mapped_dram_range(uint32_t address, uint32_t size) {
  const uint32_t segment = address & UINT32_C(0xe0000000);
  const uint32_t physical = address & UINT32_C(0x1fffffff);

  return size != 0u &&
         (segment == 0u || segment == UINT32_C(0x80000000) ||
          segment == UINT32_C(0xa0000000)) &&
         physical < SHADOW_RAM_BYTES && size <= SHADOW_RAM_BYTES - physical;
}

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

static bool shadow_read_u8(void *context, uint32_t address,
                           uint8_t *out_value) {
  CPUState *cpu = context;

  if (cpu == NULL || cpu->read_byte == NULL || out_value == NULL)
    return false;
  *out_value = cpu->read_byte(address);
  return true;
}

static bool shadow_read_u16(void *context, uint32_t address,
                            uint16_t *out_value) {
  CPUState *cpu = context;

  if (cpu == NULL || cpu->read_half == NULL || out_value == NULL)
    return false;
  *out_value = cpu->read_half(address);
  return true;
}

static bool shadow_read_u32(void *context, uint32_t address,
                            uint32_t *out_value) {
  CPUState *cpu = context;

  if (cpu == NULL || cpu->read_word == NULL || out_value == NULL)
    return false;
  *out_value = cpu->read_word(address);
  return true;
}

static bool shadow_authorize_source_range(
    void *context, XgWorldEntityShadowsSourceRangeKind kind, uint32_t address,
    uint32_t size) {
  const CPUState *cpu = context;

  if (cpu == NULL)
    return false;
  if (kind == XG_WORLD_ENTITY_SHADOWS_SOURCE_PENDING_LIST)
    return size == SHADOW_PENDING_LIST_BYTES && mapped_dram_range(address, size);
  if (kind == XG_WORLD_ENTITY_SHADOWS_SOURCE_TERRAIN_CHUNK)
    return size == 0x710u && mapped_dram_range(address, size);
  return false;
}

static void clear_pending(void) {
  shadow_state.snapshot.pending = false;
  shadow_state.has_trailing_cull = false;
  shadow_state.has_trailing_partial = false;
}

static XgWorldEntityShadowsShadowResult
block_shadow(XgWorldEntityShadowsShadowBlocker blocker) {
  clear_pending();
  shadow_state.snapshot.blocked = true;
  if (shadow_state.snapshot.blocker ==
      XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NONE)
    shadow_state.snapshot.blocker = blocker;
  return XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKED;
}

static uint32_t pack_xy(const XgHost3dProjectedVertex *vertex) {
  return (uint16_t)vertex->x | ((uint32_t)(uint16_t)vertex->y << 16u);
}

static void parse_matrix_word(XgHost3dMatrix *matrix, uint32_t index,
                              uint32_t word) {
  int16_t low = (int16_t)(uint16_t)word;
  int16_t high = (int16_t)(uint16_t)(word >> 16u);

  if (index == 0u) {
    matrix->rotation[0][0] = low;
    matrix->rotation[0][1] = high;
  } else if (index == 1u) {
    matrix->rotation[0][2] = low;
    matrix->rotation[1][0] = high;
  } else if (index == 2u) {
    matrix->rotation[1][1] = low;
    matrix->rotation[1][2] = high;
  } else if (index == 3u) {
    matrix->rotation[2][0] = low;
    matrix->rotation[2][1] = high;
  } else if (index == 4u) {
    matrix->rotation[2][2] = low;
    matrix->pad = (uint16_t)high;
  } else {
    matrix->translation[index - 5u] = (int32_t)word;
  }
}

static void note_first_mismatch(uint32_t address, uint32_t source_index) {
  if (shadow_state.snapshot.first_mismatch_address != 0u)
    return;
  shadow_state.snapshot.first_mismatch_address = address;
  shadow_state.snapshot.first_mismatch_source_index = source_index;
}

static void note_first_word_mismatch(uint32_t address, uint32_t source_index,
                                     uint32_t expected, uint32_t actual) {
  if (shadow_state.snapshot.first_mismatch_address != 0u)
    return;
  shadow_state.snapshot.first_expected_word = expected;
  shadow_state.snapshot.first_actual_word = actual;
  note_first_mismatch(address, source_index);
}

static bool prepare_output_state(CPUState *cpu) {
  uint32_t buffer_index;
  uint32_t context;
  uint32_t accepted_ordinal = 0u;
  uint32_t index;

  buffer_index = cpu->read_word(SHADOW_BUFFER_INDEX);
  if (buffer_index >= 2u)
    return false;
  shadow_state.initial_packet_cursor =
      cpu->read_word(SHADOW_PACKET_BASES + buffer_index * 4u);
  if ((shadow_state.initial_packet_cursor & 3u) != 0u ||
      !mapped_dram_range(shadow_state.initial_packet_cursor,
                         SHADOW_PACKET_BUFFER_BYTES))
    return false;

  for (index = 0u; index < XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY; ++index) {
    uint32_t word;

    for (word = 0u; word < SHADOW_PACKET_WORDS; ++word) {
      shadow_state.initial_packet_words[index][word] = cpu->read_word(
          shadow_state.initial_packet_cursor +
          index * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE + word * 4u);
    }
  }

  memset(shadow_state.ot_touched, 0, sizeof(shadow_state.ot_touched));
  shadow_state.has_trailing_cull =
      shadow_state.record_count != 0u &&
      !shadow_state.records[shadow_state.record_count - 1u].accepted;
  shadow_state.has_trailing_partial = false;
  shadow_state.trailing_partial_index = UINT32_MAX;

  for (index = 0u; index < shadow_state.record_count; ++index) {
    const XgWorldEntityShadowRecord *record = &shadow_state.records[index];

    if (record->packet_offset !=
        accepted_ordinal * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE)
      return false;
    if (record->accepted) {
      if (record->cull != XG_WORLD_ENTITY_SHADOW_CULL_NONE ||
          record->ordering_bucket >= SHADOW_OT_BUCKET_COUNT)
        return false;
      ++accepted_ordinal;
    } else if (record->cull == XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH) {
      if (shadow_state.has_trailing_cull &&
          record->packet_offset ==
              shadow_state.accepted_count *
                  XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE) {
        shadow_state.has_trailing_partial = true;
        shadow_state.trailing_partial_index = index;
      }
    } else if (record->cull != XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS) {
      return false;
    }
  }
  if (accepted_ordinal != shadow_state.accepted_count)
    return false;

  if (shadow_state.accepted_count != 0u) {
    context = cpu->read_word(SHADOW_CONTEXT);
    if ((context & 3u) != 0u || !mapped_dram_range(context, 0x74u))
      return false;
    shadow_state.ot_base = cpu->read_word(context + 0x70u);
    if ((shadow_state.ot_base & 3u) != 0u ||
        !mapped_dram_range(shadow_state.ot_base,
                           SHADOW_OT_BUCKET_COUNT * 4u))
      return false;
  }

  for (index = 0u; index < shadow_state.record_count; ++index) {
    const XgWorldEntityShadowRecord *record = &shadow_state.records[index];
    uint32_t packet;
    uint32_t bucket;

    if (!record->accepted)
      continue;
    packet = shadow_state.initial_packet_cursor + record->packet_offset;
    bucket = record->ordering_bucket;
    if (!shadow_state.ot_touched[bucket]) {
      shadow_state.initial_ot_words[bucket] =
          cpu->read_word(shadow_state.ot_base + bucket * 4u);
      shadow_state.expected_ot_words[bucket] =
          shadow_state.initial_ot_words[bucket];
      shadow_state.ot_touched[bucket] = true;
    }
    shadow_state.expected_tags[index] = UINT32_C(0x09000000) |
        (shadow_state.expected_ot_words[bucket] & UINT32_C(0x00ffffff));
    shadow_state.expected_ot_words[bucket] =
        (shadow_state.expected_ot_words[bucket] & UINT32_C(0xff000000)) |
        (packet & UINT32_C(0x00ffffff));
  }
  return true;
}

void xg_world_entity_shadows_shadow_reset(void) {
  memset(&shadow_state, 0, sizeof(shadow_state));
}

XgWorldEntityShadowsShadowResult xg_world_entity_shadows_shadow_begin(
    CPUState *cpu, uint64_t generation, const GpuDrawState *draw) {
  XgWorldEntityShadowsCaptureRequest request;
  XgWorldEntityShadowsAuthenticatedReader reader;
  XgWorldEntityShadowsNativePreparation preparation;
  XgWorldEntityShadowsCaptureResult capture_result;
  XgWorldEntityShadowsResult build_result;
  uint32_t accepted_count = 0u;
  uint32_t rtpt_count = 0u;
  uint32_t minimum_depth_count = 0u;
  uint32_t index;

  if (cpu == NULL || draw == NULL || generation == 0u)
    return XG_WORLD_ENTITY_SHADOWS_SHADOW_INVALID_ARGUMENT;
  if (shadow_state.snapshot.blocked)
    return XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKED;
  if (shadow_state.snapshot.pending)
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NESTED_BEGIN);
  if (shadow_state.snapshot.generation != 0u &&
      shadow_state.snapshot.generation != generation)
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_STALE_GENERATION);
  if (cpu->pc != XG_WORLD_ENTITY_SHADOWS_SHADOW_ENTRY ||
      cpu->gpr[31] != XG_WORLD_ENTITY_SHADOWS_SHADOW_RETURN ||
      cpu->read_byte == NULL || cpu->read_half == NULL ||
      cpu->read_word == NULL ||
      cpu->gpr[29] < SHADOW_STACK_FRAME_SIZE ||
      (cpu->gpr[29] & 3u) != 0u ||
      !mapped_dram_range(cpu->gpr[29] - SHADOW_STACK_FRAME_SIZE,
                         SHADOW_STACK_FRAME_SIZE))
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_ENTRY_CONTEXT);

  memset(&shadow_state.capture, 0, sizeof(shadow_state.capture));
  memset(shadow_state.records, 0, sizeof(shadow_state.records));
  memset(&shadow_state.effects, 0, sizeof(shadow_state.effects));
  memset(shadow_state.expected_tags, 0, sizeof(shadow_state.expected_tags));
  memset(shadow_state.ot_touched, 0, sizeof(shadow_state.ot_touched));
  shadow_state.record_count = 0u;
  shadow_state.accepted_count = 0u;
  shadow_state.has_trailing_cull = false;
  shadow_state.has_trailing_partial = false;
  shadow_state.entry_stack_pointer = cpu->gpr[29];
  shadow_state.pending_list_address = 0u;
  request = (XgWorldEntityShadowsCaptureRequest){
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
  reader = (XgWorldEntityShadowsAuthenticatedReader){
      .context = cpu,
      .read_u8 = shadow_read_u8,
      .read_u16 = shadow_read_u16,
      .read_u32 = shadow_read_u32,
      .authorize_source_range = shadow_authorize_source_range,
      .authentication_generation = generation,
      .authenticated = true,
  };
  capture_result = xg_world_entity_shadows_source_capture(
      &request, &reader, &shadow_state.capture);
  shadow_state.snapshot.last_source_capture_result = (uint32_t)capture_result;
  if (capture_result != XG_WORLD_ENTITY_SHADOWS_CAPTURE_OK) {
    if (shadow_state.snapshot.source_capture_failure_count == UINT64_MAX)
      return block_shadow(
          XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_COUNTER_SATURATED);
    ++shadow_state.snapshot.source_capture_failure_count;
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_SOURCE_CAPTURE);
  }

  build_result = xg_world_entity_shadows_prepare_native_cutover(
      &shadow_state.capture, generation, shadow_state.records,
      XG_WORLD_ENTITY_SHADOW_LIST_CAPACITY, &preparation);
  shadow_state.snapshot.last_native_build_result = (uint32_t)build_result;
  if (build_result != XG_WORLD_ENTITY_SHADOWS_OK) {
    if (shadow_state.snapshot.native_build_failure_count == UINT64_MAX)
      return block_shadow(
          XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_COUNTER_SATURATED);
    ++shadow_state.snapshot.native_build_failure_count;
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NATIVE_BUILD);
  }
  shadow_state.record_count = preparation.record_count;
  shadow_state.effects = preparation.side_effects;

  for (index = 0u; index < shadow_state.record_count; ++index) {
    if (shadow_state.records[index].accepted)
      ++accepted_count;
    else if (shadow_state.records[index].cull ==
             XG_WORLD_ENTITY_SHADOW_CULL_RTPT_FLAGS)
      ++rtpt_count;
    else if (shadow_state.records[index].cull ==
             XG_WORLD_ENTITY_SHADOW_CULL_MINIMUM_DEPTH)
      ++minimum_depth_count;
    else
      return block_shadow(
          XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NATIVE_BUILD);
  }
  shadow_state.accepted_count = accepted_count;
  if (shadow_state.effects.pending_count_before != shadow_state.record_count ||
      shadow_state.effects.pending_count_after != 0u ||
      shadow_state.effects.packet_slots_advanced != accepted_count ||
      shadow_state.effects.packet_bytes_advanced !=
          accepted_count * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE ||
      shadow_state.effects.ordering_insertions != accepted_count ||
      shadow_state.effects.pending_count_written !=
          (shadow_state.record_count != 0u) ||
      !shadow_state.effects.pending_entries_unchanged)
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_NATIVE_BUILD);

  if (shadow_state.record_count != 0u) {
    shadow_state.pending_list_address = shadow_state.capture.pending_list_address;
    if (!mapped_dram_range(shadow_state.pending_list_address,
                           SHADOW_PENDING_LIST_BYTES))
      return block_shadow(
          XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_OUTPUT_RANGE);
    for (index = 0u; index < SHADOW_PENDING_LIST_BYTES / 4u; ++index) {
      shadow_state.initial_pending_words[index] = cpu->read_word(
          shadow_state.pending_list_address + index * 4u);
    }
    if (!prepare_output_state(cpu))
      return block_shadow(
          XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_OUTPUT_RANGE);
  }

  if (!counter_can_add(shadow_state.snapshot.begin_count, 1u) ||
      !counter_can_add(shadow_state.snapshot.source_read_count,
                       shadow_state.capture.authenticated_read_count) ||
      !counter_can_add(shadow_state.snapshot.source_read_bytes,
                       shadow_state.capture.authenticated_read_bytes) ||
      !counter_can_add(shadow_state.snapshot.candidate_count,
                       shadow_state.record_count) ||
      !counter_can_add(shadow_state.snapshot.accepted_packet_count,
                       accepted_count) ||
      !counter_can_add(shadow_state.snapshot.rtpt_cull_count, rtpt_count) ||
      !counter_can_add(shadow_state.snapshot.minimum_depth_cull_count,
                       minimum_depth_count))
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_COUNTER_SATURATED);

  shadow_state.snapshot.generation = generation;
  ++shadow_state.snapshot.begin_count;
  shadow_state.snapshot.source_read_count +=
      shadow_state.capture.authenticated_read_count;
  shadow_state.snapshot.source_read_bytes +=
      shadow_state.capture.authenticated_read_bytes;
  shadow_state.snapshot.candidate_count += shadow_state.record_count;
  shadow_state.snapshot.accepted_packet_count += accepted_count;
  shadow_state.snapshot.rtpt_cull_count += rtpt_count;
  shadow_state.snapshot.minimum_depth_cull_count += minimum_depth_count;
  shadow_state.snapshot.last_candidate_count = shadow_state.record_count;
  shadow_state.snapshot.last_accepted_packet_count = accepted_count;
  shadow_state.snapshot.expected_finish_stack_pointer =
      shadow_state.entry_stack_pointer - SHADOW_STACK_FRAME_SIZE;
  shadow_state.snapshot.expected_saved_return =
      XG_WORLD_ENTITY_SHADOWS_SHADOW_RETURN;
  shadow_state.snapshot.last_mismatch_bits = 0u;
  shadow_state.snapshot.transform_diagnostic_seen = false;
  shadow_state.snapshot.pending = true;
  return XG_WORLD_ENTITY_SHADOWS_SHADOW_OK;
}

void xg_world_entity_shadows_shadow_observe_transform(CPUState *cpu) {
  const uint32_t local_matrix_address = UINT32_C(0x1f8000f0);
  const uint32_t matrix_address = UINT32_C(0x1f800110);
  const uint32_t position_address = UINT32_C(0x1f800104);
  const XgWorldEntityShadowSourceEntry *source_entry;
  uint32_t index;

  if (cpu == NULL || cpu->read_word == NULL ||
      !shadow_state.snapshot.pending || shadow_state.snapshot.blocked ||
      shadow_state.snapshot.transform_diagnostic_seen ||
      shadow_state.record_count == 0u)
    return;

  shadow_state.snapshot.transform_diagnostic_seen = true;
  source_entry = &shadow_state.capture.source.entries[0];
  shadow_state.snapshot.diagnostic_type = source_entry->pending.type;
  shadow_state.snapshot.diagnostic_pending_x = source_entry->pending.x;
  shadow_state.snapshot.diagnostic_pending_z = source_entry->pending.z;
  shadow_state.snapshot.diagnostic_terrain_chunk =
      source_entry->terrain_chunk_address;
  shadow_state.snapshot.diagnostic_terrain_cell =
      source_entry->terrain_cell_address;
  memcpy(shadow_state.snapshot.diagnostic_heights,
         source_entry->terrain_heights,
         sizeof(shadow_state.snapshot.diagnostic_heights));
  shadow_state.snapshot.expected_local_transform =
      shadow_state.records[0].local_transform;
  shadow_state.snapshot.expected_transform = shadow_state.records[0].transform;
  memset(&shadow_state.snapshot.actual_local_transform, 0,
         sizeof(shadow_state.snapshot.actual_local_transform));
  for (index = 0u; index < 5u; ++index) {
    parse_matrix_word(&shadow_state.snapshot.actual_local_transform, index,
                      cpu->read_word(local_matrix_address + index * 4u));
  }
  memset(&shadow_state.snapshot.actual_transform, 0,
         sizeof(shadow_state.snapshot.actual_transform));
  for (index = 0u; index < 8u; ++index) {
    parse_matrix_word(&shadow_state.snapshot.actual_transform, index,
                      cpu->read_word(matrix_address + index * 4u));
  }
  shadow_state.snapshot.expected_position[0] = shift_right_floor_i32(
      wrap_i32((uint32_t)((int32_t)source_entry->pending.x * 0x1000) -
               (uint32_t)shadow_state.capture.source.camera_origin_x_fixed),
      12u);
  shadow_state.snapshot.expected_position[1] = shift_right_floor_i32(
      source_entry->terrain_height_fixed, 12u);
  shadow_state.snapshot.expected_position[2] = shift_right_floor_i32(
      wrap_i32((uint32_t)shadow_state.capture.source.camera_origin_z_fixed -
               (uint32_t)((int32_t)source_entry->pending.z * 0x1000)),
      12u);
  for (index = 0u; index < 3u; ++index) {
    shadow_state.snapshot.actual_position[index] = (int32_t)cpu->read_word(
        position_address + index * 4u);
  }
  if (shadow_state.snapshot.transform_observation_count != UINT64_MAX)
    ++shadow_state.snapshot.transform_observation_count;
  if ((memcmp(shadow_state.snapshot.actual_local_transform.rotation,
              shadow_state.snapshot.expected_local_transform.rotation,
              sizeof(shadow_state.snapshot.actual_local_transform.rotation)) !=
           0 ||
        memcmp(shadow_state.snapshot.actual_position,
               shadow_state.snapshot.expected_position,
               sizeof(shadow_state.snapshot.actual_position)) != 0 ||
       memcmp(shadow_state.snapshot.actual_transform.rotation,
              shadow_state.snapshot.expected_transform.rotation,
              sizeof(shadow_state.snapshot.actual_transform.rotation)) != 0 ||
       memcmp(shadow_state.snapshot.actual_transform.translation,
              shadow_state.snapshot.expected_transform.translation,
              sizeof(shadow_state.snapshot.actual_transform.translation)) !=
           0) &&
      shadow_state.snapshot.transform_mismatch_count != UINT64_MAX)
    ++shadow_state.snapshot.transform_mismatch_count;
}

XgWorldEntityShadowsShadowResult
xg_world_entity_shadows_shadow_finish(CPUState *cpu) {
  uint64_t packet_matches = 0u;
  uint64_t packet_mismatches = 0u;
  uint64_t partial_matches = 0u;
  uint64_t partial_mismatches = 0u;
  uint64_t pending_count_mismatches = 0u;
  uint64_t pending_list_mismatches = 0u;
  uint64_t cursor_mismatches = 0u;
  uint64_t payload_mismatches = 0u;
  uint64_t geometry_mismatches = 0u;
  uint64_t tag_mismatches = 0u;
  uint64_t ot_mismatches = 0u;
  uint32_t mismatch_bits = 0u;
  uint32_t finish_stack;
  uint32_t index;

  if (!shadow_state.snapshot.pending) {
    return shadow_state.snapshot.blocked
               ? XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKED
               : XG_WORLD_ENTITY_SHADOWS_SHADOW_INVALID_STATE;
  }
  if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL)
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_FINISH_CONTEXT);
  finish_stack = shadow_state.entry_stack_pointer - SHADOW_STACK_FRAME_SIZE;
  shadow_state.snapshot.actual_finish_stack_pointer = cpu->gpr[29];
  if (cpu->gpr[29] != finish_stack || (cpu->gpr[29] & 3u) != 0u ||
      !mapped_dram_range(cpu->gpr[29], SHADOW_STACK_FRAME_SIZE))
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_FINISH_CONTEXT);
  shadow_state.snapshot.actual_saved_return =
      cpu->read_word(cpu->gpr[29] + SHADOW_SAVED_RA_OFFSET);
  if (
      shadow_state.snapshot.actual_saved_return !=
          XG_WORLD_ENTITY_SHADOWS_SHADOW_RETURN)
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_FINISH_CONTEXT);

  if (cpu->read_word(SHADOW_PENDING_COUNT) != 0u) {
    mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PENDING_COUNT;
    pending_count_mismatches = 1u;
    note_first_mismatch(SHADOW_PENDING_COUNT, 0u);
  }
  if (shadow_state.record_count != 0u) {
    for (index = 0u; index < SHADOW_PENDING_LIST_BYTES / 4u; ++index) {
      const uint32_t address = shadow_state.pending_list_address + index * 4u;

      if (cpu->read_word(address) !=
          shadow_state.initial_pending_words[index]) {
        mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PENDING_LIST;
        pending_list_mismatches = 1u;
        note_first_mismatch(address, 0u);
        break;
      }
    }
    if (cpu->gpr[18] !=
        shadow_state.initial_packet_cursor +
            shadow_state.accepted_count *
                XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE) {
      mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_CURSOR;
      cursor_mismatches = 1u;
      note_first_mismatch(shadow_state.initial_packet_cursor, 0u);
    }
  }

  for (index = 0u; index < shadow_state.record_count; ++index) {
    const XgWorldEntityShadowRecord *record = &shadow_state.records[index];
    uint32_t expected_payload[5];
    uint32_t packet;
    uint32_t packet_slot;
    uint32_t vertex;
    bool payload_matches = true;
    bool geometry_matches = true;
    bool tag_matches;

    if (!record->accepted)
      continue;
    packet = shadow_state.initial_packet_cursor + record->packet_offset;
    packet_slot = record->packet_offset /
        XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE;
    for (vertex = 0u; vertex < 5u; ++vertex)
      expected_payload[vertex] =
          shadow_state.initial_packet_words[packet_slot][vertex * 2u + 1u];
    for (vertex = 0u; vertex < 5u; ++vertex) {
      const uint32_t address = packet + 4u + vertex * 8u;

      const uint32_t actual = cpu->read_word(address);

      if (actual != expected_payload[vertex]) {
        payload_matches = false;
        note_first_word_mismatch(address, record->source_index,
                                 expected_payload[vertex], actual);
      }
    }
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
      const uint32_t address = packet + 8u + vertex * 8u;

      const uint32_t expected = pack_xy(&record->vertices[vertex]);
      const uint32_t actual = cpu->read_word(address);

      if (actual != expected) {
        geometry_matches = false;
        note_first_word_mismatch(address, record->source_index, expected,
                                 actual);
      }
    }
    tag_matches = cpu->read_word(packet) == shadow_state.expected_tags[index];
    if (!tag_matches)
      note_first_word_mismatch(packet, record->source_index,
                               shadow_state.expected_tags[index],
                               cpu->read_word(packet));
    if (!payload_matches) {
      mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PAYLOAD;
      ++payload_mismatches;
    }
    if (!geometry_matches) {
      mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_GEOMETRY;
      ++geometry_mismatches;
    }
    if (!tag_matches) {
      mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_TAG;
      ++tag_mismatches;
    }
    if (payload_matches && geometry_matches && tag_matches)
      ++packet_matches;
    else
      ++packet_mismatches;
  }

  if (shadow_state.has_trailing_cull) {
    const uint32_t slot = shadow_state.accepted_count;
    const uint32_t packet = shadow_state.initial_packet_cursor +
        slot * XG_WORLD_ENTITY_SHADOW_PACKET_STRIDE;
    bool cull_slot_matches = true;
    uint32_t word;

    for (word = 0u; word < SHADOW_PACKET_WORDS; ++word) {
      uint32_t expected = shadow_state.initial_packet_words[slot][word];
      const uint32_t address = packet + word * 4u;

      if (shadow_state.has_trailing_partial &&
          (word == 2u || word == 4u || word == 6u || word == 8u)) {
        const XgWorldEntityShadowRecord *partial =
            &shadow_state.records[shadow_state.trailing_partial_index];
        expected = pack_xy(&partial->vertices[(word - 2u) / 2u]);
      }
      if (cpu->read_word(address) == expected)
        continue;
      cull_slot_matches = false;
      mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_CULL_WRITE;
      if (word == 0u) {
        mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_TAG;
        ++tag_mismatches;
      } else if (word == 2u || word == 4u || word == 6u || word == 8u) {
        mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_GEOMETRY;
        ++geometry_mismatches;
      } else {
        mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_PAYLOAD;
        ++payload_mismatches;
      }
      note_first_mismatch(address,
                          shadow_state.records[shadow_state.record_count - 1u]
                              .source_index);
    }
    if (shadow_state.has_trailing_partial) {
      if (cull_slot_matches)
        partial_matches = 1u;
      else
        partial_mismatches = 1u;
    }
  }

  for (index = 0u; index < SHADOW_OT_BUCKET_COUNT; ++index) {
    const uint32_t address = shadow_state.ot_base + index * 4u;

    if (!shadow_state.ot_touched[index])
      continue;
    if (cpu->read_word(address) == shadow_state.expected_ot_words[index])
      continue;
    mismatch_bits |= XG_WORLD_ENTITY_SHADOWS_SHADOW_MISMATCH_OT;
    ++ot_mismatches;
    note_first_mismatch(address, 0u);
  }

  if (!counter_can_add(shadow_state.snapshot.completion_count, 1u) ||
      !counter_can_add(shadow_state.snapshot.invocation_match_count,
                       mismatch_bits == 0u ? 1u : 0u) ||
      !counter_can_add(shadow_state.snapshot.invocation_mismatch_count,
                       mismatch_bits != 0u ? 1u : 0u) ||
      !counter_can_add(shadow_state.snapshot.packet_match_count,
                       packet_matches) ||
      !counter_can_add(shadow_state.snapshot.packet_mismatch_count,
                       packet_mismatches) ||
      !counter_can_add(shadow_state.snapshot.partial_write_match_count,
                       partial_matches) ||
      !counter_can_add(shadow_state.snapshot.partial_write_mismatch_count,
                       partial_mismatches) ||
      !counter_can_add(shadow_state.snapshot.pending_count_mismatch_count,
                       pending_count_mismatches) ||
      !counter_can_add(shadow_state.snapshot.pending_list_mismatch_count,
                       pending_list_mismatches) ||
      !counter_can_add(shadow_state.snapshot.cursor_mismatch_count,
                       cursor_mismatches) ||
      !counter_can_add(shadow_state.snapshot.payload_mismatch_count,
                       payload_mismatches) ||
      !counter_can_add(shadow_state.snapshot.geometry_mismatch_count,
                       geometry_mismatches) ||
      !counter_can_add(shadow_state.snapshot.tag_mismatch_count,
                       tag_mismatches) ||
      !counter_can_add(shadow_state.snapshot.ot_mismatch_count,
                       ot_mismatches))
    return block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_COUNTER_SATURATED);

  ++shadow_state.snapshot.completion_count;
  if (mismatch_bits == 0u)
    ++shadow_state.snapshot.invocation_match_count;
  else
    ++shadow_state.snapshot.invocation_mismatch_count;
  shadow_state.snapshot.packet_match_count += packet_matches;
  shadow_state.snapshot.packet_mismatch_count += packet_mismatches;
  shadow_state.snapshot.partial_write_match_count += partial_matches;
  shadow_state.snapshot.partial_write_mismatch_count += partial_mismatches;
  shadow_state.snapshot.pending_count_mismatch_count +=
      pending_count_mismatches;
  shadow_state.snapshot.pending_list_mismatch_count +=
      pending_list_mismatches;
  shadow_state.snapshot.cursor_mismatch_count += cursor_mismatches;
  shadow_state.snapshot.payload_mismatch_count += payload_mismatches;
  shadow_state.snapshot.geometry_mismatch_count += geometry_mismatches;
  shadow_state.snapshot.tag_mismatch_count += tag_mismatches;
  shadow_state.snapshot.ot_mismatch_count += ot_mismatches;
  shadow_state.snapshot.last_mismatch_bits = mismatch_bits;
  clear_pending();
  return XG_WORLD_ENTITY_SHADOWS_SHADOW_OK;
}

void xg_world_entity_shadows_shadow_lifecycle_invalidate(void) {
  if (shadow_state.snapshot.pending) {
    (void)block_shadow(
        XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_LIFECYCLE_INVALIDATED);
    return;
  }
  if (!shadow_state.snapshot.blocked)
    shadow_state.snapshot.generation = 0u;
}

void xg_world_entity_shadows_shadow_lifecycle_block(void) {
  (void)block_shadow(
      XG_WORLD_ENTITY_SHADOWS_SHADOW_BLOCKER_LIFECYCLE_BLOCKED);
}

bool xg_world_entity_shadows_shadow_record_native_cutover(
    uint32_t primitive_count) {
  XgWorldEntityShadowsShadowSnapshot *snapshot = &shadow_state.snapshot;

  if (snapshot->blocked || snapshot->pending ||
      snapshot->native_cutover_count == UINT64_MAX ||
      snapshot->native_primitive_count > UINT64_MAX - primitive_count)
    return false;
  ++snapshot->native_cutover_count;
  snapshot->native_primitive_count += primitive_count;
  return true;
}

XgWorldEntityShadowsShadowResult xg_world_entity_shadows_shadow_snapshot(
    XgWorldEntityShadowsShadowSnapshot *out_snapshot) {
  if (out_snapshot == NULL)
    return XG_WORLD_ENTITY_SHADOWS_SHADOW_INVALID_ARGUMENT;
  *out_snapshot = shadow_state.snapshot;
  return XG_WORLD_ENTITY_SHADOWS_SHADOW_OK;
}
