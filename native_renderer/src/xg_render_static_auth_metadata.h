#ifndef XG_RENDER_STATIC_AUTH_METADATA_H
#define XG_RENDER_STATIC_AUTH_METADATA_H

#include "xg_render_auth.h"

#include <stdbool.h>

bool xg_render_static_auth_metadata_is_valid(void);
bool xg_render_static_auth_bind_identity(bool *out_bound, bool *out_gate_passed);
XgRenderAuthProfile xg_render_static_auth_profile_from_metadata(void);
XgRenderAuthExecution xg_render_static_auth_execution_for(XgRenderAuthHook hook);

#endif
