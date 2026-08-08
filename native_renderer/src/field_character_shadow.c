#include "field_character_shadow_internal.h"

#include <string.h>

FieldCharacterShadowResult field_character_shadow_block_internal(
    FieldCharacterShadow *shadow, FieldCharacterShadowBlocker blocker) {
    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    if (shadow->phase != FIELD_CHARACTER_SHADOW_PHASE_BLOCKED) {
        shadow->phase = FIELD_CHARACTER_SHADOW_PHASE_BLOCKED;
        shadow->blocker = blocker;
        shadow->open = 0u;
        shadow->has_selection = 0u;
        memset(&shadow->selection, 0, sizeof(shadow->selection));
    }
    return FIELD_CHARACTER_SHADOW_RESULT_BLOCKED;
}

FieldCharacterShadowResult field_character_shadow_require_open_internal(
    FieldCharacterShadow *shadow) {
    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    if (shadow->phase == FIELD_CHARACTER_SHADOW_PHASE_BLOCKED)
        return FIELD_CHARACTER_SHADOW_RESULT_BLOCKED;
    if (shadow->phase != FIELD_CHARACTER_SHADOW_PHASE_COLLECTING || !shadow->open)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_UNAUTHENTICATED_OBSERVATION);
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

int field_character_shadow_increment_internal(uint64_t *counter) {
    if (*counter == UINT64_MAX) return 0;
    *counter += 1u;
    return 1;
}

static int auth_is_valid(FieldCharacterShadowAuth auth) {
    return auth.authenticated == 1u && auth.site != 0u && auth.sequence != 0u;
}

static int auth_matches(FieldCharacterShadowAuth left,
                        FieldCharacterShadowAuth right) {
    return left.authenticated == right.authenticated && left.site == right.site &&
           left.sequence == right.sequence;
}

static FieldCharacterShadowSite *site_for(FieldCharacterShadow *shadow,
                                           uint32_t site) {
    for (size_t index = 0u; index < shadow->site_count; ++index) {
        if (shadow->sites[index].site == site) return &shadow->sites[index];
    }
    return NULL;
}

static int family_is_better(const FieldCharacterShadowFamily *candidate,
                            const FieldCharacterShadowFamily *best) {
    if (best == NULL || candidate->count != best->count)
        return best == NULL || candidate->count > best->count;
    if (candidate->opcode != best->opcode) return candidate->opcode < best->opcode;
    if (candidate->parser_word_count != best->parser_word_count)
        return candidate->parser_word_count < best->parser_word_count;
    if (candidate->producer_store_pc != best->producer_store_pc)
        return candidate->producer_store_pc < best->producer_store_pc;
    return candidate->site < best->site;
}

void field_character_shadow_init(FieldCharacterShadow *shadow) {
    if (shadow == NULL) return;
    memset(shadow, 0, sizeof(*shadow));
    shadow->phase = FIELD_CHARACTER_SHADOW_PHASE_IDLE;
}

FieldCharacterShadowResult field_character_shadow_begin(
    FieldCharacterShadow *shadow, FieldCharacterShadowAuth auth,
    uint64_t guest_vblank) {
    FieldCharacterShadowSite *site;

    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    if (shadow->phase == FIELD_CHARACTER_SHADOW_PHASE_BLOCKED)
        return FIELD_CHARACTER_SHADOW_RESULT_BLOCKED;
    if (shadow->phase == FIELD_CHARACTER_SHADOW_PHASE_COMPLETE)
        return FIELD_CHARACTER_SHADOW_RESULT_INVALID_STATE;
    if (!auth_is_valid(auth))
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_AUTH);
    if (shadow->open)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_NESTED_BEGIN);
    if (shadow->phase == FIELD_CHARACTER_SHADOW_PHASE_IDLE) {
        shadow->phase = FIELD_CHARACTER_SHADOW_PHASE_COLLECTING;
        shadow->start_guest_vblank = guest_vblank;
    }
    site = site_for(shadow, auth.site);
    if (site == NULL) {
        if (shadow->site_count == FIELD_CHARACTER_SHADOW_SITE_CAPACITY)
            return field_character_shadow_block_internal(
                shadow, FIELD_CHARACTER_SHADOW_BLOCKER_SITE_CAPACITY);
        site = &shadow->sites[shadow->site_count++];
        site->site = auth.site;
    }
    if (auth.sequence <= site->last_sequence)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_STALE_AUTH);
    site->last_sequence = auth.sequence;
    shadow->active_auth = auth;
    shadow->open = 1u;
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

