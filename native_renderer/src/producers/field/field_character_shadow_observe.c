#include "field_character_shadow_internal.h"

static FieldCharacterShadowFamily *family_for(
    FieldCharacterShadow *shadow, uint32_t site,
    const GpuRenderOraclePacket *packet, uint32_t producer_store_pc) {
    for (size_t index = 0u; index < shadow->family_count; ++index) {
        FieldCharacterShadowFamily *family = &shadow->families[index];

        if (family->site == site && family->opcode == packet->opcode &&
            family->parser_word_count == packet->parser_word_count &&
            family->producer_store_pc == producer_store_pc)
            return family;
    }
    return NULL;
}

static int packet_is_malformed(const GpuRenderOraclePacket *packet,
                               uint32_t producer_store_pc) {
    if (packet->parser_class == GPU_RENDER_ORACLE_PACKET_CLASS_MALFORMED)
        return 1;
    if (packet->task11_family_eligible > 1u) return 1;
    if (packet->task11_family_eligible == 0u) return 0;
    return packet->parser_class != GPU_RENDER_ORACLE_PACKET_CLASS_FIXED_TEXTURED ||
           packet->parser_word_count == 0u || (producer_store_pc & 3u) != 0u;
}

static int access_is_valid(uint8_t width, FieldCharacterShadowAccessKind kind) {
    return (width == 1u || width == 2u || width == 4u) &&
           (kind == FIELD_CHARACTER_SHADOW_ACCESS_READ ||
            kind == FIELD_CHARACTER_SHADOW_ACCESS_WRITE);
}

FieldCharacterShadowResult field_character_shadow_observe_packet(
    FieldCharacterShadow *shadow, const GpuRenderOraclePacket *packet,
    uint32_t producer_store_pc) {
    FieldCharacterShadowFamily *family;
    FieldCharacterShadowResult result;

    if (shadow == NULL || packet == NULL)
        return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    result = field_character_shadow_require_open_internal(shadow);
    if (result != FIELD_CHARACTER_SHADOW_RESULT_OK) return result;
    if (packet_is_malformed(packet, producer_store_pc))
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_PACKET);
    if (!packet->task11_family_eligible) return FIELD_CHARACTER_SHADOW_RESULT_OK;
    family = family_for(shadow, shadow->active_auth.site, packet, producer_store_pc);
    if (family == NULL) {
        if (shadow->family_count == FIELD_CHARACTER_SHADOW_FAMILY_CAPACITY)
            return field_character_shadow_block_internal(
                shadow, FIELD_CHARACTER_SHADOW_BLOCKER_FAMILY_CAPACITY);
        family = &shadow->families[shadow->family_count++];
        family->site = shadow->active_auth.site;
        family->producer_store_pc = producer_store_pc;
        family->parser_word_count = packet->parser_word_count;
        family->opcode = packet->opcode;
        family->in_use = 1u;
    }
    if (!field_character_shadow_increment_internal(&family->count))
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_COUNTER_SATURATED);
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

static FieldCharacterShadowResult observe_access(
    FieldCharacterShadow *shadow, uint32_t instruction_pc, uint32_t address,
    uint8_t width, FieldCharacterShadowAccessKind kind,
    int aggregate_dynamic_address) {
    FieldCharacterShadowResult result;

    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    result = field_character_shadow_require_open_internal(shadow);
    if (result != FIELD_CHARACTER_SHADOW_RESULT_OK) return result;
    if ((instruction_pc & 3u) != 0u || !access_is_valid(width, kind))
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_ACCESS);
    for (size_t index = 0u; index < shadow->access_count; ++index) {
        FieldCharacterShadowAccess *access = &shadow->accesses[index];

        if (access->instruction_pc == instruction_pc &&
            (aggregate_dynamic_address || access->address == address) &&
            access->width == width && access->kind == kind) {
            if (!field_character_shadow_increment_internal(&access->count))
                return field_character_shadow_block_internal(
                    shadow, FIELD_CHARACTER_SHADOW_BLOCKER_COUNTER_SATURATED);
            return FIELD_CHARACTER_SHADOW_RESULT_OK;
        }
    }
    if (shadow->access_count == FIELD_CHARACTER_SHADOW_ACCESS_CAPACITY)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_ACCESS_CAPACITY);
    shadow->accesses[shadow->access_count] = (FieldCharacterShadowAccess){
        instruction_pc, address, width, kind, 1u,
    };
    shadow->access_count++;
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

FieldCharacterShadowResult field_character_shadow_observe_access(
    FieldCharacterShadow *shadow, uint32_t instruction_pc, uint32_t address,
    uint8_t width, FieldCharacterShadowAccessKind kind) {
    return observe_access(shadow, instruction_pc, address, width, kind, 0);
}

FieldCharacterShadowResult field_character_shadow_observe_dynamic_access(
    FieldCharacterShadow *shadow, uint32_t instruction_pc, uint32_t address,
    uint8_t width, FieldCharacterShadowAccessKind kind) {
    return observe_access(shadow, instruction_pc, address, width, kind, 1);
}

FieldCharacterShadowResult field_character_shadow_observe_effect(
    FieldCharacterShadow *shadow, FieldCharacterShadowEffectKind kind) {
    uint64_t *counter;
    FieldCharacterShadowResult result;

    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    result = field_character_shadow_require_open_internal(shadow);
    if (result != FIELD_CHARACTER_SHADOW_RESULT_OK) return result;
    switch (kind) {
    case FIELD_CHARACTER_SHADOW_EFFECT_ALLOCATOR:
        counter = &shadow->allocator_effect_count;
        break;
    case FIELD_CHARACTER_SHADOW_EFFECT_OT:
        counter = &shadow->ot_effect_count;
        break;
    case FIELD_CHARACTER_SHADOW_EFFECT_GPU:
        counter = &shadow->gpu_effect_count;
        break;
    default:
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_EFFECT);
    }
    if (!field_character_shadow_increment_internal(counter))
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_COUNTER_SATURATED);
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}

FieldCharacterShadowResult field_character_shadow_observe_coverage(
    FieldCharacterShadow *shadow, FieldCharacterShadowCoverageKind kind) {
    FieldCharacterShadowResult result;

    if (shadow == NULL) return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    result = field_character_shadow_require_open_internal(shadow);
    if (result != FIELD_CHARACTER_SHADOW_RESULT_OK) return result;
    if (kind < FIELD_CHARACTER_SHADOW_COVERAGE_PRIVATE_OVERLAP ||
        kind > FIELD_CHARACTER_SHADOW_COVERAGE_DIALOG)
        return field_character_shadow_block_internal(
            shadow, FIELD_CHARACTER_SHADOW_BLOCKER_MALFORMED_COVERAGE);
    shadow->coverage_mask |= (uint8_t)(UINT8_C(1) << kind);
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}
