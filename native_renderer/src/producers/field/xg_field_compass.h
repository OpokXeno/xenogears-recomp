#ifndef XG_FIELD_COMPASS_H
#define XG_FIELD_COMPASS_H

#include "xg_field_character_capture.h"
#include "xg_render_model_repository.h"
#include "xg_render_submission.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;

typedef struct XgFieldCompassPipelineServices {
    bool (*pre_scene_available)(uint32_t primitive_count);
    bool (*begin_lifecycle)(
        uint32_t producer_pc, XgRenderProducerLifecycle *out_lifecycle);
    bool (*stage)(const XgRenderPreScenePrimitive *record);
    bool (*publish)(
        const XgRenderModelFt4SourceRecord *record,
        const XgRenderModelSourcePublication *publication);
} XgFieldCompassPipelineServices;

void xg_field_compass_fail(uint32_t blocker);
bool xg_field_compass_pre_scene_available(uint32_t primitive_count);

bool xg_field_compass_capture_material(
    CPUState *cpu, uint32_t source_address,
    XgFieldCharacterCapture *capture);
bool xg_field_compass_cutover(
    CPUState *cpu, uint32_t producer_pc, bool screen_aligned,
    const XgFieldCompassPipelineServices *services);

#endif
