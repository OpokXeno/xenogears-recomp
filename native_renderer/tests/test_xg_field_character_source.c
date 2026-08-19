#include "xg_field_character_source_adapter.h"
#include "xg_field_character_source_capture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

/* Guest addresses exceed INT_MAX, so they must not be enumeration constants:
 * on targets where such enumerators take type int (clang/MSVC ABI), the
 * negative value sign-extends in 64-bit comparisons like
 * `addr + 4u <= BASE + sizeof(mem)` and the optimizer deletes every
 * subsequent branch of the read handlers. UINT32_C keeps them unsigned
 * everywhere. */
#define ACTOR_BASE UINT32_C(0x80012000)
#define ACTOR_INDEX UINT32_C(2)
#define ACTOR_ADDRESS (ACTOR_BASE + ACTOR_INDEX * 0x5cu)
#define SECONDARY_ADDRESS UINT32_C(0x80013000)
#define STATE_ADDRESS UINT32_C(0x80014000)
#define MODEL_ADDRESS UINT32_C(0x80015000)
#define STACK_ADDRESS UINT32_C(0x801ff000)
#define SCRATCHPAD_STACK_ADDRESS UINT32_C(0x1f8002c0)
#define OT_ADDRESS UINT32_C(0x80018000)
#define FT4_INDEX UINT32_C(1)
#define PACKET_ADDRESS (MODEL_ADDRESS + 0x20u + FT4_INDEX * 0x28u)

typedef struct Fixture {
    uint32_t poison;
    uint32_t stack_address;
    uint16_t model_pad;
    uint32_t read_count;
    uint32_t packet_reads;
    uint32_t ot_reads;
    uint32_t device_reads;
} Fixture;

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static bool read_u32(void *context, uint32_t address, uint32_t *out_value) {
    Fixture *fixture = (Fixture *)context;
    const uint32_t stack_address = fixture->stack_address != 0u
        ? fixture->stack_address : STACK_ADDRESS;
    const uint32_t matrix[8] = {
        0x00001000u, 0x00000000u, 0x00001000u, 0x00000000u,
        0x00001000u, 100u, (uint32_t)-200, 300u,
    };
    const XgFieldCharacterSourceVector vertices[4] = {
        {24, 0, 24, 0u}, {-24, 0, 24, 0u},
        {24, 0, -24, 0u}, {-24, 0, -24, 0u},
    };
    uint32_t offset;

    if (out_value == NULL) return false;
    ++fixture->read_count;
    if (address == stack_address + 0xd0u) {
        *out_value = OT_ADDRESS;
        return true;
    }
    if (address == stack_address + 0xd8u) {
        *out_value = FT4_INDEX;
        return true;
    }
    if (address >= PACKET_ADDRESS && address < PACKET_ADDRESS + 0x28u) {
        ++fixture->packet_reads;
        *out_value = fixture->poison;
        return true;
    }
    if (address >= OT_ADDRESS && address < OT_ADDRESS + 0x100u) {
        ++fixture->ot_reads;
        *out_value = fixture->poison;
        return true;
    }
    if (address >= 0x1f801810u && address < 0x1f801818u) {
        ++fixture->device_reads;
        *out_value = fixture->poison;
        return true;
    }
    switch (address) {
    case 0x8004f37cu: *out_value = 0u; return true;
    case 0x80050100u: *out_value = 2u; return true;
    case 0x800adbfc: *out_value = 4u; return true;
    case 0x800afb10: *out_value = ACTOR_BASE; return true;
    case 0x800b2268: *out_value = 1u; return true;
    case 0x800af9f8: *out_value = 0x80u; return true;
    case ACTOR_ADDRESS + 0x04u: *out_value = SECONDARY_ADDRESS; return true;
    case ACTOR_ADDRESS + 0x08u: *out_value = MODEL_ADDRESS; return true;
    case ACTOR_ADDRESS + 0x20u: *out_value = 1000u; return true;
    case ACTOR_ADDRESS + 0x28u: *out_value = (uint32_t)-3000; return true;
    case ACTOR_ADDRESS + 0x4cu: *out_value = STATE_ADDRESS; return true;
    case ACTOR_ADDRESS + 0x58u: *out_value = 0x40u; return true;
    case STATE_ADDRESS + 0x00u: *out_value = 0x400u; return true;
    case STATE_ADDRESS + 0x04u: *out_value = 0u; return true;
    case STATE_ADDRESS + 0x14u: *out_value = 0u; return true;
    case STATE_ADDRESS + 0x50u: *out_value = 11u; return true;
    case STATE_ADDRESS + 0x54u: *out_value = (uint32_t)-22; return true;
    case STATE_ADDRESS + 0x58u: *out_value = 33u; return true;
    default: break;
    }
    if (address >= 0x800afa64u && address < 0x800afa84u &&
        (address & 3u) == 0u) {
        *out_value = matrix[(address - 0x800afa64u) / 4u];
        return true;
    }
    if (address >= MODEL_ADDRESS && address < MODEL_ADDRESS + 0x20u &&
        (address & 3u) == 0u) {
        offset = address - MODEL_ADDRESS;
        if ((offset & 7u) == 0u)
            *out_value = pack_s16(vertices[offset / 8u].x,
                                  vertices[offset / 8u].y);
        else
            *out_value = pack_s16(vertices[offset / 8u].z,
                                  (int16_t)(fixture->model_pad != 0u
                                      ? fixture->model_pad
                                      : vertices[offset / 8u].pad));
        return true;
    }
    return false;
}

