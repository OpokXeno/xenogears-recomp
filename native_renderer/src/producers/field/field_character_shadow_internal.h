#ifndef XG_FIELD_CHARACTER_SHADOW_INTERNAL_H
#define XG_FIELD_CHARACTER_SHADOW_INTERNAL_H

#include "field_character_shadow.h"

FieldCharacterShadowResult field_character_shadow_block_internal(
    FieldCharacterShadow *shadow, FieldCharacterShadowBlocker blocker);
FieldCharacterShadowResult field_character_shadow_require_open_internal(
    FieldCharacterShadow *shadow);
int field_character_shadow_increment_internal(uint64_t *counter);

#endif
