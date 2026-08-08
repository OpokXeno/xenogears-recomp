#include "xg_host_3d.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

enum {
    REF_FLAG_MAC1_POS = 1u << 30,
    REF_FLAG_MAC2_NEG = 1u << 26,
    REF_FLAG_IR1_SAT = 1u << 24,
    REF_FLAG_IR2_SAT = 1u << 23,
    REF_FLAG_IR3_SAT = 1u << 22,
    REF_FLAG_ERROR_MASK = 0x7f87e000u,
};

static const uint32_t REF_FLAG_ERROR = UINT32_C(0x80000000);

static int16_t ref_i16(uint32_t bits) {
    const uint32_t low = bits & UINT32_C(0xffff);

    return (int16_t)((int32_t)(low ^ UINT32_C(0x8000)) - 0x8000);
}

static int32_t ref_i32(uint32_t bits) {
    return (int32_t)((int64_t)(bits ^ UINT32_C(0x80000000)) -
                     INT64_C(0x80000000));
}

static int64_t ref_sar12(int64_t value) {
    const int64_t quotient = value / 4096;
    const int64_t remainder = value % 4096;

    return quotient - (remainder < 0 ? 1 : 0);
}

static int64_t ref_mac44_step(int64_t accumulator, int64_t term,
                              unsigned component, uint32_t *flags) {
    const uint64_t mask = (UINT64_C(1) << 44) - 1u;
    const uint64_t sign = UINT64_C(1) << 43;
    const int64_t sum = accumulator + term;
    const uint64_t low = (uint64_t)sum & mask;

    if (sum > INT64_C(0x7ffffffffff))
        *flags |= UINT32_C(1) << (30u - component);
    if (sum < -INT64_C(0x80000000000))
        *flags |= UINT32_C(1) << (27u - component);
    return (int64_t)(low ^ sign) - (int64_t)sign;
}

static int16_t ref_ir(int32_t value, unsigned component, uint32_t *flags) {
    if (value < -0x8000) {
        *flags |= UINT32_C(1) << (24u - component);
        return -0x8000;
    }
    if (value > 0x7fff) {
        *flags |= UINT32_C(1) << (24u - component);
        return 0x7fff;
    }
    return (int16_t)value;
}

static void ref_finish_flags(uint32_t *flags) {
    if ((*flags & REF_FLAG_ERROR_MASK) != 0u) *flags |= REF_FLAG_ERROR;
}

static void ref_op12(const XgHost3dLongVector *left,
                     const XgHost3dLongVector *right,
                     XgHost3dLongVector *output, uint32_t *flags) {
    const int16_t a[3] = { ref_i16((uint32_t)left->x),
                           ref_i16((uint32_t)left->y),
                           ref_i16((uint32_t)left->z) };
    const int16_t b[3] = { ref_i16((uint32_t)right->x),
                           ref_i16((uint32_t)right->y),
                           ref_i16((uint32_t)right->z) };
    const int64_t products[3] = {
        (int64_t)a[1] * b[2] - (int64_t)a[2] * b[1],
        (int64_t)a[2] * b[0] - (int64_t)a[0] * b[2],
        (int64_t)a[0] * b[1] - (int64_t)a[1] * b[0],
    };
    int32_t result[3];
    unsigned component;

    *flags = 0u;
    for (component = 0u; component < 3u; ++component) {
        result[component] = (int32_t)ref_sar12(products[component]);
        (void)ref_ir(result[component], component, flags);
    }
    ref_finish_flags(flags);
    output->x = result[0];
    output->y = result[1];
    output->z = result[2];
}

static void ref_mvmva(const XgHost3dMatrix *matrix, const int16_t vector[3],
                      const int32_t *translation, int32_t mac[3],
                      int16_t ir[3], uint32_t *flags) {
    unsigned row;

    *flags = 0u;
    for (row = 0u; row < 3u; ++row) {
        int64_t accumulator = translation == NULL
                                  ? 0
                                  : (int64_t)translation[row] * 4096;
        unsigned column;

        for (column = 0u; column < 3u; ++column) {
            accumulator = ref_mac44_step(
                accumulator,
                (int64_t)matrix->rotation[row][column] * vector[column],
                row, flags);
        }
        mac[row] = (int32_t)ref_sar12(accumulator);
        ir[row] = ref_ir(mac[row], row, flags);
    }
    ref_finish_flags(flags);
}

