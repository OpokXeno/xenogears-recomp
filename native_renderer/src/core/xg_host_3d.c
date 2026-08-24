#include "xg_host_3d.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    FLAG_MAC1_POS = 1u << 30,
    FLAG_MAC2_POS = 1u << 29,
    FLAG_MAC3_POS = 1u << 28,
    FLAG_MAC1_NEG = 1u << 27,
    FLAG_MAC2_NEG = 1u << 26,
    FLAG_MAC3_NEG = 1u << 25,
    FLAG_IR1_SAT = 1u << 24,
    FLAG_IR2_SAT = 1u << 23,
    FLAG_IR3_SAT = 1u << 22,
    FLAG_SZ3_OTZ = 1u << 18,
    FLAG_DIV_OVF = 1u << 17,
    FLAG_MAC0_POS = 1u << 16,
    FLAG_MAC0_NEG = 1u << 15,
    FLAG_SX2_SAT = 1u << 14,
    FLAG_SY2_SAT = 1u << 13,
    FLAG_IR0_SAT = 1u << 12,
    FLAG_ERROR_MASK = 0x7f87e000u,
};

static const uint32_t FLAG_ERROR_BIT = UINT32_C(0x80000000);

typedef struct XgHost3dMathState {
    const XgHost3dProjection *projection;
    uint16_t depth[4];
    uint32_t flags;
    int16_t ir0;
} XgHost3dMathState;

_Static_assert(offsetof(XgHost3dMatrix, pad) == 18u,
               "XgHost3dMatrix must match the PsyQ MATRIX rotation layout");
_Static_assert(offsetof(XgHost3dMatrix, translation) == 20u,
               "XgHost3dMatrix must match the PsyQ MATRIX translation layout");
_Static_assert(sizeof(XgHost3dMatrix) == 32u,
               "XgHost3dMatrix must match the PsyQ MATRIX size");

static uint8_t division_table[0x101];
static int division_table_ready;
static int native_view_enabled;
static int32_t native_view_center_offset_x_16_16;
static uint32_t native_view_cull_margin;
static uint32_t native_view_aspect_num = 4u;
static uint32_t native_view_aspect_den = 3u;

void xg_host_3d_configure_native_view_aspect(
    int enabled, int32_t center_offset_x_16_16,
    uint16_t aspect_num, uint16_t aspect_den) {
    native_view_enabled = enabled != 0;
    native_view_center_offset_x_16_16 =
        native_view_enabled ? center_offset_x_16_16 : 0;
    native_view_cull_margin = native_view_enabled && center_offset_x_16_16 > 0
        ? (uint32_t)(((int64_t)center_offset_x_16_16 + INT64_C(0xffff)) >> 16u)
        : 0u;
    native_view_aspect_num = native_view_enabled && aspect_num != 0u
        ? aspect_num : 4u;
    native_view_aspect_den = native_view_enabled && aspect_den != 0u
        ? aspect_den : 3u;
}

void xg_host_3d_configure_native_view_margin(uint32_t margin) {
    native_view_cull_margin = native_view_enabled ? margin : 0u;
}

void xg_host_3d_configure_native_view(int enabled,
                                      int32_t center_offset_x_16_16) {
    xg_host_3d_configure_native_view_aspect(
        enabled, center_offset_x_16_16, 4u, 3u);
}

int32_t xg_host_3d_native_view_margin(void) {
    return native_view_enabled && native_view_cull_margin <= INT32_MAX
        ? (int32_t)native_view_cull_margin : 0;
}

uint32_t xg_host_3d_native_view_depth_limit(uint32_t canonical_limit) {
    uint64_t numerator;
    uint64_t denominator;
    uint64_t result;

    if (!native_view_enabled ||
        native_view_aspect_num * 3u <= native_view_aspect_den * 4u)
        return canonical_limit;
    numerator = (uint64_t)canonical_limit * 3u * native_view_aspect_num;
    denominator = 4u * native_view_aspect_den;
    result = (numerator + denominator / 2u) / denominator;
    return result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
}

static int64_t shift_right_floor(int64_t value, unsigned bits) {
    uint64_t magnitude;
    uint64_t rounded;

    if (bits == 0u || value >= 0) return value / ((int64_t)1 << bits);
    magnitude = (uint64_t)(-(value + 1)) + 1u;
    rounded = (magnitude + (((uint64_t)1 << bits) - 1u)) >> bits;
    return -(int64_t)rounded;
}

