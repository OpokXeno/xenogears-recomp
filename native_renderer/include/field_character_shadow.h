#ifndef XG_FIELD_CHARACTER_SHADOW_H
#define XG_FIELD_CHARACTER_SHADOW_H

#include "gpu_render_oracle.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIELD_CHARACTER_SHADOW_SUMMARY_METADATA_ONLY 1

enum {
    FIELD_CHARACTER_SHADOW_FAMILY_CAPACITY = 64u,
    FIELD_CHARACTER_SHADOW_ACCESS_CAPACITY = 256u,
    FIELD_CHARACTER_SHADOW_SITE_CAPACITY = 16u,
    FIELD_CHARACTER_SHADOW_COVERAGE_COUNT = 4u,
    FIELD_CHARACTER_SHADOW_SELECTION_VBLANKS = 3600u,
    FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT = 1000u,
};

typedef enum FieldCharacterShadowResult {
    FIELD_CHARACTER_SHADOW_RESULT_OK = 0,
    FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT,
    FIELD_CHARACTER_SHADOW_RESULT_INVALID_STATE,
    FIELD_CHARACTER_SHADOW_RESULT_BLOCKED,
} FieldCharacterShadowResult;

typedef enum FieldCharacterShadowPhase {
    FIELD_CHARACTER_SHADOW_PHASE_IDLE = 0,
    FIELD_CHARACTER_SHADOW_PHASE_COLLECTING,
    FIELD_CHARACTER_SHADOW_PHASE_COMPLETE,
    FIELD_CHARACTER_SHADOW_PHASE_BLOCKED,
} FieldCharacterShadowPhase;

typedef enum FieldCharacterShadowBlocker {
    FIELD_CHARACTER_SHADOW_BLOCKER_NONE = 0,
    FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_AUTH,
    FIELD_CHARACTER_SHADOW_BLOCKER_NESTED_BEGIN,
    FIELD_CHARACTER_SHADOW_BLOCKER_UNMATCHED_END,
    FIELD_CHARACTER_SHADOW_BLOCKER_STALE_AUTH,
    FIELD_CHARACTER_SHADOW_BLOCKER_AUTH_LOSS,
    FIELD_CHARACTER_SHADOW_BLOCKER_UNAUTHENTICATED_OBSERVATION,
    FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_PACKET,
    FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_ACCESS,
    FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_EFFECT,
    FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_COVERAGE,
    FIELD_CHARACTER_SHADOW_BLOCKER_FAMILY_CAPACITY,
    FIELD_CHARACTER_SHADOW_BLOCKER_ACCESS_CAPACITY,
    FIELD_CHARACTER_SHADOW_BLOCKER_SITE_CAPACITY,
    FIELD_CHARACTER_SHADOW_BLOCKER_COUNTER_SATURATED,
    FIELD_CHARACTER_SHADOW_BLOCKER_EARLY_WINDOW,
    FIELD_CHARACTER_SHADOW_BLOCKER_LATE_WINDOW,
    FIELD_CHARACTER_SHADOW_BLOCKER_INSUFFICIENT_COUNT,
    FIELD_CHARACTER_SHADOW_BLOCKER_INSUFFICIENT_COVERAGE,
} FieldCharacterShadowBlocker;

typedef enum FieldCharacterShadowAccessKind {
    FIELD_CHARACTER_SHADOW_ACCESS_READ = 0,
    FIELD_CHARACTER_SHADOW_ACCESS_WRITE,
} FieldCharacterShadowAccessKind;

typedef enum FieldCharacterShadowEffectKind {
    FIELD_CHARACTER_SHADOW_EFFECT_ALLOCATOR = 0,
    FIELD_CHARACTER_SHADOW_EFFECT_OT,
    FIELD_CHARACTER_SHADOW_EFFECT_GPU,
} FieldCharacterShadowEffectKind;

typedef enum FieldCharacterShadowCoverageKind {
    FIELD_CHARACTER_SHADOW_COVERAGE_PRIVATE_OVERLAP = 0,
    FIELD_CHARACTER_SHADOW_COVERAGE_ACTOR,
    FIELD_CHARACTER_SHADOW_COVERAGE_CAMERA,
    FIELD_CHARACTER_SHADOW_COVERAGE_DIALOG,
} FieldCharacterShadowCoverageKind;

typedef struct FieldCharacterShadowAuth {
    uint32_t site;
    uint64_t sequence;
    uint8_t authenticated;
} FieldCharacterShadowAuth;

