#ifndef XG_RENDER_CUTOVER_DISPATCH_H
#define XG_RENDER_CUTOVER_DISPATCH_H

#include "cpu_state.h"
#include "xg_render_route_descriptor.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XgRenderCutoverDispatchResult {
    XG_RENDER_CUTOVER_DISPATCH_CONTINUE = 0,
    XG_RENDER_CUTOVER_DISPATCH_OBSERVED,
    XG_RENDER_CUTOVER_DISPATCH_BYPASS,
} XgRenderCutoverDispatchResult;

typedef struct XgRenderCutoverDispatchContext {
    XgRenderCutoverDispatchResult (*observe_route)(
        CPUState *cpu, const XgRenderCutoverRouteDescriptor *route);
} XgRenderCutoverDispatchContext;

XgRenderCutoverDispatchResult xg_render_cutover_dispatch(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    const XgRenderCutoverDispatchContext *context);
bool xg_render_cutover_dispatch_pc_relevant(uint32_t pc);
bool xg_render_cutover_dispatch_post_pc_relevant(uint32_t pc);
bool xg_render_cutover_dispatch_hook_route_lookup(
    uint32_t hook_type, uint32_t pc, uint32_t instruction_word,
    XgRenderHookRouteDescriptor *out_descriptor);
uint32_t xg_render_cutover_dispatch_exact_route_count(void);
bool xg_render_cutover_dispatch_exact_route_at(
    uint32_t index, uint32_t *out_pc, uint32_t *out_instruction_word);

#endif