static bool read_u16(void *context, uint32_t address, uint16_t *out_value) {
    Fixture *fixture = (Fixture *)context;

    if (out_value == NULL) return false;
    ++fixture->read_count;
    switch (address) {
    case SECONDARY_ADDRESS + 0x84u: *out_value = (uint16_t)-2000; return true;
    case STATE_ADDRESS + 0xf4u: *out_value = 0x1000u; return true;
    case STATE_ADDRESS + 0xf6u: *out_value = 0x0800u; return true;
    case STATE_ADDRESS + 0xf8u: *out_value = 0x0400u; return true;
    default: return false;
    }
}

static XgFieldCharacterSourceCaptureRequest request(void) {
    XgFieldCharacterSourceCaptureRequest value = {0};
    size_t index;

    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE; ++index) {
        value.game_sha256[index] = (uint8_t)(index + 1u);
        value.manifest_sha256[index] = (uint8_t)(0x80u + index);
    }
    value.source_generation = 19u;
    value.scene_generation = 7u;
    value.visual_state.scene_epoch = 5u;
    value.visual_state.state_sequence = 9u;
    value.vram_mutation_serial = 23u;
    value.producer_record_id = 3u;
    value.producer_entry = UINT32_C(0x800764b4);
    value.actor_index = ACTOR_INDEX;
    value.actor_record_address = ACTOR_ADDRESS;
    value.model_address = MODEL_ADDRESS;
    value.producer_stack_pointer = STACK_ADDRESS;
    value.producer_ft4_index = FT4_INDEX;
    value.raster.draw_area_right = 319u;
    value.raster.draw_area_bottom = 239u;
    value.raster.draw_offset_x = -12;
    value.raster.draw_offset_y = 8;
    value.raster.dither = 1u;
    value.raster.mask_check = 1u;
    return value;
}

