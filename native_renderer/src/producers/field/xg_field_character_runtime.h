#ifndef XG_FIELD_CHARACTER_RUNTIME_H
#define XG_FIELD_CHARACTER_RUNTIME_H

#include "xg_field_character_runtime_types.h"
#include "xg_field_character_source_types.h"
#include "xg_render_source_types.h"

#include <stdint.h>

typedef struct CPUState CPUState;
typedef struct XgRenderIrNativePrimitive XgRenderIrNativePrimitive;

XgFieldCharacterRuntimeResult xg_field_character_runtime_build_candidate(
    const XgFieldCharacterSourceSnapshot *snapshot,
    XgFieldCharacterRuntimeCandidate *out_candidate);
XgFieldCharacterRuntimeResult xg_field_character_runtime_build_shadow_candidate(
    CPUState *cpu, const PsxXgRenderFt4Geometry *geometry,
    XgFieldCharacterRuntimeCandidate *out_candidate);
XgFieldCharacterRuntimeResult xg_field_character_runtime_compare_original(
    CPUState *cpu, const XgFieldCharacterRuntimeCandidate *runtime_candidate,
    uint32_t original_ot_bucket, uint32_t authenticated_ot_bucket,
    XgRenderIrNativePrimitive *out_primitive, uint32_t *out_compare_result,
    uint32_t *out_mismatch_word, uint32_t *out_mismatch_byte);

#endif
