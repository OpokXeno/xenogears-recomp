#ifndef XG_RENDER_BATTLE_GEOMETRY_H
#define XG_RENDER_BATTLE_GEOMETRY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUState CPUState;
typedef struct XgRenderProducerLifecycleServices XgRenderProducerLifecycleServices;

/* Source authority only: does not require a motion pose or certify a scene. */
bool xg_render_battle_geometry_authorizes_call(uint32_t call_pc);
bool xg_render_battle_geometry_capture(
    const CPUState *cpu, const XgRenderProducerLifecycleServices *lifecycle);

#endif
