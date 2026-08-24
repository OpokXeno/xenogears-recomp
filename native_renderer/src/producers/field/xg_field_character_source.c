#include "xg_field_character_source.h"

#include <stddef.h>
#include <string.h>

typedef struct XgFieldCharacterDigestState {
    uint32_t value[8];
} XgFieldCharacterDigestState;

static void digest_byte(XgFieldCharacterDigestState *state, uint8_t value) {
    size_t index;

    for (index = 0u; index < 8u; ++index) {
        state->value[index] ^= (uint32_t)value + (uint32_t)(index * 0x3du);
        state->value[index] *= UINT32_C(0x01000193);
        state->value[index] ^= state->value[index] >> (13u + (index & 3u));
    }
}

static void digest_u8(XgFieldCharacterDigestState *state, uint8_t value) {
    digest_byte(state, value);
}

static void digest_u16(XgFieldCharacterDigestState *state, uint16_t value) {
    digest_byte(state, (uint8_t)value);
    digest_byte(state, (uint8_t)(value >> 8u));
}

static void digest_u32(XgFieldCharacterDigestState *state, uint32_t value) {
    unsigned shift;

    for (shift = 0u; shift < 32u; shift += 8u)
        digest_byte(state, (uint8_t)(value >> shift));
}

static void digest_u64(XgFieldCharacterDigestState *state, uint64_t value) {
    unsigned shift;

    for (shift = 0u; shift < 64u; shift += 8u)
        digest_byte(state, (uint8_t)(value >> shift));
}

static void digest_bytes(XgFieldCharacterDigestState *state,
                         const uint8_t *values, size_t count) {
    size_t index;

    for (index = 0u; index < count; ++index) digest_byte(state, values[index]);
}

