#include "field_character_shadow_test_adapter.h"

FieldCharacterShadowResult field_character_shadow_test_seed_family_count(
        FieldCharacterShadow *shadow, size_t index, uint64_t count) {
    if (shadow == NULL || index >= shadow->family_count)
        return FIELD_CHARACTER_SHADOW_RESULT_INVALID_ARGUMENT;
    shadow->families[index].count = count;
    return FIELD_CHARACTER_SHADOW_RESULT_OK;
}