static int test_capture_is_value_only_and_records_absence(void) {
    Fixture fixture = {0};
    XgFieldCharacterSourceCaptureRequest capture_request = request();
    XgFieldCharacterAuthenticatedReader reader = {
        &fixture, read_u16, read_u32, 19u, 1u,
    };
    XgFieldCharacterSourceSnapshot snapshot;
    XgFieldCharacterCandidate candidate;
    XgFieldCharacterSourceDerived derived;

    CHECK(xg_field_character_source_capture(&capture_request, &reader,
                                             &snapshot) ==
          XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK);
    CHECK(snapshot.sealed == 1u && snapshot.authenticated == 1u);
    CHECK(snapshot.presence_mask == XG_FIELD_CHARACTER_SOURCE_REQUIRED_PRESENCE);
    CHECK(snapshot.identity.actor_index == ACTOR_INDEX);
    CHECK(snapshot.identity.actor_count == 4u);
    CHECK(snapshot.generation.vram_mutation_serial == 23u);
    CHECK(snapshot.actor.world_offset[0] == 1000);
    CHECK(snapshot.actor.world_offset[1] == -2000);
    CHECK(snapshot.actor.world_offset[2] == -3000);
    CHECK(snapshot.actor.scale_reduced == 1u);
    CHECK(snapshot.actor.scale_shift == 14u);
    CHECK(snapshot.model_vertices[1].x == -24);
    CHECK(snapshot.source_matrix.source_identity ==
          XG_FIELD_CHARACTER_SOURCE_MATRIX_FIELD_CAMERA_800AFA64);
    CHECK(snapshot.source_matrix.rotation[0] == 0x1000);
    CHECK(snapshot.source_matrix.rotation[4] == 0x1000);
    CHECK(snapshot.source_matrix.rotation[8] == 0x1000);
    CHECK(snapshot.source_matrix.translation[1] == -200);
    CHECK(snapshot.ordering.ft4_index == FT4_INDEX);
    CHECK(snapshot.ordering.ordering_shift == 2u);
    CHECK(snapshot.ordering.ft4_index_from_actor_state == 1u);
    CHECK(snapshot.ordering.ot_base_from_producer_argument == 1u);
    CHECK(snapshot.material.source_identity ==
          XG_FIELD_CHARACTER_SOURCE_MATERIAL_INITIALIZER_8007AA44);
    CHECK(snapshot.material.present_mask ==
          XG_FIELD_CHARACTER_SOURCE_MATERIAL_REQUIRED);
    CHECK(snapshot.material.tpage == 0x5au);
    CHECK(snapshot.material.clut_x == 0x100u);
    CHECK(snapshot.material.clut_y == 0xf3u);
    CHECK(snapshot.projection.projection_distance == 0x80u);
    CHECK(snapshot.projection.screen_offset_x == 0x00a00000);
    CHECK(snapshot.projection.screen_offset_y == 0x00700000);
    CHECK(snapshot.absence.packet_arena_read_count == 0u);
    CHECK(snapshot.absence.ot_payload_read_count == 0u);
    CHECK(snapshot.absence.vram_read_count == 0u);
    CHECK(snapshot.absence.post_gte_read_count == 0u);
    CHECK(snapshot.absence.general_guest_read_count == 0u);
    CHECK(snapshot.absence.unresolved_mask == 0u);
    CHECK(snapshot.absence.rigid_quad_has_no_pose == 1u);
    CHECK(snapshot.absence.lighting_not_used == 1u);
    CHECK(snapshot.absence.depth_cue_output_not_consumed == 1u);
    CHECK(snapshot.absence.packet_is_sink_only == 1u);
    CHECK(xg_field_character_source_digest_matches(&snapshot));
    CHECK(xg_field_character_source_adapter_build(
              &snapshot, &candidate, &derived) ==
          XG_FIELD_CHARACTER_SOURCE_ADAPTER_OK);
    CHECK(candidate.finalized == 1u);
    CHECK(candidate.tpage == 0x5au);
    CHECK(fixture.packet_reads == 0u && fixture.ot_reads == 0u &&
          fixture.device_reads == 0u);
    return 1;
}