static int32_t wrap_i32(int64_t value) {
    const uint32_t low = (uint32_t)(uint64_t)value;

    if (low <= INT32_MAX) return (int32_t)low;
    return -1 - (int32_t)(UINT32_MAX - low);
}

static int16_t wrap_i16(int32_t value) {
    const uint16_t low = (uint16_t)(uint32_t)value;

    if (low <= INT16_MAX) return (int16_t)low;
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - low));
}

static uint32_t integer_square_root(uint64_t value) {
    uint64_t result = 0u;
    uint64_t bit = UINT64_C(1) << 62u;

    while (bit > value) bit >>= 2u;
    while (bit != 0u) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1u) + bit;
        } else {
            result >>= 1u;
        }
        bit >>= 2u;
    }
    return (uint32_t)result;
}

static unsigned count_leading_zeroes32(uint32_t value) {
    unsigned count = 0u;
    int bit;

    for (bit = 31; bit >= 0; --bit) {
        if ((value & (UINT32_C(1) << bit)) != 0u) break;
        ++count;
    }
    return count;
}

static void initialize_division_table(void) {
    uint32_t divisor;

    if (division_table_ready) return;
    for (divisor = 0x8000u; divisor < 0x10000u; divisor += 0x80u) {
        uint32_t approximation = 512u;
        unsigned iteration;

        for (iteration = 1u; iteration < 5u; ++iteration)
            approximation =
                (approximation *
                 (1024u * 512u - ((divisor >> 7u) * approximation))) >>
                18u;
        division_table[(divisor >> 7u) & 0xffu] =
            (uint8_t)(((approximation + 1u) >> 1u) - 0x101u);
    }
    division_table[0x100] = division_table[0xff];
    division_table_ready = 1;
}

static unsigned count_leading_zeroes16(uint16_t value) {
    unsigned count = 0u;
    int bit;

    for (bit = 15; bit >= 0; --bit) {
        if ((value & (uint16_t)(1u << bit)) != 0u) break;
        ++count;
    }
    return count;
}

static int32_t reciprocal(uint16_t divisor) {
    const int32_t x =
        0x101 + division_table[(((divisor & 0x7fffu) + 0x40u) >> 7u)];
    const int32_t first = ((int32_t)divisor * -x + 0x80) >> 8;

    return (x * (131072 + first) + 0x80) >> 8;
}

static int32_t perspective_divide(uint16_t h, uint16_t z, uint32_t *flags) {
    unsigned shift;
    uint32_t dividend;
    uint32_t divisor;
    uint32_t result;

    initialize_division_table();
    if ((uint32_t)z * 2u <= (uint32_t)h) {
        *flags |= FLAG_DIV_OVF;
        return 0x1ffff;
    }
    shift = count_leading_zeroes16(z);
    dividend = (uint32_t)h << shift;
    divisor = (uint32_t)z << shift;
    result = (uint32_t)(((uint64_t)dividend *
                         (uint32_t)reciprocal((uint16_t)(divisor | 0x8000u)) +
                         32768u) >>
                        16u);
    if (result > 0x1ffffu) result = 0x1ffffu;
    return (int32_t)result;
}

static void note_mac123_overflow(int64_t value, unsigned component,
                                 uint32_t *flags) {
    if (value > INT64_C(0x7ffffffffff))
        *flags |= 1u << (31u - component);
    if (value < -INT64_C(0x80000000000))
        *flags |= 1u << (28u - component);
}

static int64_t wrap_mac123(int64_t value, unsigned component,
                           uint32_t *flags) {
    const uint64_t mask = (UINT64_C(1) << 44u) - 1u;
    const uint64_t sign = UINT64_C(1) << 43u;
    const uint64_t low = (uint64_t)value & mask;

    note_mac123_overflow(value, component, flags);
    if ((low & sign) == 0u) return (int64_t)low;
    return -1 - (int64_t)(mask - low);
}

static void note_mac0_overflow(int64_t value, uint32_t *flags) {
    if (value > INT32_MAX) *flags |= FLAG_MAC0_POS;
    if (value < INT32_MIN) *flags |= FLAG_MAC0_NEG;
}

