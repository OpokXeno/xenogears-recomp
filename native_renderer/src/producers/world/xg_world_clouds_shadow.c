#include "xg_world_clouds_shadow.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    CLOUD_BUFFER_INDEX = 0x8009d7f0u,
    CLOUD_PACKET_BASES = 0x8009d7f8u,
    CLOUD_CONTEXT = 0x8009be3cu,
    CLOUD_CONTEXT_OT_OFFSET = 0x70u,
    CLOUD_PACKET_STRIDE = 0x28u,
    CLOUD_STACK_FRAME_SIZE = 0x50u,
    CLOUD_SAVED_RA_OFFSET = 0x4cu,
    CLOUD_POSITION_STRIDE = 0x10u,
    CLOUD_SCRATCH_EMITTED = 0x1f8002f0u,
    CLOUD_SCRATCH_ATTEMPTS = 0x1f8002f4u,
};

typedef struct XgWorldCloudsShadowState {
    XgWorldCloudsCapture capture;
    XgWorldCloudRecord records[XG_WORLD_CLOUD_PACKET_CAPACITY];
    XgWorldCloudPosition stepped_positions[XG_WORLD_CLOUD_COUNT];
    uint32_t expected_packets[XG_WORLD_CLOUD_PACKET_CAPACITY]
                             [XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT];
    uint32_t expected_ot[XG_WORLD_CLOUD_OT_BUCKET_COUNT];
    XgWorldCloudsShadowSnapshot snapshot;
    uint32_t entry_stack_pointer;
    uint32_t packet_base;
    uint32_t ot_base;
    uint32_t record_count;
    CPUState *owner_cpu;
} XgWorldCloudsShadowState;

static XgWorldCloudsShadowState shadow;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
           (right & UINT32_C(0x1fffffff));
}

static bool word_range_is_valid(uint32_t address, uint32_t byte_count) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    const uint64_t end = (uint64_t)physical + byte_count;
    const bool valid_segment =
        segment == 0u || segment == UINT32_C(0x80000000) ||
        segment == UINT32_C(0xa0000000);
    const bool main_ram = physical <= UINT32_C(0x001fffff) &&
                          end <= UINT64_C(0x00200000);
    const bool scratch = physical >= UINT32_C(0x1f800000) &&
                         end <= UINT64_C(0x1f800400);

    return valid_segment && byte_count != 0u && (address & 3u) == 0u &&
           (byte_count & 3u) == 0u && (main_ram || scratch);
}

static bool counter_can_add(uint64_t counter, uint64_t value) {
    return counter <= UINT64_MAX - value;
}

static void clear_pending(void) {
    shadow.snapshot.pending = false;
    shadow.snapshot.active_generation = 0u;
    shadow.entry_stack_pointer = 0u;
    shadow.packet_base = 0u;
    shadow.ot_base = 0u;
    shadow.record_count = 0u;
    shadow.owner_cpu = NULL;
}

static XgWorldCloudsShadowResult block_shadow(
    XgWorldCloudsShadowBlocker blocker) {
    clear_pending();
    shadow.snapshot.phase = XG_WORLD_CLOUDS_SHADOW_PHASE_BLOCKED;
    shadow.snapshot.blocked = true;
    if (shadow.snapshot.blocker == XG_WORLD_CLOUDS_SHADOW_BLOCK_NONE)
        shadow.snapshot.blocker = blocker;
    return XG_WORLD_CLOUDS_SHADOW_BLOCKED;
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
    CPUState *cpu = context;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL)
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static void fill_expected_packet(uint32_t packet_index,
                                 const XgWorldCloudRecord *record,
                                 uint32_t tag) {
    uint32_t *words = shadow.expected_packets[packet_index];
    uint32_t vertex;

    words[0] = tag;
    words[1] = record->material_word;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        const uint32_t uv_word = 3u + vertex * 2u;

        words[2u + vertex * 2u] =
            (uint16_t)record->vertices[vertex].x |
            ((uint32_t)(uint16_t)record->vertices[vertex].y << 16u);
        words[uv_word] = (words[uv_word] & UINT32_C(0xffff0000)) |
                         record->uv[vertex];
    }
    words[3] = (words[3] & UINT32_C(0x0000ffff)) |
        ((uint32_t)record->clut << 16u);
    words[5] = (words[5] & UINT32_C(0x0000ffff)) |
        ((uint32_t)record->tpage << 16u);
}

static bool prepare_output_expectations(CPUState *cpu) {
    const uint32_t packet_bytes =
        XG_WORLD_CLOUD_PACKET_CAPACITY * CLOUD_PACKET_STRIDE;
    const uint32_t ot_bytes = XG_WORLD_CLOUD_OT_BUCKET_COUNT * 4u;
    uint32_t context;
    uint32_t buffer_index;
    uint32_t index;
    uint32_t word;

    buffer_index = cpu->read_word(CLOUD_BUFFER_INDEX);
    if (buffer_index >= 2u) return false;
    shadow.packet_base = cpu->read_word(
        CLOUD_PACKET_BASES + buffer_index * 4u);
    if (!word_range_is_valid(shadow.packet_base, packet_bytes)) return false;

    context = cpu->read_word(CLOUD_CONTEXT);
    if (context > UINT32_MAX - CLOUD_CONTEXT_OT_OFFSET ||
        !word_range_is_valid(context + CLOUD_CONTEXT_OT_OFFSET, 4u))
        return false;
    shadow.ot_base = cpu->read_word(context + CLOUD_CONTEXT_OT_OFFSET);
    if (!word_range_is_valid(shadow.ot_base, ot_bytes)) return false;

    shadow.snapshot.last_buffer_index = buffer_index;
    shadow.snapshot.last_packet_base = shadow.packet_base;
    shadow.snapshot.last_ot_base = shadow.ot_base;
    for (index = 0u; index < XG_WORLD_CLOUD_PACKET_CAPACITY; ++index) {
        uint32_t packet_word;

        for (packet_word = 0u;
             packet_word < XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT;
             ++packet_word) {
            shadow.expected_packets[index][packet_word] = cpu->read_word(
                shadow.packet_base + index * CLOUD_PACKET_STRIDE +
                packet_word * 4u);
        }
    }
    for (index = 0u; index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++index)
        shadow.expected_ot[index] =
            cpu->read_word(shadow.ot_base + index * 4u);

    for (index = 0u; index < shadow.record_count; ++index) {
        const XgWorldCloudRecord *record = &shadow.records[index];
        const uint32_t bucket = record->ordering_bucket;
        const uint32_t packet =
            shadow.packet_base + index * CLOUD_PACKET_STRIDE;

        if (bucket >= XG_WORLD_CLOUD_OT_BUCKET_COUNT) return false;
        word = UINT32_C(0x09000000) |
            (shadow.expected_ot[bucket] & UINT32_C(0x00ffffff));
        fill_expected_packet(index, record, word);
        shadow.expected_ot[bucket] = packet & UINT32_C(0x00ffffff);
    }
    return true;
}

void xg_world_clouds_shadow_reset(void) {
    memset(&shadow, 0, sizeof(shadow));
    shadow.snapshot.phase = XG_WORLD_CLOUDS_SHADOW_IDLE;
}