void xg_field_character_source_digest(
    const XgFieldCharacterSourceSnapshot *snapshot,
    uint8_t out_digest[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE]) {
    XgFieldCharacterDigestState state = {{
        UINT32_C(0x811c9dc5), UINT32_C(0x9e3779b9),
        UINT32_C(0x85ebca6b), UINT32_C(0xc2b2ae35),
        UINT32_C(0x27d4eb2f), UINT32_C(0x165667b1),
        UINT32_C(0xd3a2646c), UINT32_C(0xfd7046c5),
    }};
    size_t index;

    if (out_digest == NULL) return;
    memset(out_digest, 0, XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE);
    if (snapshot == NULL) return;

    digest_u32(&state, snapshot->schema_version);
    digest_u32(&state, snapshot->presence_mask);
    digest_bytes(&state, snapshot->identity.game_sha256,
                 XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE);
    digest_bytes(&state, snapshot->identity.manifest_sha256,
                 XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE);
    digest_u32(&state, snapshot->identity.producer_entry);
    digest_u32(&state, snapshot->identity.producer_record_id);
    digest_u32(&state, snapshot->identity.actor_index);
    digest_u32(&state, snapshot->identity.actor_count);
    digest_u32(&state, snapshot->identity.model_initializer);
    digest_u64(&state, snapshot->generation.source_generation);
    digest_u64(&state, snapshot->generation.scene_generation);
    digest_u64(&state, snapshot->generation.visual_state.scene_epoch);
    digest_u64(&state, snapshot->generation.visual_state.state_sequence);
    digest_u64(&state, snapshot->generation.vram_mutation_serial);
    digest_u32(&state, snapshot->actor.active_gate_word);
    digest_u32(&state, snapshot->actor.state_flags_0);
    digest_u32(&state, snapshot->actor.state_flags_4);
    digest_u32(&state, snapshot->actor.state_flags_14);
    for (index = 0u; index < 3u; ++index)
        digest_u16(&state, (uint16_t)snapshot->actor.orientation[index]);
    for (index = 0u; index < 3u; ++index)
        digest_u32(&state, (uint32_t)snapshot->actor.world_offset[index]);
    for (index = 0u; index < 3u; ++index)
        digest_u16(&state, (uint16_t)snapshot->actor.shadow_scale[index]);
    digest_u8(&state, snapshot->actor.scale_shift);
    digest_u8(&state, snapshot->actor.scale_reduced);
    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT; ++index) {
        digest_u16(&state, (uint16_t)snapshot->model_vertices[index].x);
        digest_u16(&state, (uint16_t)snapshot->model_vertices[index].y);
        digest_u16(&state, (uint16_t)snapshot->model_vertices[index].z);
        digest_u16(&state, snapshot->model_vertices[index].pad);
    }
    digest_u32(&state, snapshot->source_matrix.source_identity);
    for (index = 0u; index < XG_FIELD_CHARACTER_SOURCE_MATRIX_ELEMENT_COUNT;
         ++index)
        digest_u16(&state, (uint16_t)snapshot->source_matrix.rotation[index]);
    for (index = 0u; index < 3u; ++index)
        digest_u32(&state, (uint32_t)snapshot->source_matrix.translation[index]);
    digest_u16(&state, (uint16_t)snapshot->source_matrix.source_pad);
    digest_u32(&state, snapshot->culling.active_mask);
    digest_u32(&state, snapshot->culling.active_value);
    digest_u32(&state, snapshot->culling.state_flags_4_reject_mask);
    digest_u32(&state, snapshot->culling.state_flags_0_reject_mask);
    digest_u32(&state, snapshot->culling.state_flags_14_reject_mask);
    digest_u8(&state, snapshot->culling.producer_disabled);
    digest_u8(&state, snapshot->culling.actor_active);
    digest_u8(&state, snapshot->culling.state_visible);
    digest_u32(&state, snapshot->material.source_identity);
    digest_u32(&state, snapshot->material.present_mask);
    digest_u8(&state, snapshot->material.red);
    digest_u8(&state, snapshot->material.green);
    digest_u8(&state, snapshot->material.blue);
    digest_u8(&state, snapshot->material.semi_transparent);
    digest_bytes(&state, snapshot->material.u,
                 XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT);
    digest_bytes(&state, snapshot->material.v,
                 XG_FIELD_CHARACTER_SOURCE_VERTEX_COUNT);
    digest_u16(&state, snapshot->material.tpage);
    digest_u16(&state, snapshot->material.clut_x);
    digest_u16(&state, snapshot->material.clut_y);
    digest_u32(&state, snapshot->projection.source_identity);
    digest_u32(&state, (uint32_t)snapshot->projection.screen_offset_x);
    digest_u32(&state, (uint32_t)snapshot->projection.screen_offset_y);
    digest_u16(&state, snapshot->projection.projection_distance);
    digest_u16(&state, (uint16_t)snapshot->projection.average_z_scale4);
    digest_u8(&state, snapshot->projection.screen_offset_fraction_bits);
    digest_u8(&state, snapshot->projection.depth_cue_output_unused);
    digest_u16(&state, snapshot->raster.draw_area_left);
    digest_u16(&state, snapshot->raster.draw_area_top);
    digest_u16(&state, snapshot->raster.draw_area_right);
    digest_u16(&state, snapshot->raster.draw_area_bottom);
    digest_u16(&state, (uint16_t)snapshot->raster.draw_offset_x);
    digest_u16(&state, (uint16_t)snapshot->raster.draw_offset_y);
    digest_u8(&state, snapshot->raster.texture_window_mask_x);
    digest_u8(&state, snapshot->raster.texture_window_mask_y);
    digest_u8(&state, snapshot->raster.texture_window_offset_x);
    digest_u8(&state, snapshot->raster.texture_window_offset_y);
    digest_u8(&state, snapshot->raster.dither);
    digest_u8(&state, snapshot->raster.mask_set);
    digest_u8(&state, snapshot->raster.mask_check);
    digest_u32(&state, snapshot->ordering.source_identity);
    digest_u32(&state, snapshot->ordering.ft4_index);
    digest_u32(&state, snapshot->ordering.ordering_depth);
    digest_u32(&state, snapshot->ordering.ordering_bucket);
    digest_u8(&state, snapshot->ordering.ordering_shift);
    digest_u8(&state, snapshot->ordering.depth_present);
    digest_u8(&state, snapshot->ordering.bucket_present);
    digest_u8(&state, snapshot->ordering.ft4_index_from_actor_state);
    digest_u8(&state, snapshot->ordering.ot_base_from_producer_argument);
    digest_u8(&state, snapshot->units.matrix_fraction_bits);
    digest_u8(&state, snapshot->units.scale_input_fraction_bits);
    digest_u8(&state, snapshot->units.scale_product_fraction_bits);
    digest_u8(&state, snapshot->units.model_vertex_unit_shift);
    digest_u16(&state, (uint16_t)snapshot->units.model_axis_seed);
    digest_u16(&state, (uint16_t)snapshot->units.scale_numerator);
    digest_u32(&state, snapshot->absence.unresolved_mask);
    digest_u32(&state, snapshot->absence.authenticated_read_count);
    digest_u32(&state, snapshot->absence.authenticated_read_bytes);
    digest_u32(&state, snapshot->absence.packet_arena_read_count);
    digest_u32(&state, snapshot->absence.ot_payload_read_count);
    digest_u32(&state, snapshot->absence.vram_read_count);
    digest_u32(&state, snapshot->absence.post_gte_read_count);
    digest_u32(&state, snapshot->absence.general_guest_read_count);
    digest_u8(&state, snapshot->absence.rigid_quad_has_no_pose);
    digest_u8(&state, snapshot->absence.lighting_not_used);
    digest_u8(&state, snapshot->absence.depth_cue_output_not_consumed);
    digest_u8(&state, snapshot->absence.packet_is_sink_only);
    digest_u8(&state, snapshot->authenticated);
    digest_u8(&state, snapshot->sealed);
    for (index = 0u; index < 8u; ++index) {
        out_digest[index * 4u] = (uint8_t)state.value[index];
        out_digest[index * 4u + 1u] = (uint8_t)(state.value[index] >> 8u);
        out_digest[index * 4u + 2u] = (uint8_t)(state.value[index] >> 16u);
        out_digest[index * 4u + 3u] = (uint8_t)(state.value[index] >> 24u);
    }
}

bool xg_field_character_source_digest_matches(
    const XgFieldCharacterSourceSnapshot *snapshot) {
    uint8_t digest[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE];

    if (snapshot == NULL) return false;
    xg_field_character_source_digest(snapshot, digest);
    return memcmp(digest, snapshot->value_digest, sizeof(digest)) == 0;
}
