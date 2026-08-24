#include "xg_render_private_compare.h"

#include "xg_field_character_adapter.h"

#include <stddef.h>
#include <string.h>

static uint16_t fixed_to_u16(int32_t value) {
    return (uint16_t)((uint32_t)value >> XG_FIELD_CHARACTER_FIXED_FRACTION_BITS);
}

static uint8_t fixed_to_u8(int32_t value) {
    return (uint8_t)((uint32_t)value >> XG_FIELD_CHARACTER_FIXED_FRACTION_BITS);
}

static uint32_t packet_word_mask(size_t index) {
    return index == 6u || index == 8u
        ? UINT32_C(0x0000ffff) : UINT32_MAX;
}

static int candidate_is_canonical(const XgFieldCharacterCandidate *candidate) {
    XgRenderIrNativePrimitive primitive;

    return xg_field_character_adapter_build_primitive(candidate, &primitive) ==
           XG_FIELD_CHARACTER_ADAPTER_OK;
}

static uint32_t xy_word(const XgFieldCharacterCandidateVertex *vertex) {
    return (uint32_t)fixed_to_u16(vertex->x) |
           ((uint32_t)fixed_to_u16(vertex->y) << 16u);
}

static uint32_t uv_word(const XgFieldCharacterCandidateVertex *vertex,
                        uint16_t upper) {
    return (uint32_t)fixed_to_u8(vertex->u) |
           ((uint32_t)fixed_to_u8(vertex->v) << 8u) |
           ((uint32_t)upper << 16u);
}

static void encode_candidate(const XgFieldCharacterCandidate *candidate,
                             uint32_t words[XG_RENDER_PRIVATE_FT4_WORD_COUNT]) {
    const uint16_t clut = (uint16_t)((candidate->clut_x >> 4u) |
                                     (candidate->clut_y << 6u));

    words[0] = ((uint32_t)(XG_FIELD_CHARACTER_FAMILY_OPCODE |
                           (candidate->semi_transparent ? 2u : 0u)) << 24u) |
               ((uint32_t)candidate->vertices[0].blue << 16u) |
               ((uint32_t)candidate->vertices[0].green << 8u) |
               candidate->vertices[0].red;
    words[1] = xy_word(&candidate->vertices[0]);
    words[2] = uv_word(&candidate->vertices[0], clut);
    words[3] = xy_word(&candidate->vertices[1]);
    words[4] = uv_word(&candidate->vertices[1], candidate->tpage);
    words[5] = xy_word(&candidate->vertices[2]);
    words[6] = uv_word(&candidate->vertices[2], 0u);
    words[7] = xy_word(&candidate->vertices[3]);
    words[8] = uv_word(&candidate->vertices[3], 0u);
}

static int draw_state_matches(const XgFieldCharacterCandidate *candidate,
                              const XgRenderPrivateDrawState *original) {
    return candidate->draw_area_left == original->draw_area_left &&
           candidate->draw_area_top == original->draw_area_top &&
           candidate->draw_area_right == original->draw_area_right &&
           candidate->draw_area_bottom == original->draw_area_bottom &&
           candidate->draw_offset_x == original->draw_offset_x &&
           candidate->draw_offset_y == original->draw_offset_y &&
           candidate->texture_window_mask_x == original->texture_window_mask_x &&
           candidate->texture_window_mask_y == original->texture_window_mask_y &&
           candidate->texture_window_offset_x == original->texture_window_offset_x &&
           candidate->texture_window_offset_y == original->texture_window_offset_y &&
           candidate->dither == original->dither &&
           candidate->mask_set == original->mask_set &&
           candidate->mask_check == original->mask_check;
}

static void copy_primitive(const XgFieldCharacterCandidate *candidate,
                            XgRenderIrNativePrimitive *out_primitive) {
    (void)xg_field_character_adapter_build_primitive(candidate, out_primitive);
}

XgRenderPrivateCompareResult xg_render_private_compare_field_character_detailed(
    const XgFieldCharacterCandidate *candidate,
    const XgRenderPrivateOriginalPacket *original,
    uint32_t authenticated_ot_bucket,
    XgRenderIrNativePrimitive *out_primitive,
    uint32_t *out_mismatch_word,
    uint32_t *out_mismatch_byte) {
    uint32_t candidate_words[XG_RENDER_PRIVATE_FT4_WORD_COUNT];
    size_t index;

    if (!candidate || !original || !out_primitive)
        return XG_RENDER_PRIVATE_COMPARE_INVALID_ARGUMENT;
    if (out_mismatch_word) *out_mismatch_word = UINT32_MAX;
    if (out_mismatch_byte) *out_mismatch_byte = UINT32_MAX;
    memset(out_primitive, 0, sizeof(*out_primitive));
    if (candidate->finalized != 1u)
        return XG_RENDER_PRIVATE_COMPARE_CANDIDATE_NOT_FINALIZED;
    if (!candidate_is_canonical(candidate))
        return XG_RENDER_PRIVATE_COMPARE_CANDIDATE_INVALID;
    if (!original->words)
        return XG_RENDER_PRIVATE_COMPARE_ORIGINAL_UNAVAILABLE;
    if (original->word_count != XG_RENDER_PRIVATE_FT4_WORD_COUNT)
        return XG_RENDER_PRIVATE_COMPARE_PACKET_LENGTH_MISMATCH;
    if (original->ot_bucket != authenticated_ot_bucket)
        return XG_RENDER_PRIVATE_COMPARE_OT_BUCKET_MISMATCH;
    if (!draw_state_matches(candidate, &original->draw_state))
        return XG_RENDER_PRIVATE_COMPARE_DRAW_STATE_MISMATCH;
    encode_candidate(candidate, candidate_words);
    for (index = 0u; index < XG_RENDER_PRIVATE_FT4_WORD_COUNT; ++index) {
        const uint32_t mask = packet_word_mask(index);

        if ((candidate_words[index] & mask) !=
            (original->words[index] & mask)) {
            uint32_t byte;

            if (out_mismatch_word) *out_mismatch_word = (uint32_t)index;
            for (byte = 0u; byte < 4u; ++byte) {
                const uint32_t byte_mask = UINT32_C(0xff) << (byte * 8u);

                if ((mask & byte_mask) != 0u &&
                    ((candidate_words[index] & byte_mask) !=
                     (original->words[index] & byte_mask))) {
                    if (out_mismatch_byte)
                        *out_mismatch_byte = (uint32_t)index * 4u + byte;
                    break;
                }
            }
            return XG_RENDER_PRIVATE_COMPARE_PACKET_MISMATCH;
        }
    }
    copy_primitive(candidate, out_primitive);
    return XG_RENDER_PRIVATE_COMPARE_EQUAL;
}

XgRenderPrivateCompareResult xg_render_private_compare_field_character(
    const XgFieldCharacterCandidate *candidate,
    const XgRenderPrivateOriginalPacket *original,
    uint32_t authenticated_ot_bucket,
    XgRenderIrNativePrimitive *out_primitive) {
    return xg_render_private_compare_field_character_detailed(
        candidate, original, authenticated_ot_bucket, out_primitive, NULL,
        NULL);
}