XgWorldCloudsShadowResult xg_world_clouds_shadow_begin(
    CPUState *cpu, uint64_t generation, GpuDrawState *draw_state,
    int32_t screen_x_cull_margin) {
    XgWorldCloudsCaptureRequest request = { 0 };
    XgWorldCloudsAuthenticatedReader reader = { 0 };
    XgWorldCloudsCaptureResult capture_result;
    XgWorldCloudsResult build_result;
    XgWorldCloudsBuildStats stats = { 0 };
    uint32_t callback_pointer;

    if (shadow.snapshot.blocked) return XG_WORLD_CLOUDS_SHADOW_BLOCKED;
    if (!counter_can_add(shadow.snapshot.begin_attempt_count, 1u))
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);
    ++shadow.snapshot.begin_attempt_count;
    if (shadow.snapshot.pending)
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_NESTED_BEGIN);
    if (cpu == NULL || draw_state == NULL || generation == 0u ||
        cpu->read_word == NULL || cpu->read_half == NULL)
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_INVALID_ARGUMENT);
    if (cpu->gpr[31] != XG_WORLD_CLOUDS_SHADOW_FULL_RETURN)
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_CALLER_RETURN);
    if (shadow.snapshot.last_generation != 0u &&
        generation < shadow.snapshot.last_generation)
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_STALE_GENERATION);
    if (cpu->gpr[29] < CLOUD_STACK_FRAME_SIZE ||
        !word_range_is_valid(cpu->gpr[29] - CLOUD_STACK_FRAME_SIZE,
                             CLOUD_STACK_FRAME_SIZE))
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_INVALID_ARGUMENT);

    callback_pointer = cpu->read_word(
        XG_WORLD_CLOUDS_SHADOW_CALLBACK_POINTER);
    shadow.snapshot.last_callback_pointer = callback_pointer;
    if (!physical_address_equals(
            callback_pointer, XG_WORLD_CLOUDS_SHADOW_CALLBACK_PHYSICAL)) {
        if (!counter_can_add(shadow.snapshot.callback_rejection_count, 1u))
            return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);
        ++shadow.snapshot.callback_rejection_count;
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_CALLBACK_POINTER);
    }
    if (!counter_can_add(shadow.snapshot.begin_count, 1u))
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);
    ++shadow.snapshot.begin_count;

    request = (XgWorldCloudsCaptureRequest){
        .authentication_generation = generation,
        .caller_return = cpu->gpr[31],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .screen_x_cull_margin = screen_x_cull_margin,
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .raster = {
            .draw_area_left = draw_state->left,
            .draw_area_top = draw_state->top,
            .draw_area_right = draw_state->right,
            .draw_area_bottom = draw_state->bottom,
            .draw_offset_x = draw_state->offset_x,
            .draw_offset_y = draw_state->offset_y,
            .texture_window_mask_x = draw_state->texture_window_mask_x,
            .texture_window_mask_y = draw_state->texture_window_mask_y,
            .texture_window_offset_x = draw_state->texture_window_offset_x,
            .texture_window_offset_y = draw_state->texture_window_offset_y,
            .dither = draw_state->dither != 0u,
            .mask_set = draw_state->mask_set != 0u,
            .mask_check = draw_state->mask_check != 0u,
        },
        .projection_state_authenticated = true,
    };
    reader = (XgWorldCloudsAuthenticatedReader){
        .context = cpu,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authentication_generation = generation,
        .authenticated = true,
    };
    capture_result = xg_world_clouds_source_capture(
        &request, &reader, &shadow.capture);
    shadow.snapshot.source_capture_result = (uint32_t)capture_result;
    if (capture_result != XG_WORLD_CLOUDS_CAPTURE_OK) {
        if (!counter_can_add(shadow.snapshot.source_capture_failure_count, 1u))
            return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);
        ++shadow.snapshot.source_capture_failure_count;
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_SOURCE_CAPTURE);
    }
    if (!counter_can_add(shadow.snapshot.source_read_count,
                         shadow.capture.authenticated_read_count) ||
        !counter_can_add(shadow.snapshot.source_read_bytes,
                         shadow.capture.authenticated_read_bytes))
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);
    shadow.snapshot.source_read_count +=
        shadow.capture.authenticated_read_count;
    shadow.snapshot.source_read_bytes +=
        shadow.capture.authenticated_read_bytes;

    build_result = xg_world_clouds_build(
        &shadow.capture.source, shadow.records,
        XG_WORLD_CLOUD_PACKET_CAPACITY, &shadow.record_count, &stats);
    shadow.snapshot.native_build_result = (uint32_t)build_result;
    shadow.snapshot.last_build_stats = stats;
    if (build_result != XG_WORLD_CLOUDS_OK) {
        if (!counter_can_add(shadow.snapshot.native_build_failure_count, 1u))
            return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);
        ++shadow.snapshot.native_build_failure_count;
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_NATIVE_BUILD);
    }
    memcpy(shadow.stepped_positions, shadow.capture.source.positions,
           sizeof(shadow.stepped_positions));
    if (xg_world_clouds_step_positions(
            shadow.stepped_positions, shadow.capture.source.velocities) !=
        XG_WORLD_CLOUDS_OK)
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_NATIVE_BUILD);
    if (!prepare_output_expectations(cpu))
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_OUTPUT_CONTEXT);

    shadow.entry_stack_pointer = cpu->gpr[29];
    shadow.snapshot.last_entry_stack_pointer = cpu->gpr[29];
    shadow.snapshot.expected_finish_stack_pointer =
        cpu->gpr[29] - CLOUD_STACK_FRAME_SIZE;
    shadow.snapshot.expected_saved_return =
        XG_WORLD_CLOUDS_SHADOW_FULL_RETURN;
    shadow.snapshot.last_position_base = shadow.capture.position_array_address;
    shadow.snapshot.last_candidate_count = shadow.record_count;
    shadow.snapshot.expected_final_cursor = shadow.packet_base +
        shadow.record_count * CLOUD_PACKET_STRIDE;
    shadow.snapshot.expected_scratch_emitted = shadow.record_count;
    shadow.snapshot.expected_scratch_attempts = stats.quad_attempt_count -
        stats.far_preinsert_depth_stops - stats.far_postinsert_depth_stops;
    shadow.snapshot.anchor_diagnostic_seen = false;
    shadow.snapshot.active_generation = generation;
    shadow.snapshot.last_generation = generation;
    shadow.owner_cpu = cpu;
    shadow.snapshot.pending = true;
    shadow.snapshot.phase = XG_WORLD_CLOUDS_SHADOW_PENDING;
    return XG_WORLD_CLOUDS_SHADOW_OK;
}

