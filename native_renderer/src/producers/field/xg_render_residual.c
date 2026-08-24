#include "xg_render_residual.h"

#include "cpu_state.h"
#include "gpu.h"
#include "xg_field_render_services.h"
#include "xg_render_address_lookup.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_quad_builder.h"
#include "xg_render_resource_watch.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    TEMPLATE_CAPACITY = 1024u,
    MAX_RESOURCE_SIZE = 0x24u,
};

typedef struct XgRenderResidualTemplate {
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t command_address;
    uint32_t producer_seam;
    uint32_t resource_size;
    bool semantic_ready;
    bool valid;
} XgRenderResidualTemplate;

static XgRenderResidualTemplate templates[TEMPLATE_CAPACITY];
static XgRenderAddressLookupSlot lookup[XG_RENDER_LOOKUP_WORD_CAPACITY];
static uint32_t template_count;
static uint16_t lookup_epoch = 1u;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool ranges_overlap(
        uint32_t left_start, uint32_t left_size,
        uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & UINT32_C(0x1fffffff);
    const uint64_t right_begin = right_start & UINT32_C(0x1fffffff);

    return left_size != 0u && right_size != 0u &&
        left_begin < right_begin + right_size &&
        right_begin < left_begin + left_size;
}

static XgRenderResidualTemplate *find_template(uint32_t command_address) {
    const uint32_t indexed = xg_render_lookup_find(
        lookup, lookup_epoch, command_address, template_count);

    if (indexed != UINT32_MAX && templates[indexed].valid &&
        physical_address_equals(
            templates[indexed].command_address, command_address))
        return &templates[indexed];
    for (uint32_t index = 0u; index < template_count; ++index) {
        XgRenderResidualTemplate *record = &templates[index];

        if (record->valid && physical_address_equals(
                record->command_address, command_address)) {
            xg_render_lookup_put(
                lookup, lookup_epoch, command_address, index);
            return record;
        }
    }
    return NULL;
}

static XgRenderResidualTemplate *find_indexed(uint32_t command_address) {
    const uint32_t indexed = xg_render_lookup_find(
        lookup, lookup_epoch, command_address, template_count);

    if (indexed == UINT32_MAX || !templates[indexed].valid ||
        !physical_address_equals(
            templates[indexed].command_address, command_address))
        return NULL;
    return &templates[indexed];
}

static bool store_template(
        uint32_t command_address, uint32_t producer_seam,
        uint32_t resource_size, const XgRenderQuadSource *source,
        const XgRenderProducerLifecycleServices *services) {
    XgRenderResidualTemplate *record;
    XgRenderProducerLifecycle lifecycle;

    if (source == NULL || services == NULL || services->begin == NULL ||
        resource_size == 0u || resource_size > MAX_RESOURCE_SIZE ||
        !services->begin(producer_seam, &lifecycle))
        return false;
    record = find_template(command_address);
    if (record == NULL) {
        if (template_count == TEMPLATE_CAPACITY) return false;
        record = &templates[template_count++];
    }
    memset(record, 0, sizeof(*record));
    if (xg_render_quad_build_primitive(source, &record->primitive) !=
            XG_RENDER_QUAD_BUILDER_OK)
        return false;
    record->command_address = command_address & UINT32_C(0x1ffffffc);
    record->producer_seam = producer_seam;
    record->resource_size = resource_size;
    record->lifecycle = lifecycle;
    record->valid = true;
    xg_render_lookup_put(
        lookup, lookup_epoch, record->command_address,
        (uint32_t)(record - templates));
    xg_render_resource_watch_add(record->command_address, resource_size);
    return true;
}

static void source_material(
        XgRenderIrMaterialState *material, XgRenderIrShading shading,
        bool semi_transparent) {
    GpuDrawState draw = {0};

    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(material, &draw);
    material->shading = shading;
    material->textured = false;
    material->raw_texture = false;
    material->semi_transparent = semi_transparent;
}

