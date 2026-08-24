#ifndef XG_FIELD_CHARACTER_SOURCE_ADAPTER_H
#define XG_FIELD_CHARACTER_SOURCE_ADAPTER_H

#include "xg_field_character_adapter_types.h"
#include "xg_field_character_source_adapter_types.h"
#include "xg_field_character_source_types.h"

#ifdef __cplusplus
extern "C" {
#endif

XgFieldCharacterSourceAdapterResult xg_field_character_source_adapter_build(
    const XgFieldCharacterSourceSnapshot *snapshot,
    XgFieldCharacterCandidate *out_candidate,
    XgFieldCharacterSourceDerived *out_derived);

#ifdef __cplusplus
}
#endif

#endif