static int test_forbidden_poison_does_not_change_snapshot_digest(void) {
    Fixture first = {.poison = 0x11111111u};
    Fixture second = {.poison = 0xeeeeeeeeu};
    XgFieldCharacterSourceCaptureRequest capture_request = request();
    XgFieldCharacterAuthenticatedReader first_reader = {
        &first, read_u16, read_u32, 19u, 1u,
    };
    XgFieldCharacterAuthenticatedReader second_reader = {
        &second, read_u16, read_u32, 19u, 1u,
    };
    XgFieldCharacterSourceSnapshot first_snapshot;
    XgFieldCharacterSourceSnapshot second_snapshot;

    CHECK(xg_field_character_source_capture(&capture_request, &first_reader,
                                             &first_snapshot) ==
          XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK);
    CHECK(xg_field_character_source_capture(&capture_request, &second_reader,
                                             &second_snapshot) ==
          XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK);
    CHECK(memcmp(first_snapshot.value_digest, second_snapshot.value_digest,
                 XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE) == 0);
    CHECK(first.packet_reads == 0u && first.ot_reads == 0u &&
          first.device_reads == 0u);
    CHECK(second.packet_reads == 0u && second.ot_reads == 0u &&
          second.device_reads == 0u);
    return 1;
}

static int test_digest_and_authentication_fail_closed(void) {
    Fixture fixture = {0};
    XgFieldCharacterSourceCaptureRequest capture_request = request();
    XgFieldCharacterAuthenticatedReader reader = {
        &fixture, read_u16, read_u32, 19u, 1u,
    };
    XgFieldCharacterSourceSnapshot snapshot;
    XgFieldCharacterCandidate candidate;
    XgFieldCharacterSourceDerived derived;

    CHECK(xg_field_character_source_capture(&capture_request, &reader,
                                             &snapshot) ==
          XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK);
    snapshot.actor.world_offset[0] ^= 1;
    CHECK(!xg_field_character_source_digest_matches(&snapshot));
    CHECK(xg_field_character_source_adapter_build(
              &snapshot, &candidate, &derived) ==
          XG_FIELD_CHARACTER_SOURCE_ADAPTER_INVALID_SNAPSHOT);
    reader.authentication_generation++;
    CHECK(xg_field_character_source_capture(&capture_request, &reader,
                                             &snapshot) ==
          XG_FIELD_CHARACTER_SOURCE_CAPTURE_UNAUTHENTICATED);
    return 1;
}

static int test_capture_accepts_authenticated_scratchpad_stack(void) {
    Fixture fixture = {.stack_address = SCRATCHPAD_STACK_ADDRESS};
    XgFieldCharacterSourceCaptureRequest capture_request = request();
    XgFieldCharacterAuthenticatedReader reader = {
        &fixture, read_u16, read_u32, 19u, 1u,
    };
    XgFieldCharacterSourceSnapshot snapshot;

    capture_request.producer_stack_pointer = SCRATCHPAD_STACK_ADDRESS;
    CHECK(xg_field_character_source_capture(&capture_request, &reader,
                                             &snapshot) ==
          XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK);
    CHECK(snapshot.sealed == 1u && snapshot.authenticated == 1u);
    CHECK(snapshot.ordering.ft4_index == FT4_INDEX);
    return 1;
}

static int test_capture_preserves_nonsemantic_model_padding(void) {
    Fixture fixture = {.model_pad = 0x400cu};
    XgFieldCharacterSourceCaptureRequest capture_request = request();
    XgFieldCharacterAuthenticatedReader reader = {
        &fixture, read_u16, read_u32, 19u, 1u,
    };
    XgFieldCharacterSourceSnapshot snapshot;
    XgFieldCharacterCandidate candidate;
    XgFieldCharacterSourceDerived derived;

    CHECK(xg_field_character_source_capture(&capture_request, &reader,
                                             &snapshot) ==
          XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK);
    CHECK(snapshot.model_vertices[0].pad == 0x400cu);
    CHECK(xg_field_character_source_adapter_build(
              &snapshot, &candidate, &derived) ==
          XG_FIELD_CHARACTER_SOURCE_ADAPTER_OK);
    CHECK(candidate.finalized == 1u);
    return 1;
}

int main(void) {
    return !(test_capture_is_value_only_and_records_absence() &&
             test_forbidden_poison_does_not_change_snapshot_digest() &&
             test_digest_and_authentication_fail_closed() &&
             test_capture_accepts_authenticated_scratchpad_stack() &&
             test_capture_preserves_nonsemantic_model_padding());
}