void xg_world_clouds_shadow_observe_anchor(CPUState *cpu) {
    XgWorldCloudsShadowSnapshot *snapshot = &shadow.snapshot;
    const XgWorldCloudsBuildStats *stats = &snapshot->last_build_stats;
    const uint32_t translation_address = UINT32_C(0x1f800284);
    const uint32_t flags_address = UINT32_C(0x1f8002d4);

    if (cpu == NULL || cpu != shadow.owner_cpu ||
        cpu->read_word == NULL || !snapshot->pending ||
        snapshot->blocked || snapshot->anchor_diagnostic_seen ||
        !stats->has_world_accepted_source ||
        !word_range_is_valid(translation_address, 12u) ||
        !word_range_is_valid(flags_address, 4u))
        return;

    snapshot->anchor_diagnostic_seen = true;
    snapshot->diagnostic_anchor_source = cpu->gpr[19];
    snapshot->expected_anchor_flags = stats->first_anchor_flags;
    snapshot->actual_anchor_flags = cpu->read_word(flags_address);
    snapshot->expected_anchor_translation[0] =
        stats->first_anchor_translation_x;
    snapshot->expected_anchor_translation[1] =
        stats->first_anchor_translation_y;
    snapshot->expected_anchor_translation[2] =
        stats->first_anchor_translation_z;
    for (uint32_t component = 0u; component < 3u; ++component) {
        snapshot->actual_anchor_translation[component] = (int32_t)cpu->read_word(
            translation_address + component * 4u);
    }
    if (snapshot->anchor_observation_count != UINT64_MAX)
        ++snapshot->anchor_observation_count;
    if ((snapshot->diagnostic_anchor_source !=
             stats->first_world_accepted_source ||
         snapshot->actual_anchor_flags != snapshot->expected_anchor_flags ||
         memcmp(snapshot->actual_anchor_translation,
                snapshot->expected_anchor_translation,
                sizeof(snapshot->actual_anchor_translation)) != 0) &&
        snapshot->anchor_mismatch_count != UINT64_MAX)
        ++snapshot->anchor_mismatch_count;
}

