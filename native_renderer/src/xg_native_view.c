#include "xg_native_view.h"

#include <limits.h>
#include <string.h>

bool xg_native_view_configure(XgNativeView *view, bool enabled,
                              uint16_t aspect_num, uint16_t aspect_den,
                              uint16_t canonical_width,
                              uint16_t canonical_height) {
    uint64_t surface_width;
    uint64_t center_offset;
    uint64_t right_margin;

    if (view == NULL) return false;
    memset(view, 0, sizeof(*view));
    xg_host_3d_configure_native_view(false, 0);
    if (!enabled) return true;
    if (aspect_num == 0u || aspect_den == 0u || canonical_width == 0u ||
        canonical_height == 0u ||
        (uint32_t)aspect_num * canonical_height <=
            (uint32_t)aspect_den * canonical_width)
        return false;
    surface_width = ((uint64_t)canonical_height * aspect_num +
                     aspect_den / 2u) / aspect_den;
    /* Native and legacy wide surfaces share one integer raster geometry. Keep
     * the reveal symmetric so changing producers cannot move the scene by a
     * fractional host pixel. */
    if (((surface_width - canonical_width) & 1u) != 0u) --surface_width;
    if (surface_width <= canonical_width ||
        surface_width > (uint64_t)UINT32_MAX >> 16u)
        return false;
    center_offset = (surface_width - canonical_width) / 2u;
    right_margin = surface_width - canonical_width - center_offset;
    if (center_offset == 0u || center_offset > (uint64_t)INT32_MAX >> 16u ||
        right_margin > UINT32_MAX)
        return false;
    view->aspect_num = aspect_num;
    view->aspect_den = aspect_den;
    view->canonical_width = canonical_width;
    view->canonical_height = canonical_height;
    view->surface_width_16_16 = (uint32_t)(surface_width << 16u);
    view->center_offset_x_16_16 = (int32_t)(center_offset << 16u);
    view->enabled = true;
    xg_host_3d_configure_native_view_aspect(
        true, view->center_offset_x_16_16, aspect_num, aspect_den);
    xg_host_3d_configure_native_view_margin(
        (uint32_t)(center_offset > right_margin
            ? center_offset : right_margin));
    return true;
}

bool xg_native_view_projection(const XgNativeView *view,
                               const XgHost3dProjection *canonical,
                               XgHost3dProjection *out_projection) {
    int64_t screen_offset;

    if (view == NULL || canonical == NULL || out_projection == NULL ||
        !view->enabled)
        return false;
    screen_offset = (int64_t)canonical->screen_offset_x +
                    view->center_offset_x_16_16;
    if (screen_offset < INT32_MIN || screen_offset > INT32_MAX) return false;
    *out_projection = *canonical;
    out_projection->screen_offset_x = (int32_t)screen_offset;
    return true;
}