static int16_t saturate_ir(int32_t value, unsigned component,
                           uint32_t *flags) {
    if (value < -0x8000) {
        *flags |= 1u << (25u - component);
        return -0x8000;
    }
    if (value > 0x7fff) {
        *flags |= 1u << (25u - component);
        return 0x7fff;
    }
    return (int16_t)value;
}

static int16_t saturate_ir0(int32_t value, uint32_t *flags) {
    if (value < 0) {
        *flags |= FLAG_IR0_SAT;
        return 0;
    }
    if (value > 0x1000) {
        *flags |= FLAG_IR0_SAT;
        return 0x1000;
    }
    return (int16_t)value;
}

static uint16_t saturate_depth(int32_t value, uint32_t *flags) {
    if (value < 0) {
        *flags |= FLAG_SZ3_OTZ;
        return 0u;
    }
    if (value > 0xffff) {
        *flags |= FLAG_SZ3_OTZ;
        return 0xffffu;
    }
    return (uint16_t)value;
}

static int16_t saturate_screen(int64_t value, uint32_t flag,
                               uint32_t *flags) {
    if (value < -0x400) {
        *flags |= flag;
        return -0x400;
    }
    if (value > 0x3ff) {
        *flags |= flag;
        return 0x3ff;
    }
    return (int16_t)value;
}

static void set_error_flag(uint32_t *flags) {
    if ((*flags & FLAG_ERROR_MASK) != 0u) *flags |= FLAG_ERROR_BIT;
}

static void matrix_vector_sf12_lm0(const XgHost3dMatrix *matrix,
                                   const int16_t vector[3],
                                   const int32_t *translation,
                                   int32_t mac[3], int16_t ir[3],
                                   uint32_t *flags) {
    unsigned row;

    *flags = 0u;
    for (row = 0u; row < 3u; ++row) {
        int64_t accumulator = translation == NULL
                                  ? 0
                                  : (int64_t)translation[row] * 4096;

        accumulator = wrap_mac123(
            accumulator +
                (int64_t)matrix->rotation[row][0] * vector[0],
            row + 1u, flags);
        accumulator = wrap_mac123(
            accumulator +
                (int64_t)matrix->rotation[row][1] * vector[1],
            row + 1u, flags);
        accumulator = wrap_mac123(
            accumulator +
                (int64_t)matrix->rotation[row][2] * vector[2],
            row + 1u, flags);
        mac[row] = wrap_i32(shift_right_floor(accumulator, 12u));
        ir[row] = saturate_ir(mac[row], row + 1u, flags);
    }
    set_error_flag(flags);
}

static int op(const XgHost3dLongVector *left,
              const XgHost3dLongVector *right,
              XgHost3dLongVector *mac,
              uint32_t *flags,
              unsigned shift) {
    int16_t d[3];
    int16_t ir[3];
    int64_t raw[3];
    int32_t result[3];
    uint32_t command_flags = 0u;
    unsigned component;

    if (left == NULL || right == NULL || mac == NULL || flags == NULL)
        return 0;

    d[0] = wrap_i16(left->x);
    d[1] = wrap_i16(left->y);
    d[2] = wrap_i16(left->z);
    ir[0] = wrap_i16(right->x);
    ir[1] = wrap_i16(right->y);
    ir[2] = wrap_i16(right->z);
    raw[0] = (int64_t)d[1] * ir[2] - (int64_t)d[2] * ir[1];
    raw[1] = (int64_t)d[2] * ir[0] - (int64_t)d[0] * ir[2];
    raw[2] = (int64_t)d[0] * ir[1] - (int64_t)d[1] * ir[0];

    for (component = 0u; component < 3u; ++component) {
        result[component] = wrap_i32(shift_right_floor(raw[component], shift));
        (void)saturate_ir(result[component], component + 1u,
                          &command_flags);
    }
    set_error_flag(&command_flags);
    mac->x = result[0];
    mac->y = result[1];
    mac->z = result[2];
    *flags = command_flags;
    return 1;
}

int xg_host_3d_op0(const XgHost3dLongVector *left,
                   const XgHost3dLongVector *right,
                   XgHost3dLongVector *mac,
                   uint32_t *flags) {
    return op(left, right, mac, flags, 0u);
}

int xg_host_3d_op12(const XgHost3dLongVector *left,
                    const XgHost3dLongVector *right,
                    XgHost3dLongVector *mac,
                    uint32_t *flags) {
    return op(left, right, mac, flags, 12u);
}

