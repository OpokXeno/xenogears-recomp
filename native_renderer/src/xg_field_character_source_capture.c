#include "xg_field_character_source_capture.h"

#include <stddef.h>
#include <string.h>

enum {
    PRODUCER_DISABLED_ADDRESS = 0x8004f37c,
    ORDERING_SHIFT_ADDRESS = 0x80050100,
    SOURCE_MATRIX_ADDRESS = 0x800afa64,
    ACTOR_COUNT_ADDRESS = 0x800adbfc,
    ACTOR_BASE_ADDRESS = 0x800afb10,
    SCALE_MODE_ADDRESS = 0x800b2268,
    PROJECTION_DISTANCE_ADDRESS = 0x800af9f8,
    PRODUCER_OT_BASE_STACK_OFFSET = 0xd0,
    PRODUCER_FT4_INDEX_STACK_OFFSET = 0xd8,
    ACTOR_SECONDARY_OFFSET = 0x04,
    ACTOR_MODEL_OFFSET = 0x08,
    ACTOR_WORLD_X_OFFSET = 0x20,
    ACTOR_WORLD_Z_OFFSET = 0x28,
    ACTOR_STATE_OFFSET = 0x4c,
    ACTOR_ACTIVE_OFFSET = 0x58,
    SECONDARY_WORLD_Y_OFFSET = 0x84,
    STATE_FLAGS_0_OFFSET = 0x00,
    STATE_FLAGS_4_OFFSET = 0x04,
    STATE_FLAGS_14_OFFSET = 0x14,
    STATE_ORIENTATION_OFFSET = 0x50,
    STATE_SCALE_OFFSET = 0xf4,
    PACKET_RECORD_OFFSET = 0x20,
    PACKET_RECORD_SIZE = 0x28,
    PSX_RAM_SIZE = 0x200000,
    PSX_SCRATCHPAD_START = 0x1f800000,
    PSX_SCRATCHPAD_SIZE = 0x400,
    GPU_MMIO_START = 0x1f801810,
    GPU_MMIO_END = 0x1f801818,
};

typedef struct CaptureAccess {
    const XgFieldCharacterSourceCaptureRequest *request;
    const XgFieldCharacterAuthenticatedReader *reader;
    uint32_t actor_address;
    uint32_t secondary_address;
    uint32_t state_address;
    uint32_t model_address;
    uint32_t packet_address;
    uint32_t ot_base;
    uint32_t read_count;
    uint32_t read_bytes;
} CaptureAccess;

static uint32_t physical_address(uint32_t address) {
    return address & UINT32_C(0x1fffffff);
}

static bool add_address(uint32_t address, uint32_t offset,
                        uint32_t *out_address) {
    const uint64_t result = (uint64_t)address + offset;

    if (out_address == NULL || result > UINT32_MAX) return false;
    *out_address = (uint32_t)result;
    return true;
}

static bool range_contains(uint32_t start, uint32_t size, uint32_t address,
                           uint32_t width) {
    const uint64_t first = physical_address(start);
    const uint64_t last = first + size;
    const uint64_t value = physical_address(address);

    return size != 0u && width != 0u && value >= first &&
           value + width <= last;
}

static bool is_ram_range(uint32_t address, uint32_t width) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint64_t end = (uint64_t)physical_address(address) + width;

    return (segment == 0u || segment == UINT32_C(0x80000000) ||
            segment == UINT32_C(0xa0000000)) &&
           width != 0u && end <= PSX_RAM_SIZE;
}

static bool is_scratchpad_range(uint32_t address, uint32_t width) {
    const uint32_t segment = address & UINT32_C(0xe0000000);
    const uint64_t start = physical_address(address);
    const uint64_t end = start + width;

    return (segment == 0u || segment == UINT32_C(0x80000000) ||
            segment == UINT32_C(0xa0000000)) &&
           width != 0u && start >= PSX_SCRATCHPAD_START &&
           end <= PSX_SCRATCHPAD_START + PSX_SCRATCHPAD_SIZE;
}

static bool is_producer_stack_read(const CaptureAccess *access,
                                   uint32_t address, uint32_t width) {
    uint32_t candidate;

    if (width != 4u) return false;
    return (add_address(access->request->producer_stack_pointer,
                        PRODUCER_OT_BASE_STACK_OFFSET, &candidate) &&
            physical_address(address) == physical_address(candidate)) ||
           (add_address(access->request->producer_stack_pointer,
                        PRODUCER_FT4_INDEX_STACK_OFFSET, &candidate) &&
            physical_address(address) == physical_address(candidate));
}

