#include "field_character_shadow.h"
#include "field_character_shadow_test_adapter.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

_Static_assert(FIELD_CHARACTER_SHADOW_FAMILY_CAPACITY == 64u,
               "family capacity is part of the fixed contract");
_Static_assert(FIELD_CHARACTER_SHADOW_ACCESS_CAPACITY == 256u,
               "access capacity is part of the fixed contract");
_Static_assert(FIELD_CHARACTER_SHADOW_SITE_CAPACITY == 16u,
               "site capacity is part of the fixed contract");
_Static_assert(FIELD_CHARACTER_SHADOW_COVERAGE_COUNT == 4u,
               "all four coverage categories are required");
_Static_assert(FIELD_CHARACTER_SHADOW_SELECTION_VBLANKS == 3600u,
               "selection is fixed at 3600 guest VBlanks");
_Static_assert(FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT == 1000u,
               "selection threshold is fixed");
_Static_assert(FIELD_CHARACTER_SHADOW_SUMMARY_METADATA_ONLY == 1,
               "summaries must remain metadata only");

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

static FieldCharacterShadowAuth authenticated(uint32_t site, uint64_t sequence) {
    FieldCharacterShadowAuth auth = {0};

    auth.site = site;
    auth.sequence = sequence;
    auth.authenticated = 1u;
    return auth;
}

static GpuRenderOraclePacket textured_packet(uint8_t opcode, uint16_t length) {
    GpuRenderOraclePacket packet = {0};

    packet.opcode = opcode;
    packet.parser_word_count = length;
    packet.parser_class = GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_TEXTURED;
    packet.task11_family_eligible = 1u;
    return packet;
}

static int observe_family(FieldCharacterShadow *shadow,
                          const GpuRenderOraclePacket *packet,
                          uint32_t store_pc, uint32_t count) {
    for (uint32_t index = 0u; index < count; ++index) {
        if (field_character_shadow_observe_packet(shadow, packet, store_pc) !=
            FIELD_CHARACTER_SHADOW_RESULT_OK)
            return 0;
    }
    return 1;
}

static int observe_all_coverage(FieldCharacterShadow *shadow) {
    for (uint32_t kind = 0u; kind < FIELD_CHARACTER_SHADOW_COVERAGE_COUNT; ++kind) {
        if (field_character_shadow_observe_coverage(
                shadow, (FieldCharacterShadowCoverageKind)kind) !=
            FIELD_CHARACTER_SHADOW_RESULT_OK)
            return 0;
    }
    return 1;
}

static int test_complete_metadata_only_selection(void) {
    FieldCharacterShadow shadow;
    FieldCharacterShadowSummary summary = {0};
    FieldCharacterShadowSummary saved = {0};
    FieldCharacterShadowAuth auth = authenticated(UINT32_C(0x440), 1u);
    GpuRenderOraclePacket packet = textured_packet(UINT8_C(0x24), UINT16_C(7));
    GpuRenderOraclePacket packet_before = packet;

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 100u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(observe_family(&shadow, &packet, UINT32_C(0x80076380),
                         FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT));
    CHECK(memcmp(&packet, &packet_before, sizeof(packet)) == 0);
    CHECK(field_character_shadow_observe_access(
              &shadow, UINT32_C(0x80076384), UINT32_C(0x80100000), 4u,
              FIELD_CHARACTER_SHADOW_ACCESS_READ) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_access(
              &shadow, UINT32_C(0x80076384), UINT32_C(0x80100000), 4u,
              FIELD_CHARACTER_SHADOW_ACCESS_READ) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_access(
              &shadow, UINT32_C(0x80076388), UINT32_C(0x80100004), 4u,
              FIELD_CHARACTER_SHADOW_ACCESS_WRITE) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_effect(
              &shadow, FIELD_CHARACTER_SHADOW_EFFECT_ALLOCATOR) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_effect(&shadow, FIELD_CHARACTER_SHADOW_EFFECT_OT) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_effect(&shadow, FIELD_CHARACTER_SHADOW_EFFECT_GPU) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(observe_all_coverage(&shadow));
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_finalize(&shadow, 3700u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.phase == FIELD_CHARACTER_SHADOW_PHASE_COMPLETE);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_NONE);
    CHECK(summary.has_selection == 1u);
    CHECK(summary.selection.site == auth.site);
    CHECK(summary.selection.opcode == packet.opcode);
    CHECK(summary.selection.parser_word_count == packet.parser_word_count);
    CHECK(summary.selection.producer_store_pc == UINT32_C(0x80076380));
    CHECK(summary.selection.count == FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT);
    CHECK(summary.site_count == 1u && summary.family_count == 1u);
    CHECK(summary.access_count == 2u);
    CHECK(summary.accesses[0].instruction_pc == UINT32_C(0x80076384));
    CHECK(summary.accesses[0].address == UINT32_C(0x80100000));
    CHECK(summary.accesses[0].width == 4u);
    CHECK(summary.accesses[0].kind == FIELD_CHARACTER_SHADOW_ACCESS_READ);
    CHECK(summary.accesses[0].count == 2u);
    CHECK(summary.accesses[1].instruction_pc == UINT32_C(0x80076388));
    CHECK(summary.accesses[1].address == UINT32_C(0x80100004));
    CHECK(summary.accesses[1].width == 4u);
    CHECK(summary.accesses[1].kind == FIELD_CHARACTER_SHADOW_ACCESS_WRITE);
    CHECK(summary.accesses[1].count == 1u);
    CHECK(summary.allocator_effect_count == 1u && summary.ot_effect_count == 1u &&
          summary.gpu_effect_count == 1u);
    saved = summary;
    field_character_shadow_init(&shadow);
    CHECK(memcmp(&saved, &summary, sizeof(summary)) == 0);
    return 1;
}