static bool capture_tile(
        uint32_t command_address, uint32_t producer_seam,
        int16_t x, int16_t y, int16_t width, int16_t height,
        uint8_t red, uint8_t green, uint8_t blue, bool semi_transparent,
        const XgRenderProducerLifecycleServices *services) {
    XgRenderQuadSource source = {0};

    source_material(
        &source.material, XG_RENDER_IR_SHADING_FLAT, semi_transparent);
    source.vertices[0] = (XgRenderQuadSourceVertex){
        x, y, 0, 0, red, green, blue};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        x + width, y, 0, 0, red, green, blue};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        x, y + height, 0, 0, red, green, blue};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        x + width, y + height, 0, 0, red, green, blue};
    return store_template(
        command_address, producer_seam, 0x0cu, &source, services);
}

void xg_render_residual_reset(void) {
    template_count = 0u;
    xg_render_lookup_reset(lookup, &lookup_epoch);
}

void xg_render_residual_retain_resident(void) {
    uint32_t retained_count = 0u;

    for (uint32_t index = 0u; index < template_count; ++index) {
        const XgRenderResidualTemplate *record = &templates[index];

        if (!record->valid || record->lifecycle.scene_resource != 0u) continue;
        if (retained_count != index) templates[retained_count] = *record;
        ++retained_count;
    }
    template_count = retained_count;
    xg_render_lookup_reset(lookup, &lookup_epoch);
    for (uint32_t index = 0u; index < template_count; ++index)
        xg_render_lookup_put(
            lookup, lookup_epoch, templates[index].command_address, index);
}

void xg_render_residual_invalidate(uint32_t address, uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);
    uint32_t first_candidate;
    uint32_t end;

    if (size == 0u || physical >= UINT32_C(0x200000)) return;
    end = size > UINT32_C(0x200000) - physical
        ? UINT32_C(0x200000) : physical + size;
    first_candidate = physical >= MAX_RESOURCE_SIZE - 1u
        ? physical - (MAX_RESOURCE_SIZE - 1u) : 0u;
    first_candidate &= UINT32_C(0x1ffffc);
    for (uint32_t candidate = first_candidate;
         candidate <= ((end - 1u) & UINT32_C(0x1ffffc)); candidate += 4u) {
        const uint32_t indexed = xg_render_lookup_find(
            lookup, lookup_epoch, candidate, template_count);
        XgRenderResidualTemplate *record;

        if (indexed == UINT32_MAX) continue;
        record = &templates[indexed];
        if (!record->valid || !physical_address_equals(
                record->command_address, candidate) ||
            !ranges_overlap(
                record->command_address, record->resource_size, address, size))
            continue;
        record->valid = false;
        xg_render_lookup_remove(
            lookup, lookup_epoch, record->command_address, indexed);
    }
}

void xg_render_residual_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.resource_mutation)
            xg_render_residual_invalidate(event->address, event->size);
        if (event->mutation.semantic_authority_loss)
            xg_render_residual_retain_resident();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
               event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_residual_reset();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
               event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
               event->kind == XG_RENDER_INVALIDATION_RESOURCE_OVERLAP) {
        xg_render_residual_retain_resident();
    }
}

