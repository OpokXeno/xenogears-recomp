#ifndef XG_FIELD_CHARACTER_SOURCE_H
#define XG_FIELD_CHARACTER_SOURCE_H

#include "xg_field_character_source_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void xg_field_character_source_digest(
    const XgFieldCharacterSourceSnapshot *snapshot,
    uint8_t out_digest[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE]);
bool xg_field_character_source_digest_matches(
    const XgFieldCharacterSourceSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