static bool address_equals(uint32_t left, uint32_t right) {
    return physical_address(left) == physical_address(right);
}

static bool is_model_vertex_read(const CaptureAccess *access,
                                 uint32_t address, uint32_t width) {
    uint32_t offset;

    if (width != 4u || !range_contains(access->model_address, 0x20u, address,
                                       width))
        return false;
    offset = physical_address(address) - physical_address(access->model_address);
    return (offset & 3u) == 0u;
}

static bool is_matrix_read(uint32_t address, uint32_t width) {
    uint32_t offset;

    if (width != 4u ||
        !range_contains(SOURCE_MATRIX_ADDRESS, 0x20u, address, width))
        return false;
    offset = physical_address(address) - physical_address(SOURCE_MATRIX_ADDRESS);
    return (offset & 3u) == 0u;
}

static bool is_allowed_read(const CaptureAccess *access, uint32_t address,
                            uint32_t width) {
    const uint32_t fixed_u32[] = {
        PRODUCER_DISABLED_ADDRESS,
        ORDERING_SHIFT_ADDRESS,
        ACTOR_COUNT_ADDRESS,
        ACTOR_BASE_ADDRESS,
        SCALE_MODE_ADDRESS,
        PROJECTION_DISTANCE_ADDRESS,
    };
    const uint32_t actor_u32[] = {
        ACTOR_SECONDARY_OFFSET,
        ACTOR_MODEL_OFFSET,
        ACTOR_WORLD_X_OFFSET,
        ACTOR_WORLD_Z_OFFSET,
        ACTOR_STATE_OFFSET,
        ACTOR_ACTIVE_OFFSET,
    };
    const uint32_t state_u32[] = {
        STATE_FLAGS_0_OFFSET,
        STATE_FLAGS_4_OFFSET,
        STATE_FLAGS_14_OFFSET,
        STATE_ORIENTATION_OFFSET,
        STATE_ORIENTATION_OFFSET + 4u,
        STATE_ORIENTATION_OFFSET + 8u,
    };
    size_t index;
    uint32_t candidate;

    if (width == 4u) {
        for (index = 0u; index < sizeof(fixed_u32) / sizeof(fixed_u32[0]);
             ++index)
            if (address_equals(address, fixed_u32[index])) return true;
        if (is_matrix_read(address, width) ||
            is_model_vertex_read(access, address, width))
            return true;
        if (add_address(access->request->producer_stack_pointer,
                        PRODUCER_OT_BASE_STACK_OFFSET, &candidate) &&
            address_equals(address, candidate))
            return true;
        if (add_address(access->request->producer_stack_pointer,
                        PRODUCER_FT4_INDEX_STACK_OFFSET, &candidate) &&
            address_equals(address, candidate))
            return true;
        for (index = 0u; index < sizeof(actor_u32) / sizeof(actor_u32[0]);
             ++index)
            if (add_address(access->actor_address, actor_u32[index],
                            &candidate) &&
                address_equals(address, candidate))
                return true;
        for (index = 0u; index < sizeof(state_u32) / sizeof(state_u32[0]);
             ++index)
            if (add_address(access->state_address, state_u32[index],
                            &candidate) &&
                address_equals(address, candidate))
                return true;
    }
    if (width == 2u) {
        if (add_address(access->secondary_address, SECONDARY_WORLD_Y_OFFSET,
                        &candidate) && address_equals(address, candidate))
            return true;
        for (index = 0u; index < 3u; ++index)
            if (add_address(access->state_address,
                            STATE_SCALE_OFFSET + (uint32_t)index * 2u,
                            &candidate) &&
                address_equals(address, candidate))
                return true;
    }
    return false;
}

static bool is_forbidden_read(const CaptureAccess *access, uint32_t address,
                              uint32_t width) {
    const uint32_t physical = physical_address(address);
    size_t index;
    uint32_t post_gte_address;

    if (!is_ram_range(address, width) &&
        !(is_scratchpad_range(address, width) &&
          is_producer_stack_read(access, address, width)))
        return true;
    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT; ++index) {
        if (add_address(access->packet_address,
                        8u + (uint32_t)index * 8u, &post_gte_address) &&
            range_contains(post_gte_address, 4u, address, width))
            return true;
    }
    if (range_contains(access->packet_address, PACKET_RECORD_SIZE, address,
                       width))
        return true;
    if (access->ot_base != 0u &&
        range_contains(access->ot_base, 4u, address, width))
        return true;
    return physical >= GPU_MMIO_START && physical < GPU_MMIO_END;
}

