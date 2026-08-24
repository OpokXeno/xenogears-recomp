#include "xg_render_auth_runtime.h"

int xg_render_auth_compat_abi_link_test(void) {
    void (*capture_model_ft3_link)(CPUState *) =
        psx_xg_render_auth_capture_model_ft3_link;
    bool *cold_enabled_state = &g_psx_xg_render_auth_cold_enabled;
    const bool initial = psx_xg_render_auth_cold_enabled();

    if (*cold_enabled_state != initial || capture_model_ft3_link == NULL)
        return 0;
    *cold_enabled_state = !initial;
    if (psx_xg_render_auth_cold_enabled() == initial) return 0;
    capture_model_ft3_link(NULL);
    *cold_enabled_state = initial;
    return psx_xg_render_auth_cold_enabled() == initial;
}
