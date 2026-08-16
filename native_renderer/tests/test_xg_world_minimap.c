#include "xg_world_minimap.h"
#include "xg_world_minimap_source_capture.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 0;                                                           \
        }                                                                       \
    } while (0)

typedef struct TestReader {
    uint32_t marker_mask;
    uint32_t buffer_index;
    uint32_t fail_address;
    uint32_t read_count;
    uint16_t marker_x[32];
    uint16_t marker_y[32];
    bool authenticated_output_read;
    bool bad_trig_zero;
    bool bad_vertex;
} TestReader;

static const XgHost3dVector expected_triangles[4][3] = {
    { { 0, 0, 0, 0u }, { -8, -8, 0, 0u }, { -4, -11, 0, 0u } },
    { { 0, 0, 0, 0u }, { -4, -11, 0, 0u }, { 0, -12, 0, 0u } },
    { { 0, 0, 0, 0u }, { 0, -12, 0, 0u }, { 4, -11, 0, 0u } },
    { { 0, 0, 0, 0u }, { 4, -11, 0, 0u }, { 8, -8, 0, 0u } },
};

static bool is_forbidden_source(uint32_t address) {
    return (address >= UINT32_C(0x8009c5a0) &&
            address < UINT32_C(0x8009c610)) ||
           (address >= UINT32_C(0x8009c664) &&
            address < UINT32_C(0x8009c744)) ||
           (address >= UINT32_C(0x8009c898) &&
            address < UINT32_C(0x8009cc98)) ||
           address == UINT32_C(0x8009be3c) ||
           (address >= UINT32_C(0x1f800000) &&
            address < UINT32_C(0x1f800120));
}

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

static bool test_read_u16(void *context, uint32_t address,
                          uint16_t *out_value) {
    TestReader *reader = context;
    uint32_t marker;

    ++reader->read_count;
    reader->authenticated_output_read |= is_forbidden_source(address);
    if (address == reader->fail_address || out_value == NULL) return false;
    if (address == UINT32_C(0x8009bd3a)) {
        *out_value = 0u;
        return true;
    }
    if (address == UINT32_C(0x8006ee60)) marker = 24u;
    else if (address == UINT32_C(0x8006ee64)) {
        *out_value = reader->marker_y[24];
        return true;
    } else if (address == UINT32_C(0x8006ee82)) marker = 25u;
    else if (address == UINT32_C(0x8006ee86)) {
        *out_value = reader->marker_y[25];
        return true;
    } else if (address == UINT32_C(0x8006ee78)) marker = 26u;
    else if (address == UINT32_C(0x8006ee7a)) {
        *out_value = reader->marker_y[26];
        return true;
    } else if (address >= UINT32_C(0x8009b6f4) &&
               address < UINT32_C(0x8009b774)) {
        marker = (address - UINT32_C(0x8009b6f4)) / 4u;
        *out_value = ((address - UINT32_C(0x8009b6f4)) & 2u) != 0u
            ? reader->marker_y[marker]
            : reader->marker_x[marker];
        return true;
    } else {
        return false;
    }
    *out_value = reader->marker_x[marker];
    return true;
}

static bool test_read_u32(void *context, uint32_t address,
                          uint32_t *out_value) {
    TestReader *reader = context;

    ++reader->read_count;
    reader->authenticated_output_read |= is_forbidden_source(address);
    if (address == reader->fail_address || out_value == NULL) return false;
    if (address == UINT32_C(0x800523f0)) {
        *out_value = reader->bad_trig_zero ? 0u : UINT32_C(0x10000000);
        return true;
    }
    if (address >= UINT32_C(0x8009a340) &&
        address < UINT32_C(0x8009a3a0) && (address & 3u) == 0u) {
        const uint32_t offset = address - UINT32_C(0x8009a340);
        const XgHost3dVector *vertex =
            &expected_triangles[offset / 24u][(offset / 8u) % 3u];

        if ((offset & 4u) == 0u)
            *out_value = pack_s16(
                reader->bad_vertex && offset == 0u
                    ? (int16_t)(vertex->x + 1)
                    : vertex->x,
                vertex->y);
        else
            *out_value = pack_s16(vertex->z, (int16_t)vertex->pad);
        return true;
    }
    switch (address) {
    case UINT32_C(0x8009d55c): *out_value = 0u; return true;
    case UINT32_C(0x8009d564): *out_value = 0u; return true;
    case UINT32_C(0x8009bcdc): *out_value = 256u; return true;
    case UINT32_C(0x8009be0c): *out_value = 120u; return true;
    case UINT32_C(0x8009d7f0):
        *out_value = reader->buffer_index;
        return true;
    case UINT32_C(0x8006f160):
        *out_value = reader->marker_mask;
        return true;
    default: return false;
    }
}

