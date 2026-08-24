#ifndef XG_FIELD_CHARACTER_SOURCE_CAPTURE_H
#define XG_FIELD_CHARACTER_SOURCE_CAPTURE_H

#include "xg_field_character_source_capture_types.h"

XgFieldCharacterSourceCaptureResult xg_field_character_source_capture(
    const XgFieldCharacterSourceCaptureRequest *request,
    const XgFieldCharacterAuthenticatedReader *reader,
    XgFieldCharacterSourceSnapshot *out_snapshot);

#endif