static XgFieldCharacterSourceCaptureResult read_u32(CaptureAccess *access,
                                                     uint32_t address,
                                                     uint32_t *out_value) {
    if (access->read_count >=
        XG_FIELD_CHARACTER_SOURCE_MAX_AUTHENTICATED_READS)
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE;
    if (is_forbidden_read(access, address, 4u) ||
        !is_allowed_read(access, address, 4u))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u32(access->reader->context, address, out_value))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 4u;
    return XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK;
}

static XgFieldCharacterSourceCaptureResult read_u16(CaptureAccess *access,
                                                     uint32_t address,
                                                     uint16_t *out_value) {
    if (access->read_count >=
        XG_FIELD_CHARACTER_SOURCE_MAX_AUTHENTICATED_READS)
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE;
    if (is_forbidden_read(access, address, 2u) ||
        !is_allowed_read(access, address, 2u))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE;
    if (!access->reader->read_u16(access->reader->context, address, out_value))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_READ_FAILED;
    ++access->read_count;
    access->read_bytes += 2u;
    return XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK;
}

static void unpack_s16_pair(uint32_t value, int16_t *low, int16_t *high) {
    *low = (int16_t)(uint16_t)value;
    *high = (int16_t)(uint16_t)(value >> 16u);
}

static bool digest_is_present(const uint8_t *digest) {
    size_t index;

    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE; ++index)
        if (digest[index] != 0u) return true;
    return false;
}

static bool raster_state_is_valid(
    const XgFieldCharacterSourceRasterState *raster) {
    return raster->draw_area_left <= raster->draw_area_right &&
           raster->draw_area_top <= raster->draw_area_bottom &&
           raster->draw_area_right <= 1023u &&
           raster->draw_area_bottom <= 1023u &&
           raster->draw_offset_x >= -1024 && raster->draw_offset_x <= 1023 &&
           raster->draw_offset_y >= -1024 && raster->draw_offset_y <= 1023 &&
           raster->texture_window_mask_x <= 31u &&
           raster->texture_window_mask_y <= 31u &&
           raster->texture_window_offset_x <= 31u &&
           raster->texture_window_offset_y <= 31u && raster->dither <= 1u &&
           raster->mask_set <= 1u && raster->mask_check <= 1u;
}

static void seal_snapshot(XgFieldCharacterSourceSnapshot *snapshot,
                          const CaptureAccess *access) {
    snapshot->absence.authenticated_read_count = access->read_count;
    snapshot->absence.authenticated_read_bytes = access->read_bytes;
    snapshot->authenticated = 1u;
    snapshot->sealed = 1u;
    xg_field_character_source_digest(snapshot, snapshot->value_digest);
}