static void capture_first_packet_mismatch(
    CPUState *cpu, uint32_t packet_index, uint16_t word_mask) {
    XgWorldCloudsShadowPacketMismatch *mismatch =
        &shadow.snapshot.first_packet_mismatch;
    uint32_t word;

    if (shadow.snapshot.has_first_packet_mismatch) return;
    shadow.snapshot.has_first_packet_mismatch = true;
    mismatch->packet_address =
        shadow.packet_base + packet_index * CLOUD_PACKET_STRIDE;
    mismatch->packet_index = packet_index;
    mismatch->word_mismatch_mask = word_mask;
    if (packet_index < shadow.record_count) {
        mismatch->source_index = shadow.records[packet_index].source_index;
        mismatch->lod_quad_index =
            shadow.records[packet_index].lod_quad_index;
        mismatch->lod = shadow.records[packet_index].lod;
    }
    for (word = 0u; word < XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT;
         ++word) {
        mismatch->expected_words[word] =
            shadow.expected_packets[packet_index][word];
        mismatch->actual_words[word] = cpu->read_word(
            mismatch->packet_address + word * 4u);
    }
}

static bool totals_can_accumulate(
    uint32_t actual_primitives, uint32_t packet_matches,
    uint32_t packet_mismatches, uint32_t payload_mismatches,
    uint32_t geometry_mismatches, uint32_t tag_mismatches,
    uint32_t unexpected_writes, uint32_t ot_mismatches,
    uint32_t position_mismatches, bool cursor_mismatch,
    bool scratch_emitted_mismatch, bool scratch_attempt_mismatch) {
    XgWorldCloudsShadowSnapshot *snapshot = &shadow.snapshot;

    return counter_can_add(snapshot->completion_count, 1u) &&
        counter_can_add(snapshot->candidate_count, shadow.record_count) &&
        counter_can_add(snapshot->primitive_count, actual_primitives) &&
        counter_can_add(snapshot->packet_match_count, packet_matches) &&
        counter_can_add(snapshot->packet_mismatch_count, packet_mismatches) &&
        counter_can_add(snapshot->payload_mismatch_count, payload_mismatches) &&
        counter_can_add(snapshot->geometry_mismatch_count,
                        geometry_mismatches) &&
        counter_can_add(snapshot->tag_mismatch_count, tag_mismatches) &&
        counter_can_add(snapshot->unexpected_packet_write_count,
                        unexpected_writes) &&
        counter_can_add(snapshot->ot_mismatch_count, ot_mismatches) &&
        counter_can_add(snapshot->position_mismatch_count,
                        position_mismatches) &&
        counter_can_add(snapshot->cursor_mismatch_count,
                        cursor_mismatch ? 1u : 0u) &&
        counter_can_add(snapshot->scratch_emitted_mismatch_count,
                        scratch_emitted_mismatch ? 1u : 0u) &&
        counter_can_add(snapshot->scratch_attempt_mismatch_count,
                        scratch_attempt_mismatch ? 1u : 0u);
}

