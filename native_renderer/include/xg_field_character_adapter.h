#ifndef XG_FIELD_CHARACTER_ADAPTER_H
#define XG_FIELD_CHARACTER_ADAPTER_H

#include "xg_field_character_adapter_types.h"
#include "xg_render_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

XgFieldCharacterAdapterResult xg_field_character_adapter_build(
    const XgFieldCharacterCapture *capture,
    XgFieldCharacterCandidate *out_candidate);
XgFieldCharacterAdapterResult xg_field_character_adapter_build_primitive(
    const XgFieldCharacterCandidate *candidate,
    XgRenderIrNativePrimitive *out_primitive);

#ifdef __cplusplus
}
#endif

#endif
