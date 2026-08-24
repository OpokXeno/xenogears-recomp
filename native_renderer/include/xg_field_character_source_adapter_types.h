#ifndef XG_FIELD_CHARACTER_SOURCE_ADAPTER_TYPES_H
#define XG_FIELD_CHARACTER_SOURCE_ADAPTER_TYPES_H

#include "xg_host_3d_types.h"

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

#endif