XgWorldCloudsShadowResult xg_world_clouds_shadow_finish(CPUState *cpu) {
    XgWorldCloudsShadowSnapshot *snapshot = &shadow.snapshot;
    uint32_t packet_matches = 0u;
    uint32_t packet_mismatches = 0u;
    uint32_t payload_mismatches = 0u;
    uint32_t geometry_mismatches = 0u;
    uint32_t tag_mismatches = 0u;
    uint32_t unexpected_writes = 0u;
    uint32_t ot_mismatches = 0u;
    uint32_t position_mismatches = 0u;
    uint32_t actual_primitives = 0u;
    uint32_t index;
    bool cursor_mismatch;
    bool scratch_emitted_mismatch;
    bool scratch_attempt_mismatch;
    bool invocation_matches = true;

    if (snapshot->blocked) return XG_WORLD_CLOUDS_SHADOW_BLOCKED;
    if (!snapshot->pending)
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_UNMATCHED_FINISH);
    if (cpu == NULL || cpu != shadow.owner_cpu ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        shadow.entry_stack_pointer < CLOUD_STACK_FRAME_SIZE) {
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_FINISH_CONTEXT);
    }
    snapshot->actual_finish_stack_pointer = cpu->gpr[29];
    if (cpu->gpr[29] !=
        shadow.entry_stack_pointer - CLOUD_STACK_FRAME_SIZE ||
        !word_range_is_valid(cpu->gpr[29] + CLOUD_SAVED_RA_OFFSET, 4u))
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_FINISH_CONTEXT);
    snapshot->actual_saved_return =
        cpu->read_word(cpu->gpr[29] + CLOUD_SAVED_RA_OFFSET);
    if (snapshot->actual_saved_return !=
        XG_WORLD_CLOUDS_SHADOW_FULL_RETURN)
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_FINISH_CONTEXT);

    snapshot->actual_final_cursor = cpu->gpr[11];
    cursor_mismatch =
        snapshot->actual_final_cursor != snapshot->expected_final_cursor;
    if (snapshot->actual_final_cursor >= shadow.packet_base) {
        const uint32_t delta =
            snapshot->actual_final_cursor - shadow.packet_base;

        if (delta % CLOUD_PACKET_STRIDE == 0u)
            actual_primitives = delta / CLOUD_PACKET_STRIDE;
    }
    snapshot->last_primitive_count = actual_primitives;

    for (index = 0u; index < XG_WORLD_CLOUD_PACKET_CAPACITY; ++index) {
        const uint32_t packet =
            shadow.packet_base + index * CLOUD_PACKET_STRIDE;
        uint16_t word_mask = 0u;
        uint32_t word;

        for (word = 0u; word < XG_WORLD_CLOUDS_SHADOW_PACKET_WORD_COUNT;
             ++word) {
            if (cpu->read_word(packet + word * 4u) !=
                shadow.expected_packets[index][word])
                word_mask |= (uint16_t)(UINT16_C(1) << word);
        }
        if (index < shadow.record_count) {
            if (word_mask == 0u) {
                ++packet_matches;
            } else {
                ++packet_mismatches;
                tag_mismatches += (word_mask & UINT16_C(0x001)) != 0u;
                payload_mismatches +=
                    (word_mask & UINT16_C(0x2aa)) != 0u;
                geometry_mismatches +=
                    (word_mask & UINT16_C(0x154)) != 0u;
            }
        } else if (word_mask != 0u) {
            ++unexpected_writes;
        }
        if (word_mask != 0u) {
            invocation_matches = false;
            capture_first_packet_mismatch(cpu, index, word_mask);
        }
    }

    for (index = 0u; index < XG_WORLD_CLOUD_OT_BUCKET_COUNT; ++index) {
        const uint32_t address = shadow.ot_base + index * 4u;
        const uint32_t actual = cpu->read_word(address);

        if (actual == shadow.expected_ot[index]) continue;
        ++ot_mismatches;
        invocation_matches = false;
        if (!snapshot->has_first_ot_mismatch) {
            snapshot->has_first_ot_mismatch = true;
            snapshot->first_ot_mismatch = (XgWorldCloudsShadowOtMismatch){
                .address = address,
                .bucket = index,
                .expected_word = shadow.expected_ot[index],
                .actual_word = actual,
            };
        }
    }

    for (index = 0u; index < XG_WORLD_CLOUD_COUNT; ++index) {
        const uint32_t address = shadow.capture.position_array_address +
                                 index * CLOUD_POSITION_STRIDE;
        XgWorldCloudPosition actual = {
            (int32_t)cpu->read_word(address),
            (int32_t)cpu->read_word(address + 4u),
            (int32_t)cpu->read_word(address + 8u),
        };
        uint8_t component_mask = 0u;

        component_mask |= actual.x != shadow.stepped_positions[index].x ? 1u : 0u;
        component_mask |= actual.y != shadow.stepped_positions[index].y ? 2u : 0u;
        component_mask |= actual.z != shadow.stepped_positions[index].z ? 4u : 0u;
        if (component_mask == 0u) continue;
        ++position_mismatches;
        invocation_matches = false;
        if (!snapshot->has_first_position_mismatch) {
            snapshot->has_first_position_mismatch = true;
            snapshot->first_position_mismatch =
                (XgWorldCloudsShadowPositionMismatch){
                    .address = address,
                    .position_index = index,
                    .component_mask = component_mask,
                    .expected = shadow.stepped_positions[index],
                    .actual = actual,
                };
        }
    }

    snapshot->actual_scratch_emitted =
        cpu->read_word(CLOUD_SCRATCH_EMITTED);
    snapshot->actual_scratch_attempts =
        cpu->read_word(CLOUD_SCRATCH_ATTEMPTS);
    scratch_emitted_mismatch = snapshot->actual_scratch_emitted !=
                               snapshot->expected_scratch_emitted;
    scratch_attempt_mismatch = snapshot->actual_scratch_attempts !=
                               snapshot->expected_scratch_attempts;
    invocation_matches &= !cursor_mismatch && !scratch_emitted_mismatch &&
                          !scratch_attempt_mismatch;

    if (!totals_can_accumulate(
            actual_primitives, packet_matches, packet_mismatches,
            payload_mismatches, geometry_mismatches, tag_mismatches,
            unexpected_writes, ot_mismatches, position_mismatches,
            cursor_mismatch, scratch_emitted_mismatch,
            scratch_attempt_mismatch) ||
        !counter_can_add(invocation_matches
                             ? snapshot->invocation_match_count
                             : snapshot->invocation_mismatch_count,
                         1u))
        return block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);

    ++snapshot->completion_count;
    snapshot->candidate_count += shadow.record_count;
    snapshot->primitive_count += actual_primitives;
    snapshot->packet_match_count += packet_matches;
    snapshot->packet_mismatch_count += packet_mismatches;
    snapshot->payload_mismatch_count += payload_mismatches;
    snapshot->geometry_mismatch_count += geometry_mismatches;
    snapshot->tag_mismatch_count += tag_mismatches;
    snapshot->unexpected_packet_write_count += unexpected_writes;
    snapshot->ot_mismatch_count += ot_mismatches;
    snapshot->position_mismatch_count += position_mismatches;
    snapshot->cursor_mismatch_count += cursor_mismatch ? 1u : 0u;
    snapshot->scratch_emitted_mismatch_count +=
        scratch_emitted_mismatch ? 1u : 0u;
    snapshot->scratch_attempt_mismatch_count +=
        scratch_attempt_mismatch ? 1u : 0u;
    if (invocation_matches)
        ++snapshot->invocation_match_count;
    else
        ++snapshot->invocation_mismatch_count;

    clear_pending();
    snapshot->phase = XG_WORLD_CLOUDS_SHADOW_IDLE;
    return invocation_matches ? XG_WORLD_CLOUDS_SHADOW_OK
                              : XG_WORLD_CLOUDS_SHADOW_MISMATCH;
}