static void configure_reader(TestReader *reader) {
    memset(reader, 0, sizeof(*reader));
    reader->buffer_index = 1u;
    reader->marker_mask = UINT32_C(1) | (UINT32_C(1) << 24u) |
        (UINT32_C(1) << 25u) | (UINT32_C(1) << 26u) |
        (UINT32_C(1) << 31u);
    reader->marker_x[0] = 1u;
    reader->marker_y[0] = 2u;
    reader->marker_x[24] = 3150u;
    reader->marker_y[24] = 3410u;
    reader->marker_x[25] = 630u;
    reader->marker_y[25] = 682u;
    reader->marker_x[26] = UINT16_MAX;
    reader->marker_y[26] = UINT16_MAX;
    reader->marker_x[31] = UINT16_MAX;
    reader->marker_y[31] = UINT16_MAX;
}

static XgWorldMinimapCaptureResult capture_source(
    TestReader *context, XgWorldMinimapCapture *capture) {
    const XgWorldMinimapCaptureRequest request = {
        .authentication_generation = 9u,
        .caller_return = UINT32_C(0x80071b84),
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 120 << 16,
        .projection_distance = 256u,
        .raster = {
            .draw_area_right = 319u,
            .draw_area_bottom = 239u,
            .dither = true,
        },
        .projection_state_authenticated = true,
    };
    const XgWorldMinimapAuthenticatedReader reader = {
        .context = context,
        .read_u16 = test_read_u16,
        .read_u32 = test_read_u32,
        .authentication_generation = 9u,
        .authenticated = true,
    };

    return xg_world_minimap_source_capture(&request, &reader, capture);
}

static int test_capture_reads_only_semantic_sources(void) {
    TestReader reader;
    XgWorldMinimapCapture capture;

    configure_reader(&reader);
    CHECK(capture_source(&reader, &capture) == XG_WORLD_MINIMAP_CAPTURE_OK);
    CHECK(capture.authenticated && capture.sealed);
    CHECK(capture.active_marker_count == 5u);
    CHECK(capture.authenticated_read_count == 43u);
    CHECK(capture.authenticated_read_bytes == 150u);
    CHECK(reader.read_count == 43u);
    CHECK(!reader.authenticated_output_read);
    CHECK(capture.source.sine == 0);
    CHECK(capture.source.cosine == 4096);
    CHECK(capture.source.markers[24].raw_x == 3150u);
    CHECK(capture.source.markers[31].raw_y == UINT16_MAX);
    return 1;
}