FieldCharacterShadowResult field_character_shadow_end(
    FieldCharacterShadow *shadow, FieldCharacterShadowAuth auth) {
    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    if (shadow->phase == FIELD_CHARACTER_SHADOW_PHASE_BLOCKED)
        return FIELD_CHARACTER_SHADOW_RESULT_BLOCKED;
    if (shadow->phase != FIELD_CHARACTER_SHADOW_PHASE_COLLECTING || !shadow->open)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_UNMATCHED_END);
    if (auth.authenticated != 1u)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_AUTH_LOSS);
    if (!auth_matches(shadow->active_auth, auth))
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_STALE_AUTH);
    shadow->open = 0u;
    memset(&shadow->active_auth, 0, sizeof(shadow->active_auth));
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

FieldCharacterShadowResult field_character_shadow_auth_lost(
    FieldCharacterShadow *shadow) {
    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    if (shadow->phase == FIELD_CHARACTER_SHADOW_PHASE_BLOCKED)
        return FIELD_CHARACTER_SHADOW_RESULT_BLOCKED;
    if (shadow->phase != FIELD_CHARACTER_SHADOW_PHASE_COLLECTING)
        return FIELD_CHARACTER_SHADOW_RESULT_INVALID_STATE;
    return field_character_shadow_block_internal(
        shadow, FIELD_CHARACTER_SHADOW_BLOCKER_AUTH_LOSS);
}

FieldCharacterShadowResult field_character_shadow_finalize(
    FieldCharacterShadow *shadow, uint64_t guest_vblank) {
    const FieldCharacterShadowFamily *best = NULL;
    uint64_t elapsed;

    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    if (shadow->phase == FIELD_CHARACTER_SHADOW_PHASE_BLOCKED)
        return FIELD_CHARACTER_SHADOW_RESULT_BLOCKED;
    if (shadow->phase != FIELD_CHARACTER_SHADOW_PHASE_COLLECTING)
        return FIELD_CHARACTER_SHADOW_RESULT_INVALID_STATE;
    if (shadow->open)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_UNMATCHED_END);
    if (guest_vblank < shadow->start_guest_vblank)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_EARLY_WINDOW);
    elapsed = guest_vblank - shadow->start_guest_vblank;
    if (elapsed < FIELD_CHARACTER_SHADOW_SELECTION_VBLANKS)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_EARLY_WINDOW);
    if (elapsed > FIELD_CHARACTER_SHADOW_SELECTION_VBLANKS)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_LATE_WINDOW);
    for (size_t index = 0u; index < shadow->family_count; ++index) {
        const FieldCharacterShadowFamily *candidate = &shadow->families[index];

        if (candidate->count >= FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT &&
            family_is_better(candidate, best))
            best = candidate;
    }
    if (best == NULL)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_INSUFFICIENT_COUNT);
    if (shadow->coverage_mask !=
        (uint8_t)((UINT8_C(1) << FIELD_CHARACTER_SHADOW_COVERAGE_COUNT) - 1u))
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_INSUFFICIENT_COVERAGE);
    shadow->selection.site = best->site;
    shadow->selection.producer_store_pc = best->producer_store_pc;
    shadow->selection.parser_word_count = best->parser_word_count;
    shadow->selection.opcode = best->opcode;
    shadow->selection.count = best->count;
    shadow->has_selection = 1u;
    shadow->end_guest_vblank = guest_vblank;
    shadow->phase = FIELD_CHARACTER_SHADOW_PHASE_COMPLETE;
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

FieldCharacterShadowResult field_character_shadow_snapshot(
    const FieldCharacterShadow *shadow, FieldCharacterShadowSummary *out_summary) {
    if (shadow == NULL || out_summary == NULL)
        return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->phase = shadow->phase;
    out_summary->blocker = shadow->blocker;
    out_summary->start_guest_vblank = shadow->start_guest_vblank;
    out_summary->end_guest_vblank = shadow->end_guest_vblank;
    out_summary->allocator_effect_count = shadow->allocator_effect_count;
    out_summary->ot_effect_count = shadow->ot_effect_count;
    out_summary->gpu_effect_count = shadow->gpu_effect_count;
    out_summary->family_count = shadow->family_count;
    out_summary->access_count = shadow->access_count;
    out_summary->site_count = shadow->site_count;
    out_summary->coverage_mask = shadow->coverage_mask;
    out_summary->has_selection = shadow->has_selection;
    out_summary->selection = shadow->selection;
    memcpy(out_summary->accesses, shadow->accesses, sizeof(out_summary->accesses));
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

#ifdef FIELD_CHARACTER_SHADOW_TESTING
FieldCharacterShadowResult field_character_shadow_test_seed_family_count(
    FieldCharacterShadow *shadow, size_t index, uint64_t count) {
    if (shadow == NULL || index >= shadow->family_count)
        return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    shadow->families[index].count = count;
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}
#endif