static int test_dynamic_accesses_aggregate_by_authenticated_site(void) {
    FieldCharacterShadow shadow;
    FieldCharacterShadowSummary summary = {0};
    FieldCharacterShadowAuth auth = authenticated(UINT32_C(0x447), 1u);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_dynamic_access(
              &shadow, UINT32_C(0x80076384), UINT32_C(0x80100000), 4u,
              FIELD_CHARACTER_SHADOW_ACCESS_READ) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_dynamic_access(
              &shadow, UINT32_C(0x80076384), UINT32_C(0x80104000), 4u,
              FIELD_CHARACTER_SHADOW_ACCESS_READ) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.access_count == 1u);
    CHECK(summary.accesses[0].instruction_pc == UINT32_C(0x80076384));
    CHECK(summary.accesses[0].address == UINT32_C(0x80100000));
    CHECK(summary.accesses[0].count == 2u);
    return 1;
}

static int test_selection_order_is_deterministic(void) {
    FieldCharacterShadow shadow;
    FieldCharacterShadowSummary summary = {0};
    FieldCharacterShadowAuth auth = authenticated(UINT32_C(0x441), 1u);
    GpuRenderOraclePacket opcode_later = textured_packet(UINT8_C(0x2c), UINT16_C(7));
    GpuRenderOraclePacket length_later = textured_packet(UINT8_C(0x24), UINT16_C(9));
    GpuRenderOraclePacket store_later = textured_packet(UINT8_C(0x24), UINT16_C(7));
    GpuRenderOraclePacket selected = textured_packet(UINT8_C(0x24), UINT16_C(7));

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(observe_family(&shadow, &opcode_later, UINT32_C(0x80076400),
                         FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT));
    CHECK(observe_family(&shadow, &length_later, UINT32_C(0x80076300),
                         FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT));
    CHECK(observe_family(&shadow, &store_later, UINT32_C(0x80076400),
                         FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT));
    CHECK(observe_family(&shadow, &selected, UINT32_C(0x80076300),
                         FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT));
    CHECK(observe_all_coverage(&shadow));
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_finalize(&shadow,
                                          FIELD_CHARACTER_SHADOW_SELECTION_VBLANKS) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.has_selection == 1u);
    CHECK(summary.selection.opcode == UINT8_C(0x24));
    CHECK(summary.selection.parser_word_count == UINT16_C(7));
    CHECK(summary.selection.producer_store_pc == UINT32_C(0x80076300));
    return 1;
}

static int test_eligibility_and_malformed_packet_fail_closed(void) {
    FieldCharacterShadow shadow;
    FieldCharacterShadowSummary summary = {0};
    FieldCharacterShadowAuth auth = authenticated(UINT32_C(0x442), 1u);
    GpuRenderOraclePacket untextured = {0};
    GpuRenderOraclePacket variable = {0};
    GpuRenderOraclePacket malformed = {0};

    untextured.opcode = UINT8_C(0x20);
    untextured.parser_word_count = UINT16_C(4);
    untextured.parser_class = GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_UNTEXTURED;
    variable.opcode = UINT8_C(0x48);
    variable.parser_class = GPU_RENDER_ORACLE_PACKET_CLASS_VARIABLE;
    malformed.opcode = UINT8_C(0xff);
    malformed.parser_class = GPU_RENDER_ORACLE_PACKET_CLASS_MALFORMED;
    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_packet(&shadow, &untextured,
                                                UINT32_C(0x80076300)) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_packet(&shadow, &variable,
                                                UINT32_C(0x80076304)) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.family_count == 0u);
    CHECK(field_character_shadow_observe_packet(&shadow, &malformed,
                                                UINT32_C(0x80076308)) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.phase == FIELD_CHARACTER_SHADOW_PHASE_BLOCKED);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_PACKET);
    CHECK(summary.has_selection == 0u);
    return 1;
}

static int test_authentication_sequences_fail_closed(void) {
    FieldCharacterShadow shadow;
    FieldCharacterShadowSummary summary = {0};
    FieldCharacterShadowAuth auth = authenticated(UINT32_C(0x443), 7u);
    FieldCharacterShadowAuth stale = authenticated(UINT32_C(0x443), 6u);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_UNMATCHED_END);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_begin(&shadow, authenticated(UINT32_C(0x444), 1u), 1u) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_NESTED_BEGIN);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_end(&shadow, stale) == FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_STALE_AUTH);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_begin(&shadow, auth, 1u) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_STALE_AUTH);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_auth_lost(&shadow) == FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_AUTH_LOSS);
    CHECK(summary.has_selection == 0u);
    return 1;
}