static int test_builds_gte_derived_triangles(void) {
    static const int16_t expected_xy[4][3][2] = {
        { { 208, 120 }, { 200, 112 }, { 204, 109 } },
        { { 208, 120 }, { 204, 109 }, { 208, 108 } },
        { { 208, 120 }, { 208, 108 }, { 212, 109 } },
        { { 208, 120 }, { 212, 109 }, { 216, 112 } },
    };
    TestReader reader;
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput output;
    uint32_t triangle;
    uint32_t vertex;

    configure_reader(&reader);
    CHECK(capture_source(&reader, &capture) == XG_WORLD_MINIMAP_CAPTURE_OK);
    CHECK(xg_world_minimap_build(&capture.source, &output) ==
          XG_WORLD_MINIMAP_OK);
    CHECK(output.projection.rotation[0][0] == 4096);
    CHECK(output.projection.rotation[1][1] == 4096);
    CHECK(output.projection.translation[0] == 48);
    CHECK(output.projection.translation[1] == 0);
    CHECK(output.projection.translation[2] == 256);
    CHECK(output.scratch.angle_address == UINT32_C(0x1f8000b8));
    CHECK(output.scratch.rotation_address == UINT32_C(0x1f8000f0));
    CHECK(output.scratch.translation_address == UINT32_C(0x1f800104));
    for (triangle = 0u; triangle < 4u; ++triangle) {
        CHECK(output.triangles[triangle].submitted);
        CHECK(output.triangles[triangle].packet_address ==
              UINT32_C(0x8009c6d4) + triangle * 0x1cu);
        CHECK(output.triangles[triangle].primitive.triangle_count == 1u);
        CHECK(output.triangles[triangle].primitive.material.shading ==
              XG_RENDER_IR_SHADING_GOURAUD);
        CHECK(output.triangles[triangle].primitive.material.blend_mode ==
              XG_RENDER_IR_BLEND_ADD);
        CHECK(!output.triangles[triangle].primitive.material.dither);
        for (vertex = 0u; vertex < 3u; ++vertex) {
            CHECK(output.triangles[triangle].screen_xy[vertex][0] ==
                  expected_xy[triangle][vertex][0]);
            CHECK(output.triangles[triangle].screen_xy[vertex][1] ==
                  expected_xy[triangle][vertex][1]);
            CHECK(output.triangles[triangle].screen_xy_address[vertex] ==
                  output.triangles[triangle].packet_address + 8u +
                      vertex * 8u);
        }
        CHECK(output.triangles[triangle]
                  .primitive.triangles[0].vertices[0].r == 0xffu);
        CHECK(output.triangles[triangle]
                  .primitive.triangles[0].vertices[1].r == 0u);
    }
    return 1;
}

static int test_builds_rotated_signed_transform(void) {
    TestReader reader;
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput output;

    configure_reader(&reader);
    CHECK(capture_source(&reader, &capture) == XG_WORLD_MINIMAP_CAPTURE_OK);
    capture.source.angle = 1024u;
    capture.source.sine = 4096;
    capture.source.cosine = 0;
    capture.source.world_x_20_12 = -315 * 4096;
    capture.source.world_z_20_12 = -341 * 4096;
    CHECK(xg_world_minimap_build(&capture.source, &output) ==
          XG_WORLD_MINIMAP_OK);
    CHECK(output.projection.rotation[0][0] == 0);
    CHECK(output.projection.rotation[0][1] == -4096);
    CHECK(output.projection.rotation[1][0] == 4096);
    CHECK(output.projection.rotation[1][1] == 0);
    CHECK(output.projection.translation[0] == 47);
    CHECK(output.projection.translation[1] == -1);
    CHECK(output.scratch.angle_z == 1024u);
    CHECK(output.scratch.rotation[0][1] == -4096);
    CHECK(output.scratch.translation[0] == 47);
    return 1;
}

static int test_builds_conditional_2d_markers(void) {
    TestReader reader;
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput output;

    configure_reader(&reader);
    CHECK(capture_source(&reader, &capture) == XG_WORLD_MINIMAP_CAPTURE_OK);
    CHECK(xg_world_minimap_build(&capture.source, &output) ==
          XG_WORLD_MINIMAP_OK);
    CHECK(output.active_marker_count == 5u);
    CHECK(output.markers[0].active);
    CHECK(output.markers[0].x == 209 && output.markers[0].y == 122);
    CHECK(output.markers[24].x == 217 && output.markers[24].y == 129);
    CHECK(output.markers[25].x == 209 && output.markers[25].y == 121);
    CHECK(output.markers[26].x == 415 && output.markers[26].y == 311);
    CHECK(output.markers[31].x == 207 && output.markers[31].y == 119);
    CHECK(!output.markers[1].active);
    CHECK(output.markers[24].coordinate_rule ==
          XG_WORLD_MINIMAP_MARKER_RESIDENT_SCALED);
    CHECK(output.markers[0].coordinate_rule ==
          XG_WORLD_MINIMAP_MARKER_OFFSET);
    CHECK(output.markers[24].packet_address == UINT32_C(0x8009cc18));
    CHECK(output.markers[24].screen_xy_address == UINT32_C(0x8009cc20));
    CHECK(output.markers[24].primitive.triangle_count == 2u);
    CHECK(!output.markers[24].primitive.triangles[0].vertices[0]
               .projective_position);
    CHECK(output.markers[24].primitive.triangles[0].vertices[0].r == 0x80u);
    CHECK(output.markers[24].primitive.triangles[0].vertices[0].b == 0x10u);
    CHECK(output.markers[24].primitive.triangles[0].vertices[1].x ==
          219 * 65536);
    CHECK(output.markers[24].width == 2u && output.markers[24].height == 2u);
    return 1;
}

