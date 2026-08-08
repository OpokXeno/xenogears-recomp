#ifndef XG_NATIVE_VIEW_H
#define XG_NATIVE_VIEW_H

#include "xg_host_3d.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgNativeView {
    uint16_t aspect_num;
    uint16_t aspect_den;
    uint16_t canonical_width;
    uint16_t canonical_height;
    uint32_t surface_width_16_16;
    int32_t center_offset_x_16_16;
    bool enabled;
} XgNativeView;

bool xg_native_view_configure(XgNativeView *view, bool enabled,
                              uint16_t aspect_num, uint16_t aspect_den,
                              uint16_t canonical_width,
                              uint16_t canonical_height);
bool xg_native_view_projection(const XgNativeView *view,
                               const XgHost3dProjection *canonical,
                               XgHost3dProjection *out_projection);

#ifdef __cplusplus
}
#endif

#endif
