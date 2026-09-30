#include "xg_render_hud.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static XgRenderSourceFrameDescription description;
static GpuRenderSemantic source, output;
static int anchor;
static uint32_t battle_ui;

static void setup(XgSemanticModuleKind module, uint64_t identity) {
    anchor = module == XG_SEMANTIC_MODULE_BATTLE ? -1 : 1;
    battle_ui = 0u;
    description = (XgRenderSourceFrameDescription){
        .scene = {.module = module, .primary_overlay_identity = identity},
        .display = {.width = 320u, .height = 216u,
            .aspect_num = 16u, .aspect_den = 9u,
            .native_width = 426u, .native_height = 240u, .native_offset_x = 53u},
    };
    source = (GpuRenderSemantic){.topology = GPU_RENDER_SEMANTIC_TRIANGLES,
        .triangle_count = 1u, .submission_command_id = 0x1234u};
    for (unsigned v = 0; v < 3; ++v) {
        source.triangles[0].vertices[v] = (GpuRenderSemanticVertex){
            .x = (260 + (int)v) * 65536, .y = 166 * 65536,
            .u = (int)v * 65536, .r = 128u,
        };
    }
}

static void anchored(uint32_t packet, uint32_t heap) {
    assert(xg_render_hud_anchor(&description, packet + 4u, heap, battle_ui, &source, &output));
    assert(output.submission_command_id == source.submission_command_id);
    assert(memcmp(&output.material, &source.material, sizeof(source.material)) == 0);
    for (unsigned v = 0; v < 3; ++v) {
        const GpuRenderSemanticVertex *a = &source.triangles[0].vertices[v];
        const GpuRenderSemanticVertex *b = &output.triangles[0].vertices[v];
        assert(a->x == b->x && a->y == b->y && a->u == b->u && a->r == b->r);
        assert(b->native_view_x == (a->native_view_position
            ? a->native_view_x + anchor * 53 * 65536
            : a->x + (1 + anchor) * 53 * 65536));
        assert(b->native_view_y == (a->native_view_position ? a->native_view_y : a->y));
        assert(b->native_view_position && !b->projective_position);
    }
}

static void rejected(uint32_t packet, uint32_t heap) {
    memset(&output, 0x5a, sizeof(output));
    GpuRenderSemantic original = output;
    assert(!xg_render_hud_anchor(&description, packet + 4u, heap, battle_ui, &source, &output));
    assert(memcmp(&output, &original, sizeof(output)) == 0);
}

