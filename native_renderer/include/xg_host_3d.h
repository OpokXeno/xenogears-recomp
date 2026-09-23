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
/* Presentation-only projection from authenticated LOCAL geometry. Also used by
 * RTPS Native metadata; retains fractional MAC/SZ and H/Z until Q16 conversion. */
int xg_host_3d_native_project(const XgHost3dProjection *projection,
                             const XgHost3dVector *vertex,
                             int32_t *out_x, int32_t *out_y,
                             int32_t *out_depth_q12);
/* Unfloored view Z as the Native depth payload, in Q12. Clamped like the
 * presentation projection saturates (near ~H/2, far 0xffff); zero behind the
 * camera. */
int32_t xg_host_3d_native_depth_q12(double view_z, uint32_t projection_distance);
/* Continuous matrix * point: translation + rotation * point / 4096, with no
 * MAC flooring. The point may carry a sub-unit source position. */
void xg_host_3d_native_transform_point(const XgHost3dMatrix *matrix,
                                       const double point[3],
                                       double out[3]);
/* Continuous left * right rotation in 4.12 units, with no IR flooring. */
void xg_host_3d_native_compose_rotation(const XgHost3dMatrix *left,
                                        const XgHost3dMatrix *right,
                                        double out[3][3]);
/* Installs the presentation transform read by xg_host_3d_native_project.
 * A NULL rotation keeps the canonical integer rotation of the projection. */
void xg_host_3d_set_native_transform(XgHost3dProjection *projection,
                                     const double rotation[3][3],
                                     const double translation[3]);
/* Source culling follows Native Q16 geometry when present; otherwise it uses
 * the canonical signed screen-area result. Reads the first three vertices. */
int32_t xg_host_3d_nclip(const XgHost3dProjectedVertex *vertices);
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