int xg_host_3d_rtir(const XgHost3dMatrix *matrix,
                    const XgHost3dVector *vector,
                    XgHost3dVector *ir,
                    uint32_t *flags) {
    const int16_t input[3] = { vector == NULL ? 0 : vector->x,
                               vector == NULL ? 0 : vector->y,
                               vector == NULL ? 0 : vector->z };
    int32_t mac[3];
    int16_t result[3];
    uint32_t command_flags;

    if (matrix == NULL || vector == NULL || ir == NULL || flags == NULL)
        return 0;
    matrix_vector_sf12_lm0(matrix, input, NULL, mac, result,
                           &command_flags);
    ir->x = result[0];
    ir->y = result[1];
    ir->z = result[2];
    ir->pad = 0u;
    *flags = command_flags;
    return 1;
}

int xg_host_3d_rt(const XgHost3dMatrix *matrix,
                  const XgHost3dLongVector *vector,
                  XgHost3dLongVector *mac,
                  uint32_t *flags) {
    int16_t input[3];
    int32_t result[3];
    int16_t ir[3];
    uint32_t command_flags;

    if (matrix == NULL || vector == NULL || mac == NULL || flags == NULL)
        return 0;
    input[0] = wrap_i16(vector->x);
    input[1] = wrap_i16(vector->y);
    input[2] = wrap_i16(vector->z);
    matrix_vector_sf12_lm0(matrix, input, matrix->translation, result, ir,
                           &command_flags);
    mac->x = result[0];
    mac->y = result[1];
    mac->z = result[2];
    *flags = command_flags;
    return 1;
}

int xg_host_3d_comp_matrix(const XgHost3dMatrix *left,
                           const XgHost3dMatrix *right,
                           XgHost3dMatrix *output) {
    XgHost3dMatrix result = { 0 };
    XgHost3dMatrix rotation_only;
    XgHost3dLongVector transformed_translation;
    int32_t translated[3];
    uint32_t flags;
    unsigned column;
    unsigned row;

    if (left == NULL || right == NULL || output == NULL) return 0;
    for (column = 0u; column < 3u; ++column) {
        const XgHost3dVector source = {
            right->rotation[0][column],
            right->rotation[1][column],
            right->rotation[2][column],
            0u,
        };
        XgHost3dVector transformed;

        if (!xg_host_3d_rtir(left, &source, &transformed, &flags)) return 0;
        result.rotation[0][column] = transformed.x;
        result.rotation[1][column] = transformed.y;
        result.rotation[2][column] = transformed.z;
    }
    result.pad = result.rotation[2][2] < 0 ? UINT16_MAX : 0u;

    rotation_only = *left;
    memset(rotation_only.translation, 0, sizeof(rotation_only.translation));
    if (!xg_host_3d_rt(
            &rotation_only,
            &(XgHost3dLongVector){
                right->translation[0], right->translation[1],
                right->translation[2],
            },
            &transformed_translation, &flags))
        return 0;
    translated[0] = transformed_translation.x;
    translated[1] = transformed_translation.y;
    translated[2] = transformed_translation.z;
    for (row = 0u; row < 3u; ++row) {
        const int64_t sum = (int64_t)translated[row] + left->translation[row];

        if (sum < INT32_MIN || sum > INT32_MAX) return 0;
        result.translation[row] = (int32_t)sum;
    }
    *output = result;
    return 1;
}