int main(void) {
    setup(XG_SEMANTIC_MODULE_FIELD, UINT64_C(0x4c096f9a82cea138));
    for (uint32_t i = 0u; i < 25u; ++i) {
        anchored(0xb06dcu + i * 0x70u, 0u);
        anchored(0xb0704u + i * 0x70u, 0u);
    }
    rejected(0xb06e0u, 0u); /* payload word, not a packet */
    rejected(0xb06dcu + 25u * 0x70u, 0u);
    for (unsigned v = 0; v < 3; ++v) {
        source.triangles[0].vertices[v].native_view_position = 1u;
        source.triangles[0].vertices[v].native_view_x = (313 + (int)v) * 65536 + 32768;
        source.triangles[0].vertices[v].native_view_y = 166 * 65536 + 32768;
        source.triangles[0].vertices[v].projective_position = 1u;
    }
    anchored(0xb06dcu, 0u); /* retain source subpixel geometry; no double centring */
    GpuRenderSemantic original = source;
    assert(xg_render_hud_anchor(&description, 0xb06e0u, 0u, 0u, &source, &source));
    assert(source.triangles[0].vertices[0].native_view_x ==
           original.triangles[0].vertices[0].native_view_x + 53 * 65536);
    setup(XG_SEMANTIC_MODULE_FIELD, UINT64_C(0x4c096f9a82cea138));
    description.display.aspect_num = 4u; description.display.aspect_den = 3u;
    rejected(0xb06dcu, 0u);
    description.display.aspect_num = 16u; description.display.aspect_den = 9u;
    description.display.native_offset_x = 0u; rejected(0xb06dcu, 0u);
    description.display.native_offset_x = 53u;
    description.scene.primary_overlay_identity = 0u; anchored(0xb06dcu, 0u);
    description.scene.module = XG_SEMANTIC_MODULE_WORLD; rejected(0xb06dcu, 0u);
    description.scene.module = XG_SEMANTIC_MODULE_FIELD;
    source.triangles[0].vertices[2].x = INT32_MAX; rejected(0xb06dcu, 0u);

    setup(XG_SEMANTIC_MODULE_WORLD, UINT64_C(0x7c3da0b332fd154c));
    for (uint32_t i = 0u; i < 2u; ++i) anchored(0x9c5c0u + i * 0x28u, 0u);
    for (uint32_t i = 0u; i < 8u; ++i) anchored(0x9c664u + i * 0x1cu, 0u);
    for (uint32_t i = 0u; i < 64u; ++i) anchored(0x9c898u + i * 0x10u, 0u);
    rejected(0x9c5a0u, 0u); /* texture-page control */
    rejected(0x9c5c0u + 2u * 0x28u, 0u);
    rejected(0x9c898u + 64u * 0x10u, 0u);
    description.scene.module = XG_SEMANTIC_MODULE_FIELD; rejected(0x9c5c0u, 0u);

    setup(XG_SEMANTIC_MODULE_BATTLE, UINT64_C(0x2971e31fefb43018));
    const uint32_t heap = 0x80140000u, base = heap & 0x1fffffu;
    const uint32_t arrays[][3] = {
        {0x6d8u,2u,0x34u}, {0x908u,12u,0x10u}, {0x3768u,20u,0x28u},
        {0x641cu,100u,0x28u}, {0x73bcu,100u,0x28u},
    };
    for (unsigned a = 0; a < sizeof(arrays)/sizeof(arrays[0]); ++a)
        for (uint32_t i = 0u; i < arrays[a][1]; ++i)
            anchored(base + arrays[a][0] + i * arrays[a][2], heap);
    for (uint32_t i = 0u; i < 3u; ++i) {
        for (uint32_t p = 0u; p < 12u; ++p)
            rejected(base + 0x835cu + i * 0x1e4u + p * 0x28u, heap);
        rejected(base + 0x835cu + i * 0x1e4u + 0x1e0u, heap);
    }
    const uint32_t excluded[] = {0u,0x5a0u,0x740u,0x818u,0x9c8u,0xba8u,
        0x2e08u,0x3a88u,0x6008u,0x63c8u,0x8908u};
    for (unsigned i = 0; i < sizeof(excluded)/sizeof(excluded[0]); ++i)
        rejected(base + excluded[i], heap);
    rejected(base + 0x641cu, 0u); rejected(base + 0x641cu, 0x801ffffcu);
    rejected(base + 0x641cu, heap + 2u);
    source = (GpuRenderSemantic){.topology = GPU_RENDER_SEMANTIC_LINES, .line_count = 1u};
    source.lines[0].vertices[0].x = 12 * 65536;
    source.lines[0].vertices[1].x = 18 * 65536;
    assert(xg_render_hud_anchor(&description, base + 0x90cu, heap, 0u, &source, &output));
    assert(output.lines[0].vertices[0].native_view_x == source.lines[0].vertices[0].x);
    assert(output.lines[0].vertices[1].native_view_x == source.lines[0].vertices[1].x);
    assert(memcmp(output.lines[0].vertices, source.lines[0].vertices,
        sizeof(source.lines[0].vertices)) != 0);
    description.display.aspect_num = 4u; description.display.aspect_den = 3u;
    rejected(base + 0x908u, heap);
    description.display.aspect_num = 16u; description.display.aspect_den = 9u;
    description.scene.module = XG_SEMANTIC_MODULE_BATTLING;
    rejected(base + 0x908u, heap);

    setup(XG_SEMANTIC_MODULE_BATTLE, 0u);
    anchor = 1;
    battle_ui = 0x8015059cu;
    const uint32_t ui_base = battle_ui & 0x1fffffu;
    const uint32_t gear_arrays[][2] = {
        {0x1720u,84u}, {0x2580u,28u}, {0x3ac0u,22u}, {0x4ce0u,10u},
        {0x4e70u,8u}, {0x4fb0u,6u}, {0x50a0u,8u}, {0x51e0u,4u},
    };
    for (unsigned a = 0; a < sizeof(gear_arrays)/sizeof(gear_arrays[0]); ++a)
        for (uint32_t i = 0; i < gear_arrays[a][1]; ++i)
            anchored(ui_base + gear_arrays[a][0] + i * 0x28u, 0u);
    /* Shared header banks: Attack Level and the full-screen frame must stay. */
    for (uint32_t i = 84u; i < 90u; ++i)
        rejected(ui_base + 0x1720u + i * 0x28u, heap);
    for (uint32_t i = 0u; i < 86u; ++i)
        if (i < 2u || i >= 30u)
            rejected(ui_base + 0x2530u + i * 0x28u, heap);
    const uint32_t gear_unchanged[] = {0u,0x3c0u,0xd20u,0xfa0u,0x32a0u,
        0x3e30u,0x3e80u,0x43d0u,0x46a0u,0x5280u,0x53c0u,0x5550u,0x5640u,0x5c80u};
    for (unsigned i = 0; i < sizeof(gear_unchanged)/sizeof(gear_unchanged[0]); ++i)
        rejected(ui_base + gear_unchanged[i], heap);
    rejected(ui_base + 0x1724u, heap);
    rejected(base + 0x2e08u, heap); /* party portrait */
    description.display.aspect_num = 4u; description.display.aspect_den = 3u;
    rejected(ui_base + 0x1720u, heap);
    description.display.aspect_num = 16u; description.display.aspect_den = 9u;
    description.scene.module = XG_SEMANTIC_MODULE_FIELD;
    rejected(ui_base + 0x1720u, heap);
    description.scene.module = XG_SEMANTIC_MODULE_BATTLE;
    battle_ui = 0u; rejected(ui_base + 0x1720u, heap);
    battle_ui = 0x801ffffcu; rejected(ui_base + 0x1720u, heap);
    battle_ui = 0x8015059eu; rejected(ui_base + 0x1720u, heap);
    puts("HUD anchors: PASS");
    return 0;
}