static int32_t ref_scale_component(int16_t component, int32_t factor) {
    const uint32_t low_product =
        (uint32_t)(int32_t)component * (uint32_t)factor;

    return (int32_t)ref_sar12(ref_i32(low_product));
}

static void ref_scale_matrix(XgHost3dMatrix *matrix,
                             const XgHost3dLongVector *scale) {
    const int32_t factor[3] = { scale->x, scale->y, scale->z };
    int32_t result[3][3];
    unsigned row;
    unsigned column;

    for (row = 0u; row < 3u; ++row) {
        for (column = 0u; column < 3u; ++column) {
            result[row][column] = ref_scale_component(
                matrix->rotation[row][column], factor[column]);
        }
    }
    for (row = 0u; row < 3u; ++row) {
        for (column = 0u; column < 3u; ++column)
            matrix->rotation[row][column] =
                ref_i16((uint32_t)result[row][column]);
    }
    matrix->pad = (uint16_t)((uint32_t)result[2][2] >> 16u);
}

static XgHost3dMatrix identity_matrix(void) {
    XgHost3dMatrix matrix = { 0 };

    matrix.rotation[0][0] = 4096;
    matrix.rotation[1][1] = 4096;
    matrix.rotation[2][2] = 4096;
    return matrix;
}

static int test_known_vectors(void) {
    const XgHost3dLongVector axis_x = { 4096, 0, 0 };
    const XgHost3dLongVector axis_y = { 0, 4096, 0 };
    const XgHost3dLongVector short_axis_x = { 16, 0, 0 };
    const XgHost3dLongVector short_axis_y = { 0, 16, 0 };
    const XgHost3dVector short_vector = { 123, -456, 789, 0xbeefu };
    const XgHost3dLongVector long_vector = { 4, -8, 12 };
    const XgHost3dLongVector scale = { 8192, -4096, 2048 };
    const XgHost3dLongVector normal_input = { 4096, 0, 0 };
    XgHost3dLongVector long_output;
    XgHost3dVector short_output;
    XgHost3dMatrix matrix = identity_matrix();
    uint32_t flags;

    CHECK(xg_host_3d_op12(&axis_x, &axis_y, &long_output, &flags));
    CHECK(long_output.x == 0 && long_output.y == 0 &&
           long_output.z == 4096 && flags == 0u);
    CHECK(xg_host_3d_op0(&short_axis_x, &short_axis_y, &long_output, &flags));
    CHECK(long_output.x == 0 && long_output.y == 0 &&
          long_output.z == 256 && flags == 0u);
    CHECK(xg_host_3d_vector_normal(&normal_input, &long_output));
    CHECK(long_output.x == 4096 && long_output.y == 0 &&
          long_output.z == 0);

    CHECK(xg_host_3d_rtir(&matrix, &short_vector, &short_output, &flags));
    CHECK(short_output.x == 123 && short_output.y == -456 &&
          short_output.z == 789 && short_output.pad == 0u && flags == 0u);

    matrix.translation[0] = 100;
    matrix.translation[1] = -200;
    matrix.translation[2] = 300;
    CHECK(xg_host_3d_rt(&matrix, &long_vector, &long_output, &flags));
    CHECK(long_output.x == 104 && long_output.y == -208 &&
          long_output.z == 312 && flags == 0u);

    {
        const XgHost3dMatrix right = {
            .rotation = {{0, -4096, 0}, {4096, 0, 0}, {0, 0, -4096}},
            .translation = {4, -8, 12},
        };
        XgHost3dMatrix composed;

        CHECK(xg_host_3d_comp_matrix(&matrix, &right, &composed));
        CHECK(memcmp(composed.rotation, right.rotation,
                     sizeof(composed.rotation)) == 0);
        CHECK(composed.pad == UINT16_MAX);
        CHECK(composed.translation[0] == 104);
        CHECK(composed.translation[1] == -208);
        CHECK(composed.translation[2] == 312);
    }

    matrix.pad = 0xbeefu;
    CHECK(xg_host_3d_scale_matrix(&matrix, &scale));
    CHECK(matrix.rotation[0][0] == 8192);
    CHECK(matrix.rotation[1][1] == -4096);
    CHECK(matrix.rotation[2][2] == 2048);
    CHECK(matrix.pad == 0u);
    CHECK(matrix.translation[0] == 100 && matrix.translation[1] == -200 &&
          matrix.translation[2] == 300);
    return 1;
}

static int test_rot_trans_pers4_uses_fourth_depth_without_avsz4(void) {
    XgHost3dProject4Input input = { 0 };
    XgHost3dRotTransPers4Output projected;
    XgHost3dRotAverage4Output averaged;
    uint32_t index;

    input.projection.rotation[0][0] = 4096;
    input.projection.rotation[1][1] = 4096;
    input.projection.rotation[2][2] = 4096;
    input.projection.screen_offset_x = 160 << 16;
    input.projection.screen_offset_y = 112 << 16;
    input.projection.projection_distance = 256u;
    input.projection.average_z_scale4 = 1024;
    input.vertices[0] = (XgHost3dVector){ -64, -32, 1024, 0u };
    input.vertices[1] = (XgHost3dVector){ 64, -32, 1024, 0u };
    input.vertices[2] = (XgHost3dVector){ -64, 32, 1024, 0u };
    input.vertices[3] = (XgHost3dVector){ 64, 32, 2048, 0u };

    CHECK(xg_host_3d_rot_trans_pers4(&input, &projected));
    CHECK(projected.fourth_depth == 512u);
    CHECK(projected.projection_flags == 0u);
    CHECK(xg_host_3d_rot_average4(&input, &averaged));
    CHECK(averaged.ordering_depth == 1280u);
    for (index = 0u; index < XG_HOST_3D_VERTEX_COUNT; ++index) {
        CHECK(projected.vertices[index].x == averaged.vertices[index].x);
        CHECK(projected.vertices[index].y == averaged.vertices[index].y);
        CHECK(projected.vertices[index].z == averaged.vertices[index].z);
    }
    CHECK(projected.projection_flags == averaged.projection_flags);
    return 1;
}

static int test_native_view_projection_metadata(void) {
    XgHost3dProjection projection = { 0 };
    XgHost3dProjectedVertex projected;
    const XgHost3dVector vertex = { 64, -32, 1024, 0u };
    uint32_t flags;

    projection.rotation[0][0] = 4096;
    projection.rotation[1][1] = 4096;
    projection.rotation[2][2] = 4096;
    projection.screen_offset_x = 160 << 16;
    projection.screen_offset_y = 112 << 16;
    projection.projection_distance = 256u;

    xg_host_3d_configure_native_view(0, 0);
    CHECK(xg_host_3d_native_view_margin() == 0);
    CHECK(xg_host_3d_rtps(&projection, &vertex, &projected, &flags));
    CHECK(projected.x_16_16 == 176 << 16);
    CHECK(projected.y_16_16 == 104 << 16);
    CHECK(!projected.native_view_position);

    xg_host_3d_configure_native_view(1, 53 << 16);
    CHECK(xg_host_3d_native_view_margin() == 53);
    CHECK(xg_host_3d_rtps(&projection, &vertex, &projected, &flags));
    CHECK(projected.x == 176 && projected.y == 104);
    CHECK(projected.x_16_16 == 176 << 16);
    CHECK(projected.y_16_16 == 104 << 16);
    CHECK(projected.native_view_x_16_16 == 229 << 16);
    CHECK(projected.native_view_y_16_16 == 104 << 16);
    CHECK(projected.native_view_position);
    xg_host_3d_configure_native_view(0, 0);
    return 1;
}

static int test_native_view_scales_world_depth_limit(void) {
    xg_host_3d_configure_native_view_aspect(1, 54 << 16, 16u, 9u);
    CHECK(xg_host_3d_native_view_margin() == 54);
    CHECK(xg_host_3d_native_view_depth_limit(0x0d80u) == 0x1200u);
    xg_host_3d_configure_native_view(0, 0);
    CHECK(xg_host_3d_native_view_depth_limit(0x0d80u) == 0x0d80u);
    return 1;
}

static int test_saturation_and_width_edges(void) {
    const XgHost3dLongVector op_left = { 32767, 32767, 32767 };
    const XgHost3dLongVector op_right = { 32767, -32768, 32767 };
    const XgHost3dVector max_vector = { 32767, 32767, 32767, 0 };
    XgHost3dLongVector long_output;
    XgHost3dVector short_output;
    XgHost3dMatrix matrix;
    uint32_t flags;
    unsigned row;
    unsigned column;

    CHECK(xg_host_3d_op12(&op_left, &op_right, &long_output, &flags));
    CHECK(long_output.x == 524264 && long_output.y == 0 &&
          long_output.z == -524265);
    CHECK(flags == (REF_FLAG_ERROR | REF_FLAG_IR1_SAT | REF_FLAG_IR3_SAT));

    memset(&matrix, 0, sizeof(matrix));
    for (row = 0u; row < 3u; ++row)
        for (column = 0u; column < 3u; ++column)
            matrix.rotation[row][column] = 32767;
    CHECK(xg_host_3d_rtir(&matrix, &max_vector, &short_output, &flags));
    CHECK(short_output.x == 32767 && short_output.y == 32767 &&
          short_output.z == 32767);
    CHECK(flags == (REF_FLAG_ERROR | REF_FLAG_IR1_SAT |
                    REF_FLAG_IR2_SAT | REF_FLAG_IR3_SAT));

    for (column = 0u; column < 3u; ++column) {
        matrix.rotation[0][column] = 32767;
        matrix.rotation[1][column] = -32768;
        matrix.rotation[2][column] = 0;
    }
    matrix.translation[0] = INT32_MAX;
    matrix.translation[1] = INT32_MIN;
    matrix.translation[2] = 0;
    CHECK(xg_host_3d_rt(&matrix,
                        &(XgHost3dLongVector){ 32767, 32767, 32767 },
                        &long_output, &flags));
    CHECK((flags & REF_FLAG_MAC1_POS) != 0u);
    CHECK((flags & REF_FLAG_MAC2_NEG) != 0u);

    memset(&matrix, 0, sizeof(matrix));
    matrix.rotation[2][2] = 32767;
    matrix.pad = 0xbeefu;
    matrix.translation[0] = 11;
    matrix.translation[1] = 22;
    matrix.translation[2] = 33;
    CHECK(xg_host_3d_scale_matrix(
        &matrix, &(XgHost3dLongVector){ 0, 0, INT32_MAX }));
    CHECK(matrix.rotation[2][2] == -8 && matrix.pad == 7u);
    CHECK(matrix.translation[0] == 11 && matrix.translation[1] == 22 &&
          matrix.translation[2] == 33);
    return 1;
}

static uint32_t random_state = UINT32_C(0x8f6a3d21);

static uint32_t next_random(void) {
    uint32_t value = random_state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    random_state = value;
    return value;
}

static XgHost3dLongVector random_long_vector(void) {
    XgHost3dLongVector vector;

    vector.x = ref_i32(next_random());
    vector.y = ref_i32(next_random());
    vector.z = ref_i32(next_random());
    return vector;
}

static XgHost3dMatrix random_matrix(void) {
    XgHost3dMatrix matrix;
    unsigned row;
    unsigned column;

    for (row = 0u; row < 3u; ++row)
        for (column = 0u; column < 3u; ++column)
            matrix.rotation[row][column] = ref_i16(next_random());
    matrix.pad = (uint16_t)next_random();
    matrix.translation[0] = ref_i32(next_random());
    matrix.translation[1] = ref_i32(next_random());
    matrix.translation[2] = ref_i32(next_random());
    return matrix;
}

static int test_deterministic_fuzz(void) {
    unsigned iteration;

    for (iteration = 0u; iteration < 20000u; ++iteration) {
        XgHost3dMatrix matrix = random_matrix();
        XgHost3dMatrix actual_scaled = matrix;
        XgHost3dMatrix expected_scaled = matrix;
        const XgHost3dLongVector left = random_long_vector();
        const XgHost3dLongVector right = random_long_vector();
        const XgHost3dLongVector scale = random_long_vector();
        const XgHost3dVector short_vector = {
            ref_i16(next_random()), ref_i16(next_random()),
            ref_i16(next_random()), (uint16_t)next_random()
        };
        const int16_t rtir_input[3] = {
            short_vector.x, short_vector.y, short_vector.z
        };
        const int16_t rt_input[3] = {
            ref_i16((uint32_t)right.x), ref_i16((uint32_t)right.y),
            ref_i16((uint32_t)right.z)
        };
        XgHost3dLongVector actual_long;
        XgHost3dLongVector expected_long;
        XgHost3dVector actual_short;
        int32_t expected_mac[3];
        int16_t expected_ir[3];
        uint32_t actual_flags;
        uint32_t expected_flags;

        ref_op12(&left, &right, &expected_long, &expected_flags);
        CHECK(xg_host_3d_op12(&left, &right, &actual_long, &actual_flags));
        CHECK(memcmp(&actual_long, &expected_long, sizeof(actual_long)) == 0);
        CHECK(actual_flags == expected_flags);

        ref_mvmva(&matrix, rtir_input, NULL, expected_mac, expected_ir,
                  &expected_flags);
        CHECK(xg_host_3d_rtir(&matrix, &short_vector, &actual_short,
                              &actual_flags));
        CHECK(actual_short.x == expected_ir[0] &&
              actual_short.y == expected_ir[1] &&
              actual_short.z == expected_ir[2] && actual_short.pad == 0u);
        CHECK(actual_flags == expected_flags);

        ref_mvmva(&matrix, rt_input, matrix.translation, expected_mac,
                  expected_ir, &expected_flags);
        CHECK(xg_host_3d_rt(&matrix, &right, &actual_long, &actual_flags));
        CHECK(actual_long.x == expected_mac[0] &&
              actual_long.y == expected_mac[1] &&
              actual_long.z == expected_mac[2]);
        CHECK(actual_flags == expected_flags);

        ref_scale_matrix(&expected_scaled, &scale);
        CHECK(xg_host_3d_scale_matrix(&actual_scaled, &scale));
        CHECK(memcmp(&actual_scaled, &expected_scaled,
                     sizeof(actual_scaled)) == 0);
    }
    return 1;
}

static int test_invalid_arguments(void) {
    XgHost3dMatrix matrix = identity_matrix();
    XgHost3dLongVector long_vector = { 0, 0, 0 };
    XgHost3dVector short_vector = { 0, 0, 0, 0 };
    XgHost3dProject4Input project4 = { 0 };
    XgHost3dRotTransPers4Output projected;
    uint32_t flags;

    CHECK(!xg_host_3d_op0(NULL, &long_vector, &long_vector, &flags));
    CHECK(!xg_host_3d_op0(&long_vector, NULL, &long_vector, &flags));
    CHECK(!xg_host_3d_op0(&long_vector, &long_vector, NULL, &flags));
    CHECK(!xg_host_3d_op0(&long_vector, &long_vector, &long_vector, NULL));
    CHECK(!xg_host_3d_op12(NULL, &long_vector, &long_vector, &flags));
    CHECK(!xg_host_3d_op12(&long_vector, NULL, &long_vector, &flags));
    CHECK(!xg_host_3d_op12(&long_vector, &long_vector, NULL, &flags));
    CHECK(!xg_host_3d_op12(&long_vector, &long_vector, &long_vector, NULL));
    CHECK(!xg_host_3d_rtir(NULL, &short_vector, &short_vector, &flags));
    CHECK(!xg_host_3d_rtir(&matrix, NULL, &short_vector, &flags));
    CHECK(!xg_host_3d_rtir(&matrix, &short_vector, NULL, &flags));
    CHECK(!xg_host_3d_rtir(&matrix, &short_vector, &short_vector, NULL));
    CHECK(!xg_host_3d_rt(NULL, &long_vector, &long_vector, &flags));
    CHECK(!xg_host_3d_rt(&matrix, NULL, &long_vector, &flags));
    CHECK(!xg_host_3d_rt(&matrix, &long_vector, NULL, &flags));
    CHECK(!xg_host_3d_rt(&matrix, &long_vector, &long_vector, NULL));
    CHECK(!xg_host_3d_comp_matrix(NULL, &matrix, &matrix));
    CHECK(!xg_host_3d_comp_matrix(&matrix, NULL, &matrix));
    CHECK(!xg_host_3d_comp_matrix(&matrix, &matrix, NULL));
    CHECK(!xg_host_3d_scale_matrix(NULL, &long_vector));
    CHECK(!xg_host_3d_scale_matrix(&matrix, NULL));
    CHECK(!xg_host_3d_vector_normal(NULL, &long_vector));
    CHECK(!xg_host_3d_vector_normal(&long_vector, NULL));
    CHECK(!xg_host_3d_vector_normal(
        &(XgHost3dLongVector){0, 0, 0}, &long_vector));
    CHECK(!xg_host_3d_rot_trans_pers4(NULL, &projected));
    CHECK(!xg_host_3d_rot_trans_pers4(&project4, NULL));
    return 1;
}

int main(void) {
    return (test_known_vectors() &&
            test_rot_trans_pers4_uses_fourth_depth_without_avsz4() &&
            test_native_view_projection_metadata() &&
            test_native_view_scales_world_depth_limit() &&
            test_saturation_and_width_edges() &&
            test_deterministic_fuzz() && test_invalid_arguments())
               ? 0
               : 1;
}