int xg_host_3d_vector_normal(const XgHost3dLongVector *vector,
                             XgHost3dLongVector *normalized) {
    int16_t component[3];
    int32_t output[3];
    uint64_t length_squared;
    uint32_t normalized_length;
    uint32_t factor;
    unsigned leading_zeroes;
    unsigned shift;
    unsigned index;

    if (vector == NULL || normalized == NULL) return 0;
    component[0] = wrap_i16(vector->x);
    component[1] = wrap_i16(vector->y);
    component[2] = wrap_i16(vector->z);
    length_squared =
        (uint64_t)((int32_t)component[0] * component[0]) +
        (uint64_t)((int32_t)component[1] * component[1]) +
        (uint64_t)((int32_t)component[2] * component[2]);
    if (length_squared == 0u || length_squared > INT32_MAX) return 0;

    /* VectorNormal @ 0x80048d7c normalizes the squared length into [0x40,
     * 0xff] and indexes the resident reciprocal-square-root table. Every one
     * of its 192 entries equals floor(sqrt(2^30 / normalized_length)). */
    leading_zeroes = count_leading_zeroes32((uint32_t)length_squared) & ~1u;
    if (leading_zeroes < 24u)
        normalized_length =
            (uint32_t)length_squared >> (24u - leading_zeroes);
    else
        normalized_length =
            (uint32_t)length_squared << (leading_zeroes - 24u);
    if (normalized_length < 0x40u || normalized_length > 0xffu) return 0;
    factor = integer_square_root((UINT64_C(1) << 30u) / normalized_length);
    shift = (31u - leading_zeroes) >> 1u;
    for (index = 0u; index < 3u; ++index)
        output[index] = wrap_i32(shift_right_floor(
            (int64_t)component[index] * factor, shift));
    normalized->x = output[0];
    normalized->y = output[1];
    normalized->z = output[2];
    return 1;
}

static int32_t scale_matrix_component(int16_t component, int32_t scale) {
    const uint32_t product =
        (uint32_t)(int32_t)component * (uint32_t)scale;

    return wrap_i32(shift_right_floor(wrap_i32((int64_t)product), 12u));
}

int xg_host_3d_scale_matrix(XgHost3dMatrix *matrix,
                            const XgHost3dLongVector *scale) {
    const int32_t factors[3] = { scale == NULL ? 0 : scale->x,
                                 scale == NULL ? 0 : scale->y,
                                 scale == NULL ? 0 : scale->z };
    int32_t scaled[3][3];
    unsigned row;
    unsigned column;

    if (matrix == NULL || scale == NULL) return 0;
    for (row = 0u; row < 3u; ++row) {
        for (column = 0u; column < 3u; ++column) {
            scaled[row][column] = scale_matrix_component(
                matrix->rotation[row][column], factors[column]);
        }
    }
    for (row = 0u; row < 3u; ++row) {
        for (column = 0u; column < 3u; ++column)
            matrix->rotation[row][column] =
                wrap_i16(scaled[row][column]);
    }

    /* The final MIPS sw writes the high half of scaled m[2][2] into pad. */
    matrix->pad = (uint16_t)((uint32_t)scaled[2][2] >> 16u);
    return 1;
}

static void project_vertex(XgHost3dMathState *state,
                           const XgHost3dVector *vertex, int set_depth_cue,
                           XgHost3dProjectedVertex *output) {
    const XgHost3dProjection *projection = state->projection;
    int64_t raw[3];
    int32_t mac[3];
    int16_t ir[3];
    int32_t scale;
    int64_t x_16_16;
    int64_t y_16_16;
    unsigned row;

    for (row = 0u; row < 3u; ++row) {
        raw[row] = (int64_t)projection->translation[row] * 4096 +
                   (int64_t)projection->rotation[row][0] * vertex->x +
                   (int64_t)projection->rotation[row][1] * vertex->y +
                   (int64_t)projection->rotation[row][2] * vertex->z;
        note_mac123_overflow(shift_right_floor(raw[row], 12u), row + 1u,
                             &state->flags);
        mac[row] = wrap_i32(shift_right_floor(raw[row], 12u));
        ir[row] = saturate_ir(mac[row], row + 1u, &state->flags);
    }

    state->depth[0] = state->depth[1];
    state->depth[1] = state->depth[2];
    state->depth[2] = state->depth[3];
    state->depth[3] = saturate_depth(mac[2], &state->flags);
    scale = perspective_divide(projection->projection_distance,
                               state->depth[3], &state->flags);
    x_16_16 = (int64_t)projection->screen_offset_x + (int64_t)ir[0] * scale;
    y_16_16 = (int64_t)projection->screen_offset_y + (int64_t)ir[1] * scale;
    output->x_16_16 = wrap_i32(x_16_16);
    output->y_16_16 = wrap_i32(y_16_16);
    output->projective_view_x = mac[0];
    output->projective_view_y = mac[1];
    output->projective_view_z = mac[2];
    output->projective_offset_x_16_16 = projection->screen_offset_x;
    output->projective_offset_y_16_16 = projection->screen_offset_y;
    output->projective_distance = projection->projection_distance;
    output->projective_position =
        mac[0] >= -0x8000 && mac[0] <= 0x7fff &&
        mac[1] >= -0x8000 && mac[1] <= 0x7fff &&
        mac[2] > 0 && mac[2] <= 0xffff &&
        (uint32_t)mac[2] * 2u > projection->projection_distance;
    if (native_view_enabled &&
        x_16_16 + native_view_center_offset_x_16_16 >= INT32_MIN &&
        x_16_16 + native_view_center_offset_x_16_16 <= INT32_MAX &&
        y_16_16 >= INT32_MIN && y_16_16 <= INT32_MAX) {
        output->native_view_x_16_16 = (int32_t)(
            x_16_16 + native_view_center_offset_x_16_16);
        output->native_view_y_16_16 = (int32_t)y_16_16;
        output->native_view_position = 1u;
        output->projective_native_offset_x_16_16 =
            native_view_center_offset_x_16_16;
    }
    output->x = saturate_screen(shift_right_floor(x_16_16, 16u),
                                FLAG_SX2_SAT, &state->flags);
    output->y = saturate_screen(shift_right_floor(y_16_16, 16u),
                                FLAG_SY2_SAT, &state->flags);
    output->z = state->depth[3];

    if (set_depth_cue) {
        const int64_t mac0 =
            (int64_t)projection->depth_cue_a * scale + projection->depth_cue_b;

        note_mac0_overflow(mac0, &state->flags);
        state->ir0 = saturate_ir0(
            wrap_i32(shift_right_floor(mac0, 12u)), &state->flags);
    }
}