static int test_fixed_capacities_and_saturation_fail_closed(void) {
    FieldCharacterShadow shadow;
    FieldCharacterShadowSummary summary = {0};
    FieldCharacterShadowAuth auth = authenticated(UINT32_C(0x445), 1u);
    GpuRenderOraclePacket packet = textured_packet(UINT8_C(0x24), UINT16_C(7));

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    for (uint32_t index = 0u; index < FIELD_CHARACTER_SHADOW_FAMILY_CAPACITY; ++index) {
        CHECK(field_character_shadow_observe_packet(
                  &shadow, &packet, UINT32_C(0x80076000) + index * 4u) ==
              FIELD_CHARACTER_SHADOW_RESULT_OK);
    }
    CHECK(field_character_shadow_observe_packet(&shadow, &packet, UINT32_C(0x80077000)) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_FAMILY_CAPACITY);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    for (uint32_t index = 0u; index < FIELD_CHARACTER_SHADOW_ACCESS_CAPACITY; ++index) {
        CHECK(field_character_shadow_observe_access(
                  &shadow, UINT32_C(0x80076000) + index * 4u,
                  UINT32_C(0x80100000) + index * 4u, 4u,
                  FIELD_CHARACTER_SHADOW_ACCESS_READ) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    }
    CHECK(field_character_shadow_observe_access(&shadow, UINT32_C(0x80077000),
                                                UINT32_C(0x80101000), 4u,
                                                FIELD_CHARACTER_SHADOW_ACCESS_READ) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_ACCESS_CAPACITY);

    field_character_shadow_init(&shadow);
    for (uint32_t index = 0u; index < FIELD_CHARACTER_SHADOW_SITE_CAPACITY; ++index) {
        FieldCharacterShadowAuth site_auth = authenticated(index + 1u, index + 1u);

        CHECK(field_character_shadow_begin(&shadow, site_auth, 0u) ==
              FIELD_CHARACTER_SHADOW_RESULT_OK);
        CHECK(field_character_shadow_end(&shadow, site_auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    }
    CHECK(field_character_shadow_begin(
              &shadow, authenticated(FIELD_CHARACTER_SHADOW_SITE_CAPACITY + 1u,
                                      FIELD_CHARACTER_SHADOW_SITE_CAPACITY + 1u), 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_SITE_CAPACITY);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_packet(&shadow, &packet, UINT32_C(0x80076300)) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_test_seed_family_count(&shadow, 0u, UINT64_MAX) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_packet(&shadow, &packet, UINT32_C(0x80076300)) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_COUNTER_SATURATED);
    return 1;
}

static int test_window_and_completion_requirements_fail_closed(void) {
    FieldCharacterShadow shadow;
    FieldCharacterShadowSummary summary = {0};
    FieldCharacterShadowAuth auth = authenticated(UINT32_C(0x446), 1u);
    GpuRenderOraclePacket packet = textured_packet(UINT8_C(0x24), UINT16_C(7));

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 100u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_finalize(&shadow, 3699u) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_EARLY_WINDOW);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 100u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_finalize(&shadow, 3701u) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_LATE_WINDOW);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_finalize(&shadow,
                                          FIELD_CHARACTER_SHADOW_SELECTION_VBLANKS) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_INSUFFICIENT_COUNT);

    field_character_shadow_init(&shadow);
    CHECK(field_character_shadow_begin(&shadow, auth, 0u) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(observe_family(&shadow, &packet, UINT32_C(0x80076300),
                         FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT));
    CHECK(field_character_shadow_observe_coverage(
              &shadow, FIELD_CHARACTER_SHADOW_COVERAGE_PRIVATE_OVERLAP) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_coverage(&shadow,
                                                  FIELD_CHARACTER_SHADOW_COVERAGE_ACTOR) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_observe_coverage(&shadow,
                                                  FIELD_CHARACTER_SHADOW_COVERAGE_CAMERA) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_end(&shadow, auth) == FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(field_character_shadow_finalize(&shadow,
                                          FIELD_CHARACTER_SHADOW_SELECTION_VBLANKS) ==
          FIELD_CHARACTER_SHADOW_RESULT_BLOCKED);
    CHECK(field_character_shadow_snapshot(&shadow, &summary) ==
          FIELD_CHARACTER_SHADOW_RESULT_OK);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_INSUFFICIENT_COVERAGE);
    CHECK(summary.has_selection == 0u);
    return 1;
}

int main(void) {
    if (!test_complete_metadata_only_selection()) return 1;
    if (!test_dynamic_accesses_aggregate_by_authenticated_site()) return 1;
    if (!test_selection_order_is_deterministic()) return 1;
    if (!test_eligibility_and_malformed_packet_fail_closed()) return 1;
    if (!test_authentication_sequences_fail_closed()) return 1;
    if (!test_fixed_capacities_and_saturation_fail_closed()) return 1;
    if (!test_window_and_completion_requirements_fail_closed()) return 1;
    return 0;
}
