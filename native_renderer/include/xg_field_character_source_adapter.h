#ifndef XG_FIELD_CHARACTER_SOURCE_ADAPTER_H
#define XG_FIELD_CHARACTER_SOURCE_ADAPTER_H

#include "xg_field_character_adapter.h"
#include "xg_field_character_source.h"
#include "xg_host_3d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgFieldCharacterSourceAdapterResult {
    XG_FIELD_CHARACTER_SOURCE_ADAPTER_OK = 0,
    XG_FIELD_CHARACTER_SOURCE_ADAPTER_INVALID_ARGUMENT = 1,
    XG_FIELD_CHARACTER_SOURCE_ADAPTER_INVALID_SNAPSHOT = 2,
    XG_FIELD_CHARACTER_SOURCE_ADAPTER_CULLED = 3,
    XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_TRANSFORM = 4,
    XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_MATERIAL = 5,
    XG_FIELD_CHARACTER_SOURCE_ADAPTER_INCOMPLETE_ORDERING = 6,
} XgFieldCharacterSourceAdapterResult;

typedef struct XgFieldCharacterSourceDerived {
    XgHost3dMatrix object_to_view;
    XgHost3dRotAverage4Output projection;
    uint32_t ordering_bucket;
} XgFieldCharacterSourceDerived;

XgFieldCharacterSourceAdapterResult xg_field_character_source_adapter_build(
    const XgFieldCharacterSourceSnapshot *snapshot,
    XgFieldCharacterCandidate *out_candidate,
    XgFieldCharacterSourceDerived *out_derived);

#ifdef __cplusplus
}
#endif

#endif