static void capture_clear_tile(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    uint32_t rect;
    uint32_t xy;
    uint32_t wh;
    uint32_t color;

    if (cpu == NULL || cpu->read_word == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    rect = cpu->gpr[8];
    xy = cpu->read_word(rect);
    wh = cpu->read_word(rect + 4u);
    if (((xy & 0x3fu) == 0u) && ((wh & 0x3fu) == 0u)) return;
    color = cpu->gpr[9];
    (void)capture_tile(
        UINT32_C(0x8005a250), UINT32_C(0x80045ed0),
        (int16_t)xy, (int16_t)(xy >> 16u),
        (int16_t)wh, (int16_t)(wh >> 16u),
        (uint8_t)color, (uint8_t)(color >> 8u),
        (uint8_t)(color >> 16u), false, services);
}

static void capture_logo_sprite(
        uint32_t command_address, uint8_t color,
        GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    XgRenderQuadSource source = {0};
    XgRenderResidualTemplate *record;

    if (render_mode != GUEST_RENDER_RENDER_NATIVE) return;
    source_material(&source.material, XG_RENDER_IR_SHADING_FLAT, false);
    source.material.tpage = 10u;
    source.material.texture_page_x = 10u;
    source.material.texture_page_y = 0u;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_4_BIT;
    source.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    source.material.clut_x = 0u;
    source.material.clut_y = 240u;
    source.material.textured = true;
    source.vertices[0] = (XgRenderQuadSourceVertex){
        32, 88, 0, 0, color, color, color};
    source.vertices[1] = (XgRenderQuadSourceVertex){
        288, 88, 0, 0, color, color, color};
    source.vertices[2] = (XgRenderQuadSourceVertex){
        32, 136, 0, 48, color, color, color};
    source.vertices[3] = (XgRenderQuadSourceVertex){
        288, 136, 0, 48, color, color, color};
    if (!store_template(
            command_address, UINT32_C(0x80019d48), 0x10u, &source, services))
        return;
    record = find_template(command_address);
    if (record != NULL) {
        record->primitive.triangles[0].vertices[1].u = 256 * INT32_C(65536);
        record->primitive.triangles[1].vertices[1].u = 256 * INT32_C(65536);
        record->primitive.triangles[1].vertices[2].u = 256 * INT32_C(65536);
    }
}

static void capture_fullscreen_tile(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    uint32_t buffer;
    uint8_t color;

    if (cpu == NULL || cpu->read_word == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    buffer = cpu->read_word(UINT32_C(0x800adb08));
    if (buffer > 1u) return;
    color = (uint8_t)(cpu->gpr[4] << 2u);
    (void)capture_tile(
        UINT32_C(0x800afe58) + buffer * 0x10u,
        UINT32_C(0x80079784), 0, 0, 320, 224,
        color, color, color, true, services);
}

static void capture_fade_tiles(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    uint32_t buffer;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    buffer = cpu->gpr[5];
    if (buffer > 1u) return;
    for (uint32_t tile = 0u; tile < 2u; ++tile) {
        const uint32_t stride = tile * 0x58u;

        if (cpu->read_half(UINT32_C(0x800b2116) + stride) == 0u) continue;
        (void)capture_tile(
            UINT32_C(0x800b20e0) + stride + buffer * 0x10u,
            UINT32_C(0x8007da44), 0, 0, 320, 224,
            (uint8_t)(cpu->read_word(UINT32_C(0x800b20fc) + stride) >> 8u),
            (uint8_t)(cpu->read_word(UINT32_C(0x800b2100) + stride) >> 8u),
            (uint8_t)(cpu->read_word(UINT32_C(0x800b2104) + stride) >> 8u),
            true, services);
    }
}

static void capture_tile_write(
        CPUState *cpu, uint32_t command_address, uint32_t writer_pc,
        uint8_t color, GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    if (render_mode != GUEST_RENDER_RENDER_NATIVE) return;
    if (writer_pc == UINT32_C(0x800797d0)) {
        (void)capture_tile(
            command_address, UINT32_C(0x80079784), 0, 0, 320, 224,
            color, color, color, true, services);
        return;
    }
    if (writer_pc == UINT32_C(0x8007dbb4) && cpu != NULL &&
        cpu->read_word != NULL && command_address >= UINT32_C(0x000b20e0)) {
        const uint32_t relative = command_address - UINT32_C(0x000b20e0);
        const uint32_t tile = relative / 0x58u;
        const uint32_t stride = tile * 0x58u;

        if (tile >= 2u) return;
        (void)capture_tile(
            command_address, UINT32_C(0x8007da44), 0, 0, 320, 224,
            (uint8_t)(cpu->read_word(UINT32_C(0x800b20fc) + stride) >> 8u),
            (uint8_t)(cpu->read_word(UINT32_C(0x800b2100) + stride) >> 8u),
            color, true, services);
    }
}

static void capture_static_gouraud(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    uint32_t global;
    uint32_t packet_base;

    if (cpu == NULL || cpu->read_word == NULL || services == NULL ||
        services->guest_data_range_is_valid == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    global = cpu->read_word(UINT32_C(0x800625a0));
    if (!services->guest_data_range_is_valid(global, 0x350u, 4u, false))
        return;
    packet_base = cpu->read_word(global + 0x34cu);
    for (uint32_t quad = 0u; quad < 2u; ++quad) {
        XgRenderQuadSource source = {0};

        source_material(&source.material, XG_RENDER_IR_SHADING_GOURAUD, false);
        source.vertices[0] = (XgRenderQuadSourceVertex){
            0, 0x4a, 0, 0, 0x80, 0x00, 0x80};
        source.vertices[1] = (XgRenderQuadSourceVertex){
            320, 0x4a, 0, 0, 0x00, 0x00, 0x80};
        source.vertices[2] = (XgRenderQuadSourceVertex){
            0, 0x8a, 0, 0, 0x10, 0x00, 0x10};
        source.vertices[3] = (XgRenderQuadSourceVertex){
            320, 0x8a, 0, 0, 0x00, 0x00, 0x10};
        (void)store_template(
            packet_base + 0xa54u + quad * 0x24u,
            UINT32_C(0x801e61a4), 0x24u, &source, services);
    }
}

static void capture_projected_gouraud(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    XgHost3dProject4Input input = {0};
    XgHost3dRotTransPers4Output output;
    XgRenderQuadSource source = {0};
    uint32_t first_xy;

    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return;
    xg_render_runtime_capture_shadow_projection(cpu, &input.projection);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = cpu->gpr[4u + vertex];

        input.vertices[vertex] = (XgHost3dVector){
            (int16_t)cpu->read_half(address),
            (int16_t)cpu->read_half(address + 2u),
            (int16_t)cpu->read_half(address + 4u),
            cpu->read_half(address + 6u),
        };
    }
    if (!xg_host_3d_rot_trans_pers4(&input, &output)) return;
    source_material(&source.material, XG_RENDER_IR_SHADING_GOURAUD, true);
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        source.vertices[vertex] = (XgRenderQuadSourceVertex){
            .red = 0x68u,
            .green = 0x68u,
            .blue = 0x68u,
        };
        xg_render_quad_set_projected_position(
            &source.vertices[vertex], &output.vertices[vertex]);
    }
    first_xy = cpu->read_word(cpu->gpr[29] + 0x10u);
    if (first_xy < 4u) return;
    (void)store_template(
        first_xy - 4u, UINT32_C(0x801d09b0), 0x24u, &source, services);
}

void xg_render_residual_capture(
        const XgRenderResidualCaptureRequest *request,
        GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    if (request == NULL) return;
    switch (request->kind) {
    case XG_RENDER_RESIDUAL_CAPTURE_CLEAR_TILE:
        capture_clear_tile(request->cpu, render_mode, services);
        break;
    case XG_RENDER_RESIDUAL_CAPTURE_LOGO_SPRITE:
        capture_logo_sprite(
            request->command_address, request->color, render_mode, services);
        break;
    case XG_RENDER_RESIDUAL_CAPTURE_FULLSCREEN_TILE:
        capture_fullscreen_tile(request->cpu, render_mode, services);
        break;
    case XG_RENDER_RESIDUAL_CAPTURE_FADE_TILES:
        capture_fade_tiles(request->cpu, render_mode, services);
        break;
    case XG_RENDER_RESIDUAL_CAPTURE_TILE_WRITE:
        capture_tile_write(
            request->cpu, request->command_address, request->writer_pc,
            request->color, render_mode, services);
        break;
    case XG_RENDER_RESIDUAL_CAPTURE_STATIC_GOURAUD:
        capture_static_gouraud(request->cpu, render_mode, services);
        break;
    case XG_RENDER_RESIDUAL_CAPTURE_PROJECTED_GOURAUD:
        capture_projected_gouraud(request->cpu, render_mode, services);
        break;
    }
}

bool xg_render_residual_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
        const XgRenderProducerLifecycleServices *services) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderResidualTemplate *record;
    uint32_t lookup_key;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        services == NULL || services->matches_replay == NULL ||
        services->replay_container_matches_command == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    record = xg_render_lookup_key((uint32_t)command_id, &lookup_key)
        ? find_indexed((uint32_t)command_id)
        : find_template((uint32_t)command_id);
    return record != NULL && record->valid &&
        services->replay_container_matches_command(context) &&
        services->matches_replay(&record->lifecycle, context) &&
        xg_render_primitive_translate_cached(
            &record->primitive, &record->semantic, &record->semantic_ready,
            out_semantic);
}