static int test_builds_fixed_2d_panel(void) {
    TestReader reader;
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput output;

    configure_reader(&reader);
    CHECK(capture_source(&reader, &capture) == XG_WORLD_MINIMAP_CAPTURE_OK);
    CHECK(xg_world_minimap_build(&capture.source, &output) ==
          XG_WORLD_MINIMAP_OK);
    CHECK(output.panel.packet_address == UINT32_C(0x8009c5e8));
    CHECK(output.panel.screen_xy[0][0] == 208);
    CHECK(output.panel.screen_xy[3][1] == 215);
    CHECK(output.panel.uv[0][1] == 128u);
    CHECK(output.panel.uv[3][0] == 127u);
    CHECK(output.panel.primitive.triangle_count == 2u);
    CHECK(output.panel.primitive.material.tpage == 0x001eu);
    CHECK(output.panel.primitive.material.clut_x == 256u);
    CHECK(output.panel.primitive.material.clut_y == 510u);
    CHECK(output.panel.primitive.material.textured);
    CHECK(output.panel.primitive.material.semi_transparent);
    CHECK(output.panel.primitive.triangles[1].vertices[2].u == 127 * 65536);
    CHECK(output.panel.primitive.triangles[1].vertices[2].v == 255 * 65536);
    return 1;
}

static int test_preserves_exact_mixed_prepend_order(void) {
    static const XgWorldMinimapOrderKind expected_kind[11] = {
        XG_WORLD_MINIMAP_ORDER_TRIANGLE, XG_WORLD_MINIMAP_ORDER_TRIANGLE,
        XG_WORLD_MINIMAP_ORDER_TRIANGLE, XG_WORLD_MINIMAP_ORDER_TRIANGLE,
        XG_WORLD_MINIMAP_ORDER_DRAW_MODE, XG_WORLD_MINIMAP_ORDER_MARKER,
        XG_WORLD_MINIMAP_ORDER_MARKER, XG_WORLD_MINIMAP_ORDER_MARKER,
        XG_WORLD_MINIMAP_ORDER_MARKER, XG_WORLD_MINIMAP_ORDER_MARKER,
        XG_WORLD_MINIMAP_ORDER_PANEL,
    };
    static const uint8_t expected_source[11] = {
        0u, 1u, 2u, 3u, UINT8_MAX, 0u, 24u, 25u, 26u, 31u, UINT8_MAX,
    };
    TestReader reader;
    XgWorldMinimapCapture capture;
    XgWorldMinimapBuildOutput output;
    uint32_t event;

    configure_reader(&reader);
    CHECK(capture_source(&reader, &capture) == XG_WORLD_MINIMAP_CAPTURE_OK);
    CHECK(xg_world_minimap_build(&capture.source, &output) ==
          XG_WORLD_MINIMAP_OK);
    CHECK(output.ordering_count == 11u);
    CHECK(output.requires_external_ot_tail);
    CHECK(output.draw_mode.packet_address == UINT32_C(0x8009c5a0));
    CHECK(output.draw_mode.command_word == UINT32_C(0xe100043e));
    for (event = 0u; event < output.ordering_count; ++event) {
        CHECK(output.ordering[event].kind == expected_kind[event]);
        CHECK(output.ordering[event].source_index == expected_source[event]);
        CHECK(output.ordering[event].insertion_ordinal == event);
        CHECK(output.ordering[event].final_chain_ordinal == 10u - event);
        CHECK(output.ordering[event].successor_event_index ==
              (event == 0u ? XG_WORLD_MINIMAP_NO_ORDER_EVENT : event - 1u));
    }
    CHECK(output.ordering[4].payload_word_count == 1u);
    CHECK(output.ordering[5].payload_word_count == 3u);
    CHECK(output.ordering[10].payload_word_count == 9u);
    return 1;
}

