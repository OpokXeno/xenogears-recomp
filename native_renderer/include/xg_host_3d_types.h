#ifndef XG_HOST_3D_TYPES_H
#define XG_HOST_3D_TYPES_H

#include <stdint.h>

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
    int32_t projective_view_x;
    int32_t projective_view_y;
    int32_t projective_view_z;
    int32_t projective_offset_x_16_16;
    int32_t projective_offset_y_16_16;
    int32_t projective_native_offset_x_16_16;
    int32_t projective_native_offset_y_16_16;
    uint16_t projective_distance;
    uint8_t projective_position;
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

#endif