void xg_world_clouds_shadow_invalidate(void) {
    if (shadow.snapshot.blocked) return;
    if (!counter_can_add(shadow.snapshot.lifecycle_invalidation_count, 1u)) {
        (void)block_shadow(XG_WORLD_CLOUDS_SHADOW_BLOCK_COUNTER_OVERFLOW);
        return;
    }
    ++shadow.snapshot.lifecycle_invalidation_count;
    if (shadow.snapshot.pending) {
        (void)block_shadow(
            XG_WORLD_CLOUDS_SHADOW_BLOCK_LIFECYCLE_INVALIDATED);
        return;
    }
    clear_pending();
    shadow.snapshot.phase = XG_WORLD_CLOUDS_SHADOW_IDLE;
    shadow.snapshot.active_generation = 0u;
}

bool xg_world_clouds_shadow_record_native_cutover(uint32_t primitive_count) {
    if (shadow.snapshot.native_cutover_count == UINT64_MAX ||
        shadow.snapshot.native_primitive_count > UINT64_MAX - primitive_count)
        return false;
    ++shadow.snapshot.native_cutover_count;
    shadow.snapshot.native_primitive_count += primitive_count;
    return true;
}

void xg_world_clouds_shadow_snapshot(
    XgWorldCloudsShadowSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = shadow.snapshot;
}