static int test_capture_and_builder_fail_closed(void) {
    TestReader context;
    XgWorldMinimapCapture capture;
    XgWorldMinimapCaptureRequest request = {
        .authentication_generation = 9u,
        .caller_return = UINT32_C(0x80071b84),
        .projection_distance = 256u,
        .raster = { .draw_area_right = 319u, .draw_area_bottom = 239u },
        .projection_state_authenticated = true,
    };
    XgWorldMinimapAuthenticatedReader reader = {
        .context = &context,
        .read_u16 = test_read_u16,
        .read_u32 = test_read_u32,
        .authentication_generation = 9u,
        .authenticated = true,
    };
    XgWorldMinimapBuildOutput output;

    configure_reader(&context);
    request.caller_return -= 4u;
    CHECK(xg_world_minimap_source_capture(&request, &reader, &capture) ==
          XG_WORLD_MINIMAP_CAPTURE_INVALID_ARGUMENT);
    request.caller_return += 4u;
    reader.authenticated = false;
    CHECK(xg_world_minimap_source_capture(&request, &reader, &capture) ==
          XG_WORLD_MINIMAP_CAPTURE_UNAUTHENTICATED);
    reader.authenticated = true;
    context.bad_vertex = true;
    CHECK(xg_world_minimap_source_capture(&request, &reader, &capture) ==
          XG_WORLD_MINIMAP_CAPTURE_SOURCE_MISMATCH);
    context.bad_vertex = false;
    context.bad_trig_zero = true;
    CHECK(xg_world_minimap_source_capture(&request, &reader, &capture) ==
          XG_WORLD_MINIMAP_CAPTURE_SOURCE_MISMATCH);
    context.bad_trig_zero = false;
    context.buffer_index = 2u;
    CHECK(xg_world_minimap_source_capture(&request, &reader, &capture) ==
          XG_WORLD_MINIMAP_CAPTURE_SOURCE_MISMATCH);
    context.buffer_index = 1u;
    context.fail_address = UINT32_C(0x8009d55c);
    CHECK(xg_world_minimap_source_capture(&request, &reader, &capture) ==
          XG_WORLD_MINIMAP_CAPTURE_READ_FAILED);
    context.fail_address = 0u;
    CHECK(xg_world_minimap_source_capture(&request, &reader, &capture) ==
          XG_WORLD_MINIMAP_CAPTURE_OK);
    capture.source.buffer_index = 2u;
    CHECK(xg_world_minimap_build(&capture.source, &output) ==
          XG_WORLD_MINIMAP_INVALID_SOURCE);
    return 1;
}

static int test_all_active_capture_fits_authenticated_budget(void) {
    TestReader reader;
    XgWorldMinimapCapture capture;

    configure_reader(&reader);
    reader.marker_mask = UINT32_MAX;
    CHECK(capture_source(&reader, &capture) == XG_WORLD_MINIMAP_CAPTURE_OK);
    CHECK(capture.active_marker_count == 32u);
    CHECK(capture.authenticated_read_count ==
          XG_WORLD_MINIMAP_MAX_AUTHENTICATED_READS);
    CHECK(capture.authenticated_read_bytes == 258u);
    CHECK(!reader.authenticated_output_read);
    return 1;
}

int main(void) {
    return test_capture_reads_only_semantic_sources() &&
            test_builds_gte_derived_triangles() &&
            test_builds_rotated_signed_transform() &&
            test_builds_conditional_2d_markers() &&
            test_builds_fixed_2d_panel() &&
            test_preserves_exact_mixed_prepend_order() &&
            test_capture_and_builder_fail_closed() &&
            test_all_active_capture_fits_authenticated_budget()
        ? 0
        : 1;
}
