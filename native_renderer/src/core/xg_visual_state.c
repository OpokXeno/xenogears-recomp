#include "xg_visual_state.h"

#include <stddef.h>

struct XgVisualStateBridge {
    uint64_t scene_epoch;
    uint64_t next_state_sequence;
    XgVisualStateId active_id;
    XgVisualStateId last_completed_id;
    XgVisualStatePhase phase;
    bool has_scene;
    bool has_last_completed;
};

static XgVisualStateBridge process_owner;

bool xg_visual_state_id_equal(XgVisualStateId left, XgVisualStateId right) {
    return left.scene_epoch == right.scene_epoch &&
           left.state_sequence == right.state_sequence;
}

bool xg_visual_state_modes_are_valid(XgVisualStateModes modes) {
    const bool timing_valid = modes.timing_mode == XG_TIMING_MODE_ORIGINAL ||
                              modes.timing_mode == XG_TIMING_MODE_NATIVE_59_94;
    const bool render_valid = modes.render_mode == XG_RENDER_MODE_ORIGINAL ||
                              modes.render_mode == XG_RENDER_MODE_SHADOW ||
                              modes.render_mode == XG_RENDER_MODE_NATIVE;
    return timing_valid && render_valid;
}

XgVisualStateResult xg_visual_state_process_owner(XgVisualStateBridge **out_owner) {
    if (out_owner == NULL)
        return XG_VISUAL_STATE_INVALID_ARGUMENT;
    *out_owner = &process_owner;
    return XG_VISUAL_STATE_OK;
}

XgVisualStateResult xg_visual_state_begin_scene(XgVisualStateBridge *bridge) {
    if (bridge == NULL)
        return XG_VISUAL_STATE_INVALID_ARGUMENT;
    if (bridge->phase == XG_VISUAL_STATE_OPEN)
        return XG_VISUAL_STATE_INVALID_TRANSITION;
    if (bridge->scene_epoch >= XG_VISUAL_STATE_COUNTER_MAX)
        return XG_VISUAL_STATE_OVERFLOW;

    bridge->scene_epoch++;
    bridge->next_state_sequence = 0;
    bridge->phase = XG_VISUAL_STATE_IDLE;
    bridge->has_scene = true;
    bridge->has_last_completed = false;
    return XG_VISUAL_STATE_OK;
}

XgVisualStateResult xg_visual_state_open(XgVisualStateBridge *bridge,
                                          XgVisualStateId *out_id) {
    if (bridge == NULL || out_id == NULL)
        return XG_VISUAL_STATE_INVALID_ARGUMENT;
    if (!bridge->has_scene || bridge->phase == XG_VISUAL_STATE_OPEN)
        return XG_VISUAL_STATE_INVALID_TRANSITION;
    if (bridge->next_state_sequence >= XG_VISUAL_STATE_COUNTER_MAX)
        return XG_VISUAL_STATE_OVERFLOW;

    bridge->active_id = (XgVisualStateId){
        bridge->scene_epoch,
        bridge->next_state_sequence,
    };
    bridge->next_state_sequence++;
    bridge->phase = XG_VISUAL_STATE_OPEN;
    *out_id = bridge->active_id;
    return XG_VISUAL_STATE_OK;
}

XgVisualStateResult xg_visual_state_finalize(XgVisualStateBridge *bridge,
                                              XgVisualStateId id) {
    if (bridge == NULL)
        return XG_VISUAL_STATE_INVALID_ARGUMENT;
    if (bridge->phase != XG_VISUAL_STATE_OPEN ||
        !xg_visual_state_id_equal(bridge->active_id, id))
        return XG_VISUAL_STATE_INVALID_TRANSITION;

    bridge->last_completed_id = bridge->active_id;
    bridge->has_last_completed = true;
    bridge->phase = XG_VISUAL_STATE_FINALIZED;
    return XG_VISUAL_STATE_OK;
}

XgVisualStateResult xg_visual_state_present(const XgVisualStateBridge *bridge,
                                             XgVisualStateId *out_id) {
    if (bridge == NULL || out_id == NULL)
        return XG_VISUAL_STATE_INVALID_ARGUMENT;
    if (!bridge->has_last_completed)
        return XG_VISUAL_STATE_NO_COMPLETED_STATE;

    *out_id = bridge->last_completed_id;
    return XG_VISUAL_STATE_OK;
}
