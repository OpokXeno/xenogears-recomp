#include "xg_visual_state.h"

#include <stdio.h>
#include <stdlib.h>

static void check(int condition, const char *expression) {
    if (condition)
        return;
    fprintf(stderr, "check failed: %s\n", expression);
    exit(EXIT_FAILURE);
}

#define CHECK(expression) check((expression), #expression)

static void test_process_owner_and_null_inputs(void) {
    XgVisualStateBridge *bridge;
    XgVisualStateBridge *same_owner;
    XgVisualStateId first = { 0 };
    XgVisualStateId present = { 0 };

    CHECK(xg_visual_state_process_owner(NULL) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_process_owner(&bridge) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_process_owner(&same_owner) == XG_VISUAL_STATE_OK);
    CHECK(bridge == same_owner);
    CHECK(xg_visual_state_begin_scene(NULL) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_open(NULL, &first) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_open(bridge, NULL) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_finalize(NULL, first) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_present(NULL, &present) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_present(bridge, NULL) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_present(bridge, &present) == XG_VISUAL_STATE_NO_COMPLETED_STATE);
    CHECK(xg_visual_state_begin_scene(bridge) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_open(bridge, NULL) == XG_VISUAL_STATE_INVALID_ARGUMENT);
    CHECK(xg_visual_state_open(bridge, &first) == XG_VISUAL_STATE_OK);
    CHECK(first.scene_epoch == 1 && first.state_sequence == 0);
    CHECK(xg_visual_state_finalize(bridge, first) == XG_VISUAL_STATE_OK);
}

static void test_lifecycle_and_process_wide_uniqueness(void) {
    XgVisualStateBridge *bridge;
    XgVisualStateId first;
    XgVisualStateId second;
    XgVisualStateId next_scene;
    XgVisualStateId present;
    const XgVisualStateId stale = { 1, 99 };

    CHECK(xg_visual_state_process_owner(&bridge) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_open(bridge, &first) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_finalize(bridge, stale) == XG_VISUAL_STATE_INVALID_TRANSITION);
    CHECK(xg_visual_state_finalize(bridge, first) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_open(bridge, &second) == XG_VISUAL_STATE_OK);
    CHECK(!xg_visual_state_id_equal(first, second));
    CHECK(second.scene_epoch == first.scene_epoch);
    CHECK(second.state_sequence == first.state_sequence + 1);
    CHECK(xg_visual_state_present(bridge, &present) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_id_equal(first, present));
    CHECK(xg_visual_state_finalize(bridge, second) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_begin_scene(bridge) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_open(bridge, &next_scene) == XG_VISUAL_STATE_OK);
    CHECK(next_scene.scene_epoch == first.scene_epoch + 1);
    CHECK(next_scene.state_sequence == 0);
    CHECK(!xg_visual_state_id_equal(first, next_scene));
    CHECK(xg_visual_state_finalize(bridge, next_scene) == XG_VISUAL_STATE_OK);
}

static void test_independent_mode_axes(void) {
    XgVisualStateModes modes = {
        XG_TIMING_MODE_ORIGINAL,
        XG_RENDER_MODE_ORIGINAL,
    };

    CHECK(xg_visual_state_modes_are_valid(modes));
    modes.render_mode = XG_RENDER_MODE_NATIVE;
    CHECK(modes.timing_mode == XG_TIMING_MODE_ORIGINAL);
    CHECK(xg_visual_state_modes_are_valid(modes));
    modes.timing_mode = XG_TIMING_MODE_NATIVE_59_94;
    CHECK(modes.render_mode == XG_RENDER_MODE_NATIVE);
    CHECK(xg_visual_state_modes_are_valid(modes));
}

static void test_overflow_fails_closed(void) {
    XgVisualStateBridge *bridge;
    XgVisualStateId id;
    XgVisualStateId last;

    CHECK(xg_visual_state_process_owner(&bridge) == XG_VISUAL_STATE_OK);
    while (xg_visual_state_open(bridge, &id) == XG_VISUAL_STATE_OK)
        CHECK(xg_visual_state_finalize(bridge, id) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_present(bridge, &last) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_open(bridge, &id) == XG_VISUAL_STATE_OVERFLOW);
    CHECK(xg_visual_state_present(bridge, &id) == XG_VISUAL_STATE_OK);
    CHECK(xg_visual_state_id_equal(id, last));
}

int main(void) {
    test_process_owner_and_null_inputs();
    test_lifecycle_and_process_wide_uniqueness();
    test_independent_mode_axes();
    test_overflow_fails_closed();
    puts("visual-state lifecycle assertions passed");
    return 0;
}