typedef struct FieldCharacterShadowFamily {
    uint32_t site;
    uint32_t producer_store_pc;
    uint16_t parser_word_count;
    uint8_t opcode;
    uint8_t in_use;
    uint64_t count;
} FieldCharacterShadowFamily;

typedef struct FieldCharacterShadowAccess {
    uint32_t instruction_pc;
    uint32_t address;
    uint8_t width;
    FieldCharacterShadowAccessKind kind;
    uint64_t count;
} FieldCharacterShadowAccess;

typedef struct FieldCharacterShadowSite {
    uint32_t site;
    uint64_t last_sequence;
} FieldCharacterShadowSite;

typedef struct FieldCharacterShadowSelection {
    uint32_t site;
    uint32_t producer_store_pc;
    uint16_t parser_word_count;
    uint8_t opcode;
    uint64_t count;
} FieldCharacterShadowSelection;

typedef struct FieldCharacterShadowSummary {
    FieldCharacterShadowPhase phase;
    FieldCharacterShadowBlocker blocker;
    uint64_t start_guest_vblank;
    uint64_t end_guest_vblank;
    uint64_t allocator_effect_count;
    uint64_t ot_effect_count;
    uint64_t gpu_effect_count;
    size_t family_count;
    size_t access_count;
    size_t site_count;
    uint8_t coverage_mask;
    uint8_t has_selection;
    FieldCharacterShadowSelection selection;
    FieldCharacterShadowAccess accesses[FIELD_CHARACTER_SHADOW_ACCESS_CAPACITY];
} FieldCharacterShadowSummary;

typedef struct FieldCharacterShadow {
    FieldCharacterShadowFamily families[FIELD_CHARACTER_SHADOW_FAMILY_CAPACITY];
    FieldCharacterShadowAccess accesses[FIELD_CHARACTER_SHADOW_ACCESS_CAPACITY];
    FieldCharacterShadowSite sites[FIELD_CHARACTER_SHADOW_SITE_CAPACITY];
    FieldCharacterShadowAuth active_auth;
    uint64_t start_guest_vblank;
    uint64_t end_guest_vblank;
    uint64_t allocator_effect_count;
    uint64_t ot_effect_count;
    uint64_t gpu_effect_count;
    size_t family_count;
    size_t access_count;
    size_t site_count;
    uint8_t coverage_mask;
    uint8_t open;
    uint8_t has_selection;
    FieldCharacterShadowPhase phase;
    FieldCharacterShadowBlocker blocker;
    FieldCharacterShadowSelection selection;
} FieldCharacterShadow;

void field_character_shadow_init(FieldCharacterShadow *shadow);
FieldCharacterShadowResult field_character_shadow_begin(
    FieldCharacterShadow *shadow, FieldCharacterShadowAuth auth,
    uint64_t guest_vblank);
FieldCharacterShadowResult field_character_shadow_end(
    FieldCharacterShadow *shadow, FieldCharacterShadowAuth auth);
FieldCharacterShadowResult field_character_shadow_auth_lost(
    FieldCharacterShadow *shadow);
FieldCharacterShadowResult field_character_shadow_observe_packet(
    FieldCharacterShadow *shadow, const GpuRenderOraclePacket *packet,
    uint32_t producer_store_pc);
FieldCharacterShadowResult field_character_shadow_observe_access(
    FieldCharacterShadow *shadow, uint32_t instruction_pc, uint32_t address,
    uint8_t width, FieldCharacterShadowAccessKind kind);
FieldCharacterShadowResult field_character_shadow_observe_dynamic_access(
    FieldCharacterShadow *shadow, uint32_t instruction_pc, uint32_t address,
    uint8_t width, FieldCharacterShadowAccessKind kind);
FieldCharacterShadowResult field_character_shadow_observe_effect(
    FieldCharacterShadow *shadow, FieldCharacterShadowEffectKind kind);
FieldCharacterShadowResult field_character_shadow_observe_coverage(
    FieldCharacterShadow *shadow, FieldCharacterShadowCoverageKind kind);
FieldCharacterShadowResult field_character_shadow_finalize(
    FieldCharacterShadow *shadow, uint64_t guest_vblank);
FieldCharacterShadowResult field_character_shadow_snapshot(
    const FieldCharacterShadow *shadow, FieldCharacterShadowSummary *out_summary);

#ifdef FIELD_CHARACTER_SHADOW_TESTING
FieldCharacterShadowResult field_character_shadow_test_seed_family_count(
    FieldCharacterShadow *shadow, size_t index, uint64_t count);
#endif

#ifdef __cplusplus
}
#endif

#endif
