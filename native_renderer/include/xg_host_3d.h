#ifndef XG_HOST_3D_H
#define XG_HOST_3D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_HOST_3D_VERTEX_COUNT = 4,
};

typedef struct XgHost3dVector {
    int16_t x;
    int16_t y;
    int16_t z;
    uint16_t pad;
} XgHost3dVector;

typedef struct XgHost3dLongVector {
    int32_t x;
    int32_t y;
    int32_t z;
} XgHost3dLongVector;

typedef struct XgHost3dMatrix {
    int16_t rotation[3][3];
    uint16_t pad;
    int32_t translation[3];
} XgHost3dMatrix;

typedef struct XgHost3dProjection {
    int16_t rotation[3][3];
    int32_t translation[3];
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    uint16_t projection_distance;
    int16_t depth_cue_a;
    int32_t depth_cue_b;
    int16_t average_z_scale4;
} XgHost3dProjection;

typedef struct XgHost3dProject4Input {
    XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT];
    XgHost3dProjection projection;
} XgHost3dProject4Input;

typedef XgHost3dProject4Input XgHost3dRotAverage4Input;

typedef struct XgHost3dProjectedVertex {
    int16_t x;
    int16_t y;
    uint16_t z;
    int32_t x_16_16;
    int32_t y_16_16;
    int32_t native_view_x_16_16;
    int32_t native_view_y_16_16;
    uint8_t native_view_position;
} XgHost3dProjectedVertex;

typedef struct XgHost3dRotAverage4Output {
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    int16_t depth_cue;
    uint16_t ordering_depth;
    uint32_t rtpt_flags;
    uint32_t rtps_flags;
    uint32_t projection_flags;
    uint32_t avsz4_flags;
} XgHost3dRotAverage4Output;

typedef struct XgHost3dRotTransPers4Output {
    XgHost3dProjectedVertex vertices[XG_HOST_3D_VERTEX_COUNT];
    int16_t depth_cue;
    uint16_t fourth_depth;
    uint32_t rtpt_flags;
    uint32_t rtps_flags;
    uint32_t projection_flags;
} XgHost3dRotTransPers4Output;

int xg_host_3d_rtps(const XgHost3dProjection *projection,
                    const XgHost3dVector *vertex,
                    XgHost3dProjectedVertex *output,
                    uint32_t *flags);

void xg_host_3d_configure_native_view(int enabled,
                                      int32_t center_offset_x_16_16);
void xg_host_3d_configure_native_view_aspect(
    int enabled, int32_t center_offset_x_16_16,
    uint16_t aspect_num, uint16_t aspect_den);
int32_t xg_host_3d_native_view_margin(void);
uint32_t xg_host_3d_native_view_depth_limit(uint32_t canonical_limit);

int xg_host_3d_rot_average4(const XgHost3dRotAverage4Input *input,
                             XgHost3dRotAverage4Output *output);
int xg_host_3d_rot_trans_pers4(const XgHost3dProject4Input *input,
                               XgHost3dRotTransPers4Output *output);

/* Exact host equivalents of OuterProduct0 and OuterProduct12. */
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
/* Exact host equivalent of CompMatrix @ 0x8004931c. */
int xg_host_3d_comp_matrix(const XgHost3dMatrix *left,
                           const XgHost3dMatrix *right,
                           XgHost3dMatrix *output);
int xg_host_3d_vector_normal(const XgHost3dLongVector *vector,
                             XgHost3dLongVector *normalized);

/* Exact host equivalent of the ScaleMatrix call at 0x80049dcc. */
int xg_host_3d_scale_matrix(XgHost3dMatrix *matrix,
                            const XgHost3dLongVector *scale);

#ifdef __cplusplus
}
#endif

#endif