int xg_host_3d_rtps(const XgHost3dProjection *projection,
                    const XgHost3dVector *vertex,
                    XgHost3dProjectedVertex *output,
                    uint32_t *flags) {
    XgHost3dMathState state;

    if (projection == NULL || vertex == NULL || output == NULL || flags == NULL)
        return 0;
    memset(&state, 0, sizeof(state));
    memset(output, 0, sizeof(*output));
    state.projection = projection;
    project_vertex(&state, vertex, 1, output);
    set_error_flag(&state.flags);
    *flags = state.flags;
    return 1;
}

int xg_host_3d_rot_trans_pers4(const XgHost3dProject4Input *input,
                               XgHost3dRotTransPers4Output *output) {
    XgHost3dMathState state;
    unsigned index;

    if (input == NULL || output == NULL) return 0;
    memset(output, 0, sizeof(*output));
    memset(&state, 0, sizeof(state));
    state.projection = &input->projection;

    for (index = 0u; index < 3u; ++index)
        project_vertex(&state, &input->vertices[index], index == 2u,
                       &output->vertices[index]);
    set_error_flag(&state.flags);
    output->rtpt_flags = state.flags;

    state.flags = 0u;
    project_vertex(&state, &input->vertices[3], 1,
                   &output->vertices[3]);
    set_error_flag(&state.flags);
    output->rtps_flags = state.flags;
    output->projection_flags = output->rtpt_flags | output->rtps_flags;
    output->depth_cue = state.ir0;
    output->fourth_depth = state.depth[3] >> 2u;
    return 1;
}

int xg_host_3d_rot_average4(const XgHost3dRotAverage4Input *input,
                            XgHost3dRotAverage4Output *output) {
    XgHost3dRotTransPers4Output projected;
    uint32_t flags = 0u;
    int64_t average;
    int32_t mac0;

    if (input == NULL || output == NULL ||
        !xg_host_3d_rot_trans_pers4(input, &projected))
        return 0;
    memset(output, 0, sizeof(*output));
    memcpy(output->vertices, projected.vertices, sizeof(output->vertices));
    output->depth_cue = projected.depth_cue;
    output->rtpt_flags = projected.rtpt_flags;
    output->rtps_flags = projected.rtps_flags;
    output->projection_flags = projected.projection_flags;

    average = (int64_t)input->projection.average_z_scale4 *
              ((uint32_t)projected.vertices[0].z + projected.vertices[1].z +
               projected.vertices[2].z + projected.vertices[3].z);
    note_mac0_overflow(average, &flags);
    mac0 = wrap_i32(shift_right_floor(average, 12u));
    output->ordering_depth = saturate_depth(mac0, &flags);
    set_error_flag(&flags);
    output->avsz4_flags = flags;
    return 1;
}
