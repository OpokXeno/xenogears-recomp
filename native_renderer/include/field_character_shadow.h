#ifndef XG_FIELD_CHARACTER_SHADOW_H
#define XG_FIELD_CHARACTER_SHADOW_H

#include "gpu_render_oracle.h"
#include "field_character_shadow_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif
