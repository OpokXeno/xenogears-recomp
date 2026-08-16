#include "xg_world_horizon.h"
#include "xg_world_horizon_source_capture.h"

#include <stdint.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

typedef struct TestReader {
    uint32_t fail_address;
    uint32_t read_count;
    bool touched_output;
    bool mutate_vertex;
} TestReader;

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static bool output_address(uint32_t address) {
    return (address >= UINT32_C(0x8009c744) &&
            address < UINT32_C(0x8009c7e4)) ||
           (address >= UINT32_C(0x8009d3d8) &&
            address < UINT32_C(0x8009d3f0)) ||
           (address >= UINT32_C(0x1f800000) &&
            address < UINT32_C(0x1f800060)) ||
           (address >= UINT32_C(0x800a0000) &&
            address < UINT32_C(0x800b0000));
}

static bool test_read_u16(void *context, uint32_t address,
                          uint16_t *out_value) {
    TestReader *reader = context;

    ++reader->read_count;
    reader->touched_output |= output_address(address);
    if (address == reader->fail_address) return false;
    if (address != UINT32_C(0x8009bd3a)) return false;
    *out_value = 0u;
    return true;
}

static bool test_read_u32(void *context, uint32_t address,
                          uint32_t *out_value) {
    static const uint32_t matrix[8] = {
        0x00001000u, 0x00000000u, 0xf8020ddcu, 0x07fe0000u,
        0x00000ddcu, 0u, 273u, 798u,
    };
    static const XgHost3dVector vertices[2][4] = {
        { { -4096, -896, 4032, 0u }, { 0, -896, 4032, 0u },
          { -4096, -640, 4032, 0u }, { 0, -640, 4032, 0u } },
        { { 0, -896, 4032, 0u }, { 4096, -896, 4032, 0u },
          { 0, -640, 4032, 0u }, { 4096, -640, 4032, 0u } },
    };
    TestReader *reader = context;

    ++reader->read_count;
    reader->touched_output |= output_address(address);
    if (address == reader->fail_address) return false;
    if (address == UINT32_C(0x800523f0)) {
        *out_value = UINT32_C(0x10000000);
        return true;
    }
    if (address >= UINT32_C(0x8009c808) &&
        address < UINT32_C(0x8009c828) && (address & 3u) == 0u) {
        *out_value = matrix[(address - UINT32_C(0x8009c808)) / 4u];
        return true;
    }
    if (address >= UINT32_C(0x8009a300) &&
        address < UINT32_C(0x8009a340) && (address & 3u) == 0u) {
        const uint32_t offset = address - UINT32_C(0x8009a300);
        const XgHost3dVector *vertex = &vertices[offset / 8u / 4u]
            [(offset / 8u) % 4u];

        if ((offset & 4u) == 0u)
            *out_value = pack_s16(
                reader->mutate_vertex && offset == 0u
                    ? (int16_t)(vertex->x + 1) : vertex->x,
                vertex->y);
        else
            *out_value = pack_s16(vertex->z, (int16_t)vertex->pad);
        return true;
    }
    switch (address) {
    case UINT32_C(0x8009bcdc): *out_value = 0x100u; return true;
    case UINT32_C(0x8009be0c): *out_value = 140u; return true;
    case UINT32_C(0x8009d7cc): *out_value = 0u; return true;
    case UINT32_C(0x80050100): *out_value = 2u; return true;
    case UINT32_C(0x8009d7f0): *out_value = 1u; return true;
    default: return false;
    }
}

static XgWorldHorizonCaptureResult capture_source(
    TestReader *context, XgWorldHorizonCapture *capture) {
    const XgWorldHorizonCaptureRequest request = {
        .authentication_generation = 7u,
        .caller_return = UINT32_C(0x80071b58),
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 140 << 16,
        .projection_distance = 0x100u,
        .raster = {
            .draw_area_right = 319u,
            .draw_area_bottom = 239u,
        },
        .projection_state_authenticated = true,
    };
    const XgWorldHorizonAuthenticatedReader reader = {
        .context = context,
        .read_u16 = test_read_u16,
        .read_u32 = test_read_u32,
        .authentication_generation = 7u,
        .authenticated = true,
    };

    return xg_world_horizon_source_capture(&request, &reader, capture);
}

static int test_captured_source_and_projection(void) {
    static const int16_t expected_xy[2][4][2] = {
        { { -113, -28 }, { 160, -28 }, { -105, -9 }, { 160, -9 } },
        { { 160, -28 }, { 432, -28 }, { 160, -9 }, { 424, -9 } },
    };
    TestReader reader = { 0 };
    XgWorldHorizonCapture capture;
    XgWorldHorizonRecord records[2];
    uint32_t quad;
    uint32_t vertex;

    CHECK(capture_source(&reader, &capture) == XG_WORLD_HORIZON_CAPTURE_OK);
    CHECK(capture.authenticated && capture.sealed);
    CHECK(capture.authenticated_read_count == 29u);
    CHECK(capture.authenticated_read_bytes == 114u);
    CHECK(reader.read_count == 29u);
    CHECK(!reader.touched_output);
    CHECK(capture.buffer_index == 1u);
    CHECK(capture.source.material.tpage == 0x003eu);
    CHECK(capture.source.material.clut_x == 272u);
    CHECK(capture.source.material.clut_y == 510u);
    CHECK(capture.source.material.texture_window_mask_x == 16u);
    CHECK(capture.source.material.blend_mode == XG_RENDER_IR_BLEND_ADD);
    CHECK(!capture.source.material.raw_texture);
    CHECK(xg_world_horizon_build(&capture.source, records) ==
          XG_WORLD_HORIZON_OK);
    for (quad = 0u; quad < 2u; ++quad) {
        CHECK(records[quad].accepted);
        CHECK(records[quad].ordering_bucket == 248u);
        for (vertex = 0u; vertex < 4u; ++vertex) {
            CHECK(records[quad].vertices[vertex].x ==
                  expected_xy[quad][vertex][0]);
            CHECK(records[quad].vertices[vertex].y ==
                  expected_xy[quad][vertex][1]);
        }
        CHECK(records[quad].primitive.triangle_count == 2u);
        CHECK(records[quad].primitive.triangles[0].vertices[0].r == 0x30u);
        CHECK(records[quad].primitive.triangles[0].vertices[0].u == 0);
        CHECK(records[quad].primitive.triangles[0].vertices[1].u ==
              (128 << 16));
        CHECK(records[quad].primitive.triangles[1].vertices[2].v ==
              (63 << 16));
    }
    return 1;
}

static int test_native_view_uses_shared_source_projection(void) {
    static const uint8_t split[2][3] = { { 0u, 1u, 2u },
                                         { 2u, 1u, 3u } };
    TestReader reader = { 0 };
    XgWorldHorizonCapture capture;
    XgWorldHorizonRecord records[2];
    XgNativeView view;
    const int32_t expected_offset = 53 * INT32_C(65536);

    CHECK(capture_source(&reader, &capture) == XG_WORLD_HORIZON_CAPTURE_OK);
    CHECK(xg_native_view_configure(&view, true, 16u, 9u, 320u, 240u));
    CHECK(view.surface_width_16_16 == 427u * UINT32_C(65536));
    CHECK(view.center_offset_x_16_16 == expected_offset);
    CHECK(xg_host_3d_native_view_margin() == 54);
    CHECK(xg_world_horizon_build_for_view(&capture.source, &view, records) ==
          XG_WORLD_HORIZON_OK);
    for (uint32_t quad = 0u; quad < 2u; ++quad) {
        for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
            for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                const XgRenderIrVertex *ir =
                    &records[quad].primitive.triangles[triangle]
                         .vertices[vertex];
                const XgHost3dProjectedVertex *canonical =
                    &records[quad].vertices[split[triangle][vertex]];

                CHECK(ir->native_view_position);
                CHECK(ir->native_view_x - canonical->x_16_16 ==
                      expected_offset);
                CHECK(ir->native_view_y == canonical->y_16_16);
            }
        }
    }
    CHECK(capture.source.projection.screen_offset_x == 160 * INT32_C(65536));
    return 1;
}

static int test_all_or_none_acceptance(void) {
    TestReader reader = { 0 };
    XgWorldHorizonCapture capture;
    XgWorldHorizonRecord records[2];

    CHECK(capture_source(&reader, &capture) == XG_WORLD_HORIZON_CAPTURE_OK);
    capture.source.vertices[0][0].x = INT16_MIN;
    CHECK(xg_world_horizon_build(&capture.source, records) ==
          XG_WORLD_HORIZON_OK);
    CHECK((int32_t)records[0].projection_flags < 0);
    CHECK(records[0].accepted && records[1].accepted);

    CHECK(capture_source(&reader, &capture) == XG_WORLD_HORIZON_CAPTURE_OK);
    capture.source.vertices[1][0].x = INT16_MIN;
    CHECK(xg_world_horizon_build(&capture.source, records) ==
          XG_WORLD_HORIZON_OK);
    CHECK((int32_t)records[1].projection_flags < 0);
    CHECK(!records[0].accepted && !records[1].accepted);
    return 1;
}

static int test_capture_fails_closed(void) {
    TestReader context = { 0 };
    XgWorldHorizonCapture capture;
    XgWorldHorizonCaptureRequest request = {
        .authentication_generation = 7u,
        .caller_return = UINT32_C(0x80071b58),
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 140 << 16,
        .projection_distance = 0x100u,
        .raster = { .draw_area_right = 319u, .draw_area_bottom = 239u },
        .projection_state_authenticated = true,
    };
    XgWorldHorizonAuthenticatedReader reader = {
        .context = &context,
        .read_u16 = test_read_u16,
        .read_u32 = test_read_u32,
        .authentication_generation = 7u,
        .authenticated = true,
    };

    CHECK(xg_world_horizon_source_capture(NULL, &reader, &capture) ==
          XG_WORLD_HORIZON_CAPTURE_INVALID_ARGUMENT);
    reader.authenticated = false;
    CHECK(xg_world_horizon_source_capture(&request, &reader, &capture) ==
          XG_WORLD_HORIZON_CAPTURE_UNAUTHENTICATED);
    reader.authenticated = true;
    request.caller_return += 4u;
    CHECK(xg_world_horizon_source_capture(&request, &reader, &capture) ==
          XG_WORLD_HORIZON_CAPTURE_INVALID_ARGUMENT);
    request.caller_return -= 4u;
    context.fail_address = UINT32_C(0x8009d7cc);
    CHECK(xg_world_horizon_source_capture(&request, &reader, &capture) ==
          XG_WORLD_HORIZON_CAPTURE_READ_FAILED);
    context.fail_address = 0u;
    context.mutate_vertex = true;
    CHECK(xg_world_horizon_source_capture(&request, &reader, &capture) ==
          XG_WORLD_HORIZON_CAPTURE_SOURCE_MISMATCH);
    return 1;
}

int main(void) {
    return test_captured_source_and_projection() &&
           test_native_view_uses_shared_source_projection() &&
           test_all_or_none_acceptance() && test_capture_fails_closed()
        ? 0 : 1;
}