XgFieldCharacterSourceCaptureResult xg_field_character_source_capture(
    const XgFieldCharacterSourceCaptureRequest *request,
    const XgFieldCharacterAuthenticatedReader *reader,
    XgFieldCharacterSourceSnapshot *out_snapshot) {
    XgFieldCharacterSourceSnapshot snapshot;
    CaptureAccess access;
    XgFieldCharacterSourceCaptureResult result;
    uint32_t actor_count;
    uint32_t actor_base;
    uint32_t actor_offset;
    uint32_t actor_address;
    uint32_t secondary_address;
    uint32_t model_address;
    uint32_t state_address;
    uint32_t producer_disabled;
    uint32_t scale_mode;
    uint32_t ordering_shift;
    uint32_t projection_distance;
    uint32_t local_ft4_index;
    uint32_t packet_offset;
    uint32_t stack_ot_base_address;
    uint32_t stack_ft4_index_address;
    uint32_t word;
    uint16_t half;
    size_t index;

    _Static_assert(sizeof(XgFieldCharacterSourceSnapshot) <=
                       XG_FIELD_CHARACTER_SOURCE_SNAPSHOT_MAX_SIZE,
                   "field character source snapshot exceeds its bound");

    if (out_snapshot == NULL) return XG_FIELD_CHARACTER_SOURCE_CAPTURE_INVALID_ARGUMENT;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (request == NULL || reader == NULL || reader->read_u16 == NULL ||
        reader->read_u32 == NULL || request->source_generation == 0u ||
        request->visual_state.scene_epoch == 0u ||
        request->producer_record_id == 0u ||
        !digest_is_present(request->game_sha256) ||
        !digest_is_present(request->manifest_sha256) ||
        !raster_state_is_valid(&request->raster))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_INVALID_ARGUMENT;
    if (reader->authenticated != 1u ||
        reader->authentication_generation != request->source_generation)
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_UNAUTHENTICATED;
    if (!add_address(request->producer_stack_pointer,
                     PRODUCER_OT_BASE_STACK_OFFSET,
                     &stack_ot_base_address) ||
        !add_address(request->producer_stack_pointer,
                     PRODUCER_FT4_INDEX_STACK_OFFSET,
                     &stack_ft4_index_address) ||
        !(is_ram_range(stack_ot_base_address, 4u) ||
          is_scratchpad_range(stack_ot_base_address, 4u)) ||
        !(is_ram_range(stack_ft4_index_address, 4u) ||
          is_scratchpad_range(stack_ft4_index_address, 4u)))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE;

    memset(&snapshot, 0, sizeof(snapshot));
    memset(&access, 0, sizeof(access));
    access.request = request;
    access.reader = reader;
    access.actor_address = request->actor_record_address;
    access.model_address = request->model_address;

#define READ_U32(address, output)                                               \
    do {                                                                        \
        result = read_u32(&access, (address), (output));                         \
        if (result != XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK) return result;       \
    } while (0)
#define READ_U16(address, output)                                               \
    do {                                                                        \
        result = read_u16(&access, (address), (output));                         \
        if (result != XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK) return result;       \
    } while (0)

    READ_U32(stack_ft4_index_address, &local_ft4_index);
    if (local_ft4_index != request->producer_ft4_index ||
        local_ft4_index >= XG_FIELD_CHARACTER_SOURCE_DYNAMIC_MODEL_FT4_COUNT ||
        local_ft4_index > (UINT32_MAX - PACKET_RECORD_OFFSET) /
                              PACKET_RECORD_SIZE)
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_SOURCE_MISMATCH;
    packet_offset = local_ft4_index * PACKET_RECORD_SIZE + PACKET_RECORD_OFFSET;
    if (!add_address(request->model_address, packet_offset,
                     &access.packet_address) ||
        !is_ram_range(access.packet_address, PACKET_RECORD_SIZE))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE;
    READ_U32(stack_ot_base_address, &access.ot_base);
    if (!is_ram_range(access.ot_base, 4u))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE;

    READ_U32(PRODUCER_DISABLED_ADDRESS, &producer_disabled);
    READ_U32(ACTOR_COUNT_ADDRESS, &actor_count);
    READ_U32(ACTOR_BASE_ADDRESS, &actor_base);
    if (actor_count == 0u || request->actor_index >= actor_count ||
        request->actor_index > UINT32_MAX / XG_FIELD_CHARACTER_SOURCE_ACTOR_STRIDE)
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_SOURCE_MISMATCH;
    actor_offset = request->actor_index * XG_FIELD_CHARACTER_SOURCE_ACTOR_STRIDE;
    if (!add_address(actor_base, actor_offset, &actor_address) ||
        !address_equals(actor_address, request->actor_record_address) ||
        !is_ram_range(actor_address, XG_FIELD_CHARACTER_SOURCE_ACTOR_STRIDE))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_SOURCE_MISMATCH;
    access.actor_address = actor_address;
    READ_U32(actor_address + ACTOR_SECONDARY_OFFSET, &secondary_address);
    READ_U32(actor_address + ACTOR_MODEL_OFFSET, &model_address);
    READ_U32(actor_address + ACTOR_STATE_OFFSET, &state_address);
    if (!address_equals(model_address, request->model_address) ||
        !is_ram_range(secondary_address, SECONDARY_WORLD_Y_OFFSET + 2u) ||
        !is_ram_range(state_address, STATE_SCALE_OFFSET + 6u))
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_SOURCE_MISMATCH;
    access.secondary_address = secondary_address;
    access.model_address = model_address;
    access.state_address = state_address;

    snapshot.schema_version = XG_FIELD_CHARACTER_SOURCE_SCHEMA_VERSION;
    snapshot.presence_mask = XG_FIELD_CHARACTER_SOURCE_REQUIRED_PRESENCE;
    memcpy(snapshot.identity.game_sha256, request->game_sha256,
           sizeof(snapshot.identity.game_sha256));
    memcpy(snapshot.identity.manifest_sha256, request->manifest_sha256,
           sizeof(snapshot.identity.manifest_sha256));
    snapshot.identity.producer_entry = XG_FIELD_CHARACTER_SOURCE_PRODUCER_ENTRY;
    snapshot.identity.producer_record_id = request->producer_record_id;
    snapshot.identity.actor_index = request->actor_index;
    snapshot.identity.actor_count = actor_count;
    snapshot.identity.model_initializer =
        XG_FIELD_CHARACTER_SOURCE_MODEL_INITIALIZER_8007AA44;
    snapshot.generation.source_generation = request->source_generation;
    snapshot.generation.scene_generation = request->scene_generation;
    snapshot.generation.visual_state = request->visual_state;
    snapshot.generation.vram_mutation_serial = request->vram_mutation_serial;

    READ_U32(actor_address + ACTOR_ACTIVE_OFFSET,
             &snapshot.actor.active_gate_word);
    READ_U32(state_address + STATE_FLAGS_0_OFFSET,
             &snapshot.actor.state_flags_0);
    READ_U32(state_address + STATE_FLAGS_4_OFFSET,
             &snapshot.actor.state_flags_4);
    READ_U32(state_address + STATE_FLAGS_14_OFFSET,
             &snapshot.actor.state_flags_14);
    for (index = 0u; index < 3u; ++index) {
        READ_U32(state_address + STATE_ORIENTATION_OFFSET +
                     (uint32_t)index * 4u,
                 &word);
        snapshot.actor.orientation[index] = (int16_t)(uint16_t)word;
    }
    READ_U32(actor_address + ACTOR_WORLD_X_OFFSET, &word);
    snapshot.actor.world_offset[0] = (int32_t)word;
    READ_U16(secondary_address + SECONDARY_WORLD_Y_OFFSET, &half);
    snapshot.actor.world_offset[1] = (int16_t)half;
    READ_U32(actor_address + ACTOR_WORLD_Z_OFFSET, &word);
    snapshot.actor.world_offset[2] = (int32_t)word;
    for (index = 0u; index < 3u; ++index) {
        READ_U16(state_address + STATE_SCALE_OFFSET + (uint32_t)index * 2u,
                 &half);
        snapshot.actor.shadow_scale[index] = (int16_t)half;
    }
    READ_U32(SCALE_MODE_ADDRESS, &scale_mode);
    snapshot.actor.scale_reduced =
        scale_mode != 0u && (snapshot.actor.state_flags_0 & 0x400u) != 0u;
    snapshot.actor.scale_shift = snapshot.actor.scale_reduced ? 14u : 12u;

    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT; ++index) {
        static const XgFieldCharacterSourceVector expected[4] = {
            { 24, 0, 24, 0u },
            { -24, 0, 24, 0u },
            { 24, 0, -24, 0u },
            { -24, 0, -24, 0u },
        };
        int16_t model_pad;

        READ_U32(model_address + (uint32_t)index * 8u, &word);
        unpack_s16_pair(word, &snapshot.model_vertices[index].x,
                       &snapshot.model_vertices[index].y);
        READ_U32(model_address + (uint32_t)index * 8u + 4u, &word);
        unpack_s16_pair(word, &snapshot.model_vertices[index].z,
                        &model_pad);
        snapshot.model_vertices[index].pad = (uint16_t)model_pad;
        if (snapshot.model_vertices[index].x != expected[index].x ||
            snapshot.model_vertices[index].y != expected[index].y ||
            snapshot.model_vertices[index].z != expected[index].z)
            return XG_FIELD_CHARACTER_SOURCE_CAPTURE_SOURCE_MISMATCH;
    }
    snapshot.source_matrix.source_identity =
        XG_FIELD_CHARACTER_SOURCE_MATRIX_FIELD_CAMERA_800AFA64;
    for (index = 0u; index < 4u; ++index) {
        READ_U32(SOURCE_MATRIX_ADDRESS + (uint32_t)index * 4u, &word);
        unpack_s16_pair(word, &snapshot.source_matrix.rotation[index * 2u],
                       &snapshot.source_matrix.rotation[index * 2u + 1u]);
    }
    READ_U32(SOURCE_MATRIX_ADDRESS + 16u, &word);
    snapshot.source_matrix.rotation[8] = (int16_t)(uint16_t)word;
    snapshot.source_matrix.source_pad = (int16_t)(uint16_t)(word >> 16u);
    for (index = 0u; index < 3u; ++index) {
        READ_U32(SOURCE_MATRIX_ADDRESS + 20u + (uint32_t)index * 4u, &word);
        snapshot.source_matrix.translation[index] = (int32_t)word;
    }
    READ_U32(ORDERING_SHIFT_ADDRESS, &ordering_shift);
    READ_U32(PROJECTION_DISTANCE_ADDRESS, &projection_distance);
    if (projection_distance > UINT16_MAX)
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_SOURCE_MISMATCH;

    snapshot.culling.active_mask = 0x60u;
    snapshot.culling.active_value = 0x40u;
    snapshot.culling.state_flags_4_reject_mask = 0x102a00u;
    snapshot.culling.state_flags_0_reject_mask = 0x10000u;
    snapshot.culling.state_flags_14_reject_mask = 0x200002u;
    snapshot.culling.producer_disabled = producer_disabled != 0u;
    snapshot.culling.actor_active =
        (snapshot.actor.active_gate_word & snapshot.culling.active_mask) ==
        snapshot.culling.active_value;
    snapshot.culling.state_visible =
        (snapshot.actor.state_flags_4 &
         snapshot.culling.state_flags_4_reject_mask) == 0u &&
        (snapshot.actor.state_flags_0 &
         snapshot.culling.state_flags_0_reject_mask) == 0u &&
        (snapshot.actor.state_flags_14 &
         snapshot.culling.state_flags_14_reject_mask) == 0u;
    snapshot.material.source_identity =
        XG_FIELD_CHARACTER_SOURCE_MATERIAL_INITIALIZER_8007AA44;
    snapshot.material.present_mask =
        XG_FIELD_CHARACTER_SOURCE_MATERIAL_REQUIRED;
    snapshot.material.red = 0x80u;
    snapshot.material.green = 0x80u;
    snapshot.material.blue = 0x80u;
    snapshot.material.semi_transparent = 1u;
    snapshot.material.u[0] = 0u;
    snapshot.material.u[1] = 15u;
    snapshot.material.u[2] = 0u;
    snapshot.material.u[3] = 15u;
    snapshot.material.v[0] = 224u;
    snapshot.material.v[1] = 224u;
    snapshot.material.v[2] = 239u;
    snapshot.material.v[3] = 239u;
    snapshot.material.tpage = 0x5au;
    snapshot.material.clut_x = 0x100u;
    snapshot.material.clut_y = 0xf3u;
    snapshot.projection.source_identity =
        XG_FIELD_CHARACTER_SOURCE_PROJECTION_SETUP_80074108;
    snapshot.projection.screen_offset_x = 0x00a00000;
    snapshot.projection.screen_offset_y = 0x00700000;
    snapshot.projection.projection_distance = (uint16_t)projection_distance;
    snapshot.projection.average_z_scale4 = 0x100;
    snapshot.projection.screen_offset_fraction_bits = 16u;
    snapshot.projection.depth_cue_output_unused = 1u;
    snapshot.raster = request->raster;
    snapshot.ordering.source_identity =
        XG_FIELD_CHARACTER_SOURCE_ORDERING_800769EC;
    snapshot.ordering.ft4_index = local_ft4_index;
    snapshot.ordering.ordering_shift = (uint8_t)(ordering_shift & 0x1fu);
    snapshot.ordering.ft4_index_from_actor_state = 1u;
    snapshot.ordering.ot_base_from_producer_argument = 1u;
    snapshot.units.matrix_fraction_bits = 12u;
    snapshot.units.scale_input_fraction_bits = 0u;
    snapshot.units.scale_product_fraction_bits = 12u;
    snapshot.units.model_vertex_unit_shift = 0u;
    snapshot.units.model_axis_seed = 0x1000;
    snapshot.units.scale_numerator = 0x0c00;
    snapshot.absence.unresolved_mask = 0u;
    snapshot.absence.rigid_quad_has_no_pose = 1u;
    snapshot.absence.lighting_not_used = 1u;
    snapshot.absence.depth_cue_output_not_consumed = 1u;
    snapshot.absence.packet_is_sink_only = 1u;
    seal_snapshot(&snapshot, &access);
    *out_snapshot = snapshot;

#undef READ_U32
#undef READ_U16

    if (snapshot.culling.producer_disabled != 0u ||
        snapshot.culling.actor_active != 1u ||
        snapshot.culling.state_visible != 1u)
        return XG_FIELD_CHARACTER_SOURCE_CAPTURE_CULLED;
    return XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK;
}
