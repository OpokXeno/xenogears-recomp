#ifndef XG_HOST_3D_H
#define XG_HOST_3D_H

#include "xg_host_3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int xg_host_3d_rtps(const XgHost3dProjection *projection,
                    const XgHost3dVector *vertex,
                    XgHost3dProjectedVertex *output,
                    uint32_t *flags);
void xg_host_3d_configure_native_view(int enabled,
                                      int32_t center_offset_x_16_16);
void xg_host_3d_configure_native_view_aspect(
    int enabled, int32_t center_offset_x_16_16,
    uint16_t aspect_num, uint16_t aspect_den);
void xg_host_3d_configure_native_view_margin(uint32_t margin);
int32_t xg_host_3d_native_view_margin(void);
uint32_t xg_host_3d_native_view_depth_limit(uint32_t canonical_limit);
int xg_host_3d_rot_average4(const XgHost3dRotAverage4Input *input,
                            XgHost3dRotAverage4Output *output);
int xg_host_3d_rot_trans_pers4(const XgHost3dProject4Input *input,
                               XgHost3dRotTransPers4Output *output);
int xg_host_3d_op0(const XgHost3dLongVector *left,
                   const XgHost3dLongVector *right,
                   XgHost3dLongVector *mac,
                   uint32_t *flags);
int xg_host_3d_op12(const XgHost3dLongVector *left,
                    const XgHost3dLongVector *right,
                    XgHost3dLongVector *mac,
                    uint32_t *flags);
int xg_host_3d_rtir(const XgHost3dMatrix *matrix,
                    const XgHost3dVector *vector,
                    XgHost3dVector *ir,
                    uint32_t *flags);
int xg_host_3d_rt(const XgHost3dMatrix *matrix,
                  const XgHost3dLongVector *vector,
                  XgHost3dLongVector *mac,
                  uint32_t *flags);
int xg_host_3d_comp_matrix(const XgHost3dMatrix *left,
                           const XgHost3dMatrix *right,
                           XgHost3dMatrix *output);
int xg_host_3d_vector_normal(const XgHost3dLongVector *vector,
                             XgHost3dLongVector *normalized);
int xg_host_3d_scale_matrix(XgHost3dMatrix *matrix,
                            const XgHost3dLongVector *scale);

#ifdef __cplusplus
}
#endif

#endif
