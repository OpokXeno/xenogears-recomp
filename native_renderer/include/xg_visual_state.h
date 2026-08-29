#ifndef XG_VISUAL_STATE_H
#define XG_VISUAL_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef XG_VISUAL_STATE_COUNTER_MAX
#define XG_VISUAL_STATE_COUNTER_MAX UINT64_MAX
#endif

typedef struct XgVisualStateId {
    uint64_t scene_epoch;
    uint64_t state_sequence;
} XgVisualStateId;

typedef enum XgVisualStatePhase {
    XG_VISUAL_STATE_IDLE,
    XG_VISUAL_STATE_OPEN,
    XG_VISUAL_STATE_FINALIZED,
} XgVisualStatePhase;

typedef enum XgVisualStateResult {
    XG_VISUAL_STATE_OK,
    XG_VISUAL_STATE_INVALID_ARGUMENT,
    XG_VISUAL_STATE_INVALID_TRANSITION,
    XG_VISUAL_STATE_OVERFLOW,
    XG_VISUAL_STATE_NO_COMPLETED_STATE,
} XgVisualStateResult;

typedef enum XgRenderMode {
    XG_RENDER_MODE_ORIGINAL,
    XG_RENDER_MODE_SHADOW,
    XG_RENDER_MODE_NATIVE,
} XgRenderMode;

typedef struct XgVisualStateModes {
    XgRenderMode render_mode;
} XgVisualStateModes;

typedef struct XgVisualStateBridge XgVisualStateBridge;

bool xg_visual_state_id_equal(XgVisualStateId left, XgVisualStateId right);
bool xg_visual_state_modes_are_valid(XgVisualStateModes modes);
XgVisualStateResult xg_visual_state_process_owner(XgVisualStateBridge **out_owner);
XgVisualStateResult xg_visual_state_begin_scene(XgVisualStateBridge *bridge);
XgVisualStateResult xg_visual_state_open(XgVisualStateBridge *bridge,
                                          XgVisualStateId *out_id);
XgVisualStateResult xg_visual_state_finalize(XgVisualStateBridge *bridge,
                                              XgVisualStateId id);
XgVisualStateResult xg_visual_state_present(const XgVisualStateBridge *bridge,
                                             XgVisualStateId *out_id);

#endif
