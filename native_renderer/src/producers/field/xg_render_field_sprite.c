#include "xg_render_field_sprite.h"

#include "xg_render_overlay_ft4.h"

#include "gpu.h"
#include "xg_render_address_lookup.h"
#include "xg_render_primitive_utils.h"
#include "xg_render_quad_builder.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    BUILDER_CAPACITY = 256u,
    TEMPLATE_CAPACITY = 1024u,
    PRODUCER_PC = UINT32_C(0x8002675c),
};

typedef struct XgRenderFieldSpriteRecord {
    XgRenderIrNativePrimitive primitive;
    GpuRenderSemantic semantic;
    XgRenderProducerLifecycle lifecycle;
    uint32_t packet_address;
    uint32_t descriptor_address;
    uint32_t interpolation_producer_id;
    uint32_t interpolation_primitive_id;
    uint32_t xy[4];
    uint16_t uv[4];
    uint16_t tpage;
    uint16_t clut;
    bool interpolation_identity_valid;
    bool semantic_ready;
} XgRenderFieldSpriteRecord;

typedef struct XgRenderFieldSpriteBuilder {
    XgRenderFieldSpriteRecord records[BUILDER_CAPACITY];
    uint32_t count;
    uint8_t overlay_family;
} XgRenderFieldSpriteBuilder;

static XgRenderFieldSpriteBuilder builder;
static XgRenderFieldSpriteRecord templates[TEMPLATE_CAPACITY];
static uint32_t template_count;
static XgRenderAddressLookupSlot template_lookup[XG_RENDER_LOOKUP_WORD_CAPACITY];
static uint16_t template_lookup_epoch = 1u;
static PsxXgRenderSpriteFt4ShadowSnapshot field_sprite_diagnostics;
static struct {
    XgRenderFieldSpriteRecord source;
    bool active;
} xy_override;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool normalized_ranges_overlap(
        uint32_t left_start, uint32_t left_size,
        uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & UINT32_C(0x1fffffff);
    const uint64_t left_end = left_begin + left_size;
    const uint64_t right_begin = right_start & UINT32_C(0x1fffffff);
    const uint64_t right_end = right_begin + right_size;

    return left_size != 0u && right_size != 0u &&
        left_begin < right_end && right_begin < left_end;
}

static PsxXgRenderSpriteFt4ShadowSnapshot *snapshot(
        const XgRenderFieldSpriteServices *services) {
    (void)services;
    return &field_sprite_diagnostics;
}

void xg_render_field_sprite_clear_builder(
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);

    if (builder.count == 0u && builder.overlay_family == 0u &&
        (telemetry == NULL || !telemetry->field_builder_pending))
        return;
    builder.count = 0u;
    builder.overlay_family = 0u;
    if (telemetry != NULL) telemetry->field_builder_pending = false;
}

void xg_render_field_sprite_clear_templates(
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);

    xy_override.active = false;
    template_count = 0u;
    xg_render_lookup_reset(template_lookup, &template_lookup_epoch);
    if (telemetry != NULL) telemetry->field_builder_template_count = 0u;
}

static void block_builder(
        uint32_t blocker, const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);

    xg_render_field_sprite_clear_builder(services);
    if (telemetry == NULL) return;
    telemetry->field_builder_blocked = true;
    if (telemetry->field_builder_blocker == 0u)
        telemetry->field_builder_blocker = blocker;
}

static XgRenderFieldSpriteRecord *find_template(uint32_t packet_address) {
    const uint32_t indexed = xg_render_lookup_find(
        template_lookup, template_lookup_epoch, packet_address, template_count);

    if (indexed != UINT32_MAX && physical_address_equals(
            templates[indexed].packet_address, packet_address))
        return &templates[indexed];
    for (uint32_t index = 0u; index < template_count; ++index) {
        if (!physical_address_equals(
                templates[index].packet_address, packet_address))
            continue;
        xg_render_lookup_put(
            template_lookup, template_lookup_epoch, packet_address, index);
        return &templates[index];
    }
    return NULL;
}

static bool capture_template(
        const XgRenderFieldSpriteRecord *record,
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);
    XgRenderFieldSpriteRecord *target;

    if (record == NULL) return false;
    target = find_template(record->packet_address);
    if (target != NULL) {
        *target = *record;
        target->semantic_ready = false;
        if (telemetry != NULL)
            ++telemetry->field_builder_template_update_count;
    } else {
        if (template_count == TEMPLATE_CAPACITY) return false;
        templates[template_count] = *record;
        templates[template_count].semantic_ready = false;
        xg_render_lookup_put(
            template_lookup, template_lookup_epoch,
            record->packet_address, template_count);
        ++template_count;
        if (telemetry != NULL)
            telemetry->field_builder_template_count = template_count;
    }
    if (telemetry != NULL)
        ++telemetry->field_builder_template_capture_count;
    if (services != NULL && services->register_replay_command != NULL)
        services->register_replay_command(
            (record->packet_address & UINT32_C(0x1fffffff)) + 4u);
    if (services != NULL && services->watch_resource != NULL) {
        services->watch_resource(record->packet_address + 4u, 0x24u);
        if (record->descriptor_address != 0u)
            services->watch_resource(record->descriptor_address, 0x1cu);
    }
    return true;
}

bool xg_render_field_sprite_has_template(uint32_t packet_address) {
    return find_template(packet_address) != NULL;
}

uint32_t xg_render_field_sprite_available_template_capacity(void) {
    return TEMPLATE_CAPACITY - template_count;
}

bool xg_render_field_sprite_capture_template(
        const XgRenderFieldSpriteTemplateInput *input,
        const XgRenderFieldSpriteServices *services) {
    XgRenderFieldSpriteRecord record = {0};

    if (input == NULL) return false;
    record.primitive = input->primitive;
    record.lifecycle = input->lifecycle;
    record.packet_address = input->packet_address;
    record.interpolation_producer_id = input->interpolation_producer_id;
    record.interpolation_primitive_id = input->interpolation_primitive_id;
    record.interpolation_identity_valid = input->interpolation_identity_valid;
    record.tpage = input->tpage;
    record.clut = input->clut;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        record.xy[vertex] = input->xy[vertex];
        record.uv[vertex] = input->uv[vertex];
    }
    return capture_template(&record, services);
}

void xg_render_field_sprite_invalidate_template(
        uint32_t packet_address,
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);
    XgRenderFieldSpriteRecord *record = find_template(packet_address);
    uint32_t index;

    if (record == NULL) return;
    index = (uint32_t)(record - templates);
    --template_count;
    xg_render_lookup_remove(
        template_lookup, template_lookup_epoch, packet_address, index);
    if (index != template_count) templates[index] = templates[template_count];
    if (index != template_count)
        xg_render_lookup_put(
            template_lookup, template_lookup_epoch,
            templates[index].packet_address, index);
    if (telemetry != NULL) {
        telemetry->field_builder_template_count = template_count;
        ++telemetry->field_builder_template_invalidation_count;
    }
}

void xg_render_field_sprite_retain_resident(
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);
    uint32_t retained_count = 0u;

    xy_override.active = false;
    if (services == NULL || services->lifecycle == NULL ||
        services->lifecycle->matches == NULL) {
        xg_render_field_sprite_clear_templates(services);
        return;
    }
    for (uint32_t index = 0u; index < template_count; ++index) {
        const XgRenderFieldSpriteRecord *record = &templates[index];

        if (record->lifecycle.scene_resource != 0u ||
            !services->lifecycle->matches(&record->lifecycle))
            continue;
        if (retained_count != index) templates[retained_count] = *record;
        ++retained_count;
    }
    template_count = retained_count;
    xg_render_lookup_reset(template_lookup, &template_lookup_epoch);
    for (uint32_t index = 0u; index < retained_count; ++index) {
        const XgRenderFieldSpriteRecord *record = &templates[index];

        xg_render_lookup_put(
            template_lookup, template_lookup_epoch,
            record->packet_address, index);
        if (services->register_replay_command != NULL)
            services->register_replay_command(
                (record->packet_address & UINT32_C(0x1fffffff)) + 4u);
    }
    if (telemetry != NULL)
        telemetry->field_builder_template_count = retained_count;
}

void xg_render_field_sprite_invalidate_overlapping(
        uint32_t address, uint32_t size) {
    for (uint32_t index = 0u; index < template_count; ++index) {
        XgRenderFieldSpriteRecord *record = &templates[index];

        if (normalized_ranges_overlap(
                record->packet_address + 4u, 0x24u, address, size) ||
            normalized_ranges_overlap(
                record->descriptor_address, 0x1cu, address, size))
            record->lifecycle = (XgRenderProducerLifecycle){0};
    }
}

uint16_t xg_render_field_sprite_tpage(
        uint16_t texture_depth, int16_t x, int16_t y) {
    return (uint16_t)(((texture_depth & 3u) << 7u) |
        (((uint16_t)y & 0x100u) >> 4u) |
        (((uint16_t)x & 0x3ffu) >> 6u));
}

uint16_t xg_render_field_sprite_clut(int16_t x, int16_t y) {
    return (uint16_t)((((uint16_t)y & 0x1ffu) << 6u) |
        (((uint16_t)x >> 4u) & 0x3fu));
}

static void observe_caller(
        uint32_t caller, PsxXgRenderSpriteFt4ShadowSnapshot *telemetry) {
    uint32_t empty = 4u;

    if (telemetry == NULL) return;
    for (uint32_t index = 0u; index < 4u; ++index) {
        if (telemetry->field_builder_caller_counts[index] != 0u &&
            physical_address_equals(
                telemetry->field_builder_caller_candidates[index], caller)) {
            ++telemetry->field_builder_caller_counts[index];
            return;
        }
        if (empty == 4u &&
            telemetry->field_builder_caller_counts[index] == 0u)
            empty = index;
    }
    if (empty != 4u) {
        telemetry->field_builder_caller_candidates[empty] = caller;
        telemetry->field_builder_caller_counts[empty] = 1u;
        return;
    }
    for (uint32_t index = 0u; index < 4u; ++index)
        --telemetry->field_builder_caller_counts[index];
}

static uint8_t overlay_family(uint32_t caller) {
    if (xg_render_overlay_ft4_projected_2e_descriptor_scope() &&
        (physical_address_equals(caller, UINT32_C(0x801d3e30)) ||
         physical_address_equals(caller, UINT32_C(0x801d3e70)) ||
         physical_address_equals(caller, UINT32_C(0x801d3eb0)) ||
         physical_address_equals(caller, UINT32_C(0x801d3ef0))))
        return 10u;
    if (physical_address_equals(caller, UINT32_C(0x801e855c)) ||
        physical_address_equals(caller, UINT32_C(0x801e8a38)))
        return 4u;
    if (physical_address_equals(caller, UINT32_C(0x801e8628)) ||
        physical_address_equals(caller, UINT32_C(0x801e8a98)))
        return 5u;
    return 0u;
}

static bool begin(
        CPUState *cpu, GuestRenderRenderMode render_mode, bool scene_active,
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);
    GpuDrawState draw = {0};
    XgRenderProducerLifecycle lifecycle = {0};
    uint32_t source_base;
    uint32_t table_entry;
    uint32_t descriptor_base;
    uint32_t packet_base;
    uint32_t packet_buffer;
    int16_t origin_x;
    int16_t origin_y;
    uint16_t scale;
    uint32_t count;

    if (telemetry != NULL) {
        ++telemetry->field_builder_begin_count;
        telemetry->last_field_builder_caller = cpu != NULL ? cpu->gpr[31] : 0u;
        observe_caller(telemetry->last_field_builder_caller, telemetry);
        if (scene_active) ++telemetry->field_builder_active_scene_count;
        builder.overlay_family = overlay_family(
            telemetry->last_field_builder_caller);
    } else {
        builder.overlay_family = overlay_family(
            cpu != NULL ? cpu->gpr[31] : 0u);
    }
    if (render_mode != GUEST_RENDER_RENDER_SHADOW &&
        render_mode != GUEST_RENDER_RENDER_NATIVE)
        return false;
    if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
        (services == NULL || services->lifecycle == NULL ||
         services->lifecycle->begin == NULL ||
         !services->lifecycle->begin(PRODUCER_PC, &lifecycle)))
        return false;
    if (telemetry != NULL && telemetry->field_builder_blocked) return false;
    if (telemetry != NULL && telemetry->field_builder_pending) {
        block_builder(1u, services);
        return false;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL)
        return false;
    source_base = cpu->gpr[4];
    if (cpu->gpr[5] > (UINT32_MAX - source_base - 4u) / 2u) {
        block_builder(2u, services);
        return false;
    }
    table_entry = source_base + cpu->gpr[5] * 2u + 4u;
    descriptor_base = source_base + cpu->read_half(table_entry);
    count = cpu->read_half(descriptor_base);
    packet_base = cpu->gpr[6];
    packet_buffer = cpu->gpr[7];
    origin_x = (int16_t)cpu->read_half(cpu->gpr[29] + 0x10u);
    origin_y = (int16_t)cpu->read_half(cpu->gpr[29] + 0x14u);
    scale = cpu->read_half(cpu->gpr[29] + 0x18u);
    if (count > BUILDER_CAPACITY) {
        block_builder(3u, services);
        return false;
    }
    gpu_get_draw_state(&draw);
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t descriptor = descriptor_base + 4u + index * 0x1cu;
        const uint32_t packet = packet_base + index * 0x50u +
            packet_buffer * 0x28u;
        XgRenderFieldSpriteRecord *record = &builder.records[index];
        XgRenderQuadSource source = {0};
        const int32_t x = origin_x +
            ((int32_t)(int16_t)cpu->read_half(descriptor + 8u) * scale) /
                4096;
        const int32_t y = origin_y +
            ((int32_t)(int16_t)cpu->read_half(descriptor + 10u) * scale) /
                4096;
        const int32_t width =
            ((int32_t)(int16_t)cpu->read_half(descriptor + 4u) * scale) /
                4096;
        const int32_t height =
            ((int32_t)(int16_t)cpu->read_half(descriptor + 6u) * scale) /
                4096;
        int32_t left = x;
        int32_t right = x + width;
        int32_t top = y;
        int32_t bottom = y + height;
        int32_t u0 = cpu->read_half(descriptor) - 1u;
        int32_t v0 = cpu->read_half(descriptor + 2u) - 1u;
        int32_t uv_width = cpu->read_byte(descriptor + 4u);
        int32_t uv_height = cpu->read_byte(descriptor + 6u);
        int32_t u1;
        int32_t v1;

        if (cpu->read_byte(descriptor + 0x1au) == 0u) {
            u0 = cpu->read_half(descriptor);
        } else {
            const int32_t swap = left;
            left = right;
            right = swap;
            if ((int16_t)u0 < 0) {
                u0 = 0;
                --uv_width;
            }
        }
        u1 = u0 + uv_width;
        if (cpu->read_byte(descriptor + 0x1bu) == 0u) {
            v0 = cpu->read_half(descriptor + 2u);
        } else {
            const int32_t swap = top;
            top = bottom;
            bottom = swap;
            if ((int16_t)v0 < 0) {
                v0 = 0;
                --uv_height;
            }
        }
        v1 = v0 + uv_height;
        record->lifecycle = lifecycle;
        record->packet_address = packet;
        record->descriptor_address = descriptor;
        record->interpolation_producer_id =
            packet_base & UINT32_C(0x1fffffff);
        record->interpolation_primitive_id = index;
        record->interpolation_identity_valid =
            record->interpolation_producer_id != 0u;
        if (telemetry != NULL) {
            if (telemetry->field_builder_min_packet == 0u ||
                packet < telemetry->field_builder_min_packet)
                telemetry->field_builder_min_packet = packet;
            if (packet > telemetry->field_builder_max_packet)
                telemetry->field_builder_max_packet = packet;
        }
        record->xy[0] = (uint16_t)left | ((uint32_t)(uint16_t)top << 16u);
        record->xy[1] = (uint16_t)right | ((uint32_t)(uint16_t)top << 16u);
        record->xy[2] = (uint16_t)left | ((uint32_t)(uint16_t)bottom << 16u);
        record->xy[3] = (uint16_t)right | ((uint32_t)(uint16_t)bottom << 16u);
        record->uv[0] = (uint8_t)u0 | ((uint16_t)(uint8_t)v0 << 8u);
        record->uv[1] = (uint8_t)u1 | ((uint16_t)(uint8_t)v0 << 8u);
        record->uv[2] = (uint8_t)u0 | ((uint16_t)(uint8_t)v1 << 8u);
        record->uv[3] = (uint8_t)u1 | ((uint16_t)(uint8_t)v1 << 8u);
        record->tpage = xg_render_field_sprite_tpage(
            cpu->read_half(descriptor + 16u),
            (int16_t)cpu->read_half(descriptor + 22u),
            (int16_t)cpu->read_half(descriptor + 24u));
        record->clut = xg_render_field_sprite_clut(
            (int16_t)cpu->read_half(descriptor + 18u),
            (int16_t)cpu->read_half(descriptor + 20u));
        xg_render_material_apply_draw_state(&source.material, &draw);
        source.material.tpage = record->tpage;
        source.material.texture_page_x = record->tpage & 0x0fu;
        source.material.texture_page_y = (record->tpage >> 4u) & 1u;
        source.material.texture_depth = (XgRenderIrTextureDepth)(
            (record->tpage >> 7u) & 3u);
        source.material.blend_mode = (XgRenderIrBlendMode)(
            (record->tpage >> 5u) & 3u);
        source.material.clut_x = (record->clut & 0x3fu) << 4u;
        source.material.clut_y = record->clut >> 6u;
        source.material.shading = XG_RENDER_IR_SHADING_FLAT;
        source.material.textured = true;
        source.material.raw_texture = true;
        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            source.vertices[vertex].x = (int16_t)record->xy[vertex];
            source.vertices[vertex].y = (int16_t)(record->xy[vertex] >> 16u);
            source.vertices[vertex].u = (uint8_t)record->uv[vertex];
            source.vertices[vertex].v = (uint8_t)(record->uv[vertex] >> 8u);
        }
        if (xg_render_quad_build_primitive(
                &source, &record->primitive) != XG_RENDER_QUAD_BUILDER_OK) {
            block_builder(4u, services);
            return false;
        }
    }
    builder.count = count;
    if (telemetry != NULL) telemetry->field_builder_pending = true;
    return true;
}

static bool publish_overlay(
        const XgRenderFieldSpriteRecord *record,
        XgRenderIrNativePrimitive primitive, uint32_t source_primitive_index,
        uint8_t family, bool material_ready,
        XgRenderFieldSpriteOverlayKind kind,
        const XgRenderFieldSpriteServices *services,
        uint32_t *failure_detail) {
    XgRenderFieldSpriteOverlayPublication publication;

    if (services == NULL || services->publish_overlay == NULL) return false;
    publication = (XgRenderFieldSpriteOverlayPublication){
        .primitive = primitive,
        .lifecycle = record->lifecycle,
        .packet_address = record->packet_address,
        .source_primitive_index = source_primitive_index,
        .interpolation_producer_id = record->interpolation_producer_id,
        .interpolation_primitive_id = record->interpolation_primitive_id,
        .family = family,
        .interpolation_identity_valid = record->interpolation_identity_valid,
        .material_ready = material_ready,
        .kind = kind,
    };
    return services->publish_overlay(&publication, failure_detail);
}

static void finish(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);
    bool all_match = true;

    if (telemetry == NULL || !telemetry->field_builder_pending) return;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL) {
        block_builder(6u, services);
        return;
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
        (builder.count == 0u || services == NULL ||
         services->lifecycle == NULL ||
         services->lifecycle->matches == NULL ||
         !services->lifecycle->matches(&builder.records[0].lifecycle))) {
        block_builder(10u, services);
        return;
    }
    telemetry->field_builder_primitive_count += builder.count;
    if (builder.overlay_family == 10u) {
        for (uint32_t index = 0u; index < builder.count; ++index) {
            const XgRenderFieldSpriteRecord *record = &builder.records[index];
            XgRenderIrNativePrimitive primitive = record->primitive;
            uint32_t failure_detail = 0u;

            primitive.material.raw_texture = false;
            primitive.material.semi_transparent = true;
            for (uint32_t triangle = 0u;
                 triangle < primitive.triangle_count; ++triangle) {
                for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
                    XgRenderIrVertex *target =
                        &primitive.triangles[triangle].vertices[vertex];
                    target->r = 0x80u;
                    target->g = 0x80u;
                    target->b = 0x80u;
                }
            }
            if (!publish_overlay(
                    record, primitive,
                    UINT32_C(0x2e3d0000) |
                        (record->packet_address & UINT32_C(0xffff)),
                    10u, true, XG_RENDER_FIELD_SPRITE_OVERLAY_PROJECTED_2E,
                    services, &failure_detail)) {
                telemetry->field_builder_failure_detail =
                    100u + failure_detail;
                block_builder(5u, services);
                return;
            }
        }
        xg_render_field_sprite_clear_builder(services);
        return;
    }
    for (uint32_t index = 0u; index < builder.count; ++index) {
        const XgRenderFieldSpriteRecord *record = &builder.records[index];
        const uint32_t command = cpu->read_word(record->packet_address + 4u);
        const uint16_t actual_clut =
            cpu->read_half(record->packet_address + 14u);
        const uint16_t actual_tpage =
            cpu->read_half(record->packet_address + 22u);
        uint32_t mismatch_bits =
            ((command & UINT32_C(0xff000000)) != UINT32_C(0x2d000000) ?
                 1u : 0u) |
            (actual_clut != record->clut ? 2u : 0u) |
            (actual_tpage != record->tpage ? 4u : 0u);

        for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
            if (cpu->read_word(record->packet_address + 8u + vertex * 8u) !=
                    record->xy[vertex])
                mismatch_bits |= 8u << vertex;
            if (cpu->read_half(record->packet_address + 12u + vertex * 8u) !=
                    record->uv[vertex])
                mismatch_bits |= 0x80u << vertex;
        }
        if (mismatch_bits == 0u) {
            ++telemetry->field_builder_match_count;
        } else {
            all_match = false;
            if (telemetry->field_builder_mismatch_count == 0u) {
                telemetry->field_builder_first_mismatch_packet =
                    record->packet_address;
                telemetry->field_builder_first_mismatch_descriptor =
                    record->descriptor_address;
                telemetry->field_builder_first_mismatch_caller =
                    telemetry->last_field_builder_caller;
                telemetry->field_builder_first_mismatch_bits = mismatch_bits;
                telemetry->field_builder_expected_tpage = record->tpage;
                telemetry->field_builder_actual_tpage = actual_tpage;
                telemetry->field_builder_expected_clut = record->clut;
                telemetry->field_builder_actual_clut = actual_clut;
                telemetry->field_builder_actual_command = command;
                for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
                    telemetry->field_builder_expected_xy[vertex] =
                        record->xy[vertex];
                    telemetry->field_builder_actual_xy[vertex] = cpu->read_word(
                        record->packet_address + 8u + vertex * 8u);
                    telemetry->field_builder_expected_uv[vertex] =
                        record->uv[vertex];
                    telemetry->field_builder_actual_uv[vertex] = cpu->read_half(
                        record->packet_address + 12u + vertex * 8u);
                }
            }
            ++telemetry->field_builder_mismatch_count;
        }
    }
    if (render_mode == GUEST_RENDER_RENDER_NATIVE) {
        uint32_t staged_count = 0u;

        if (!all_match) {
            block_builder(8u, services);
            return;
        }
        for (uint32_t index = 0u; index < builder.count; ++index) {
            if (!capture_template(&builder.records[index], services)) {
                block_builder(9u, services);
                return;
            }
        }
        for (uint32_t index = 0u; index < builder.count; ++index) {
            const XgRenderFieldSpriteRecord *record = &builder.records[index];
            const uint32_t packet = record->packet_address;
            uint32_t failure_detail = 0u;

            if (builder.overlay_family != 0u) {
                if (!publish_overlay(
                        record, record->primitive,
                        UINT32_C(0x2e580000) |
                            (packet & UINT32_C(0xffff)),
                        builder.overlay_family, false,
                        XG_RENDER_FIELD_SPRITE_OVERLAY_FIELD,
                        services, &failure_detail)) {
                    telemetry->field_builder_failure_detail =
                        200u + failure_detail;
                    block_builder(5u, services);
                    return;
                }
                continue;
            }
            if (services == NULL || services->stage_primitive == NULL ||
                !services->stage_primitive(
                    &record->primitive, packet,
                    UINT32_C(0x53000000) |
                        (packet & UINT32_C(0x001ffffc)),
                    record->interpolation_producer_id,
                    record->interpolation_primitive_id, &failure_detail)) {
                telemetry->field_builder_failure_detail =
                    1000u + failure_detail;
                block_builder(5u, services);
                return;
            }
            ++staged_count;
        }
        if (staged_count != 0u)
            ++telemetry->field_builder_native_cutover_count;
        telemetry->field_builder_native_primitive_count += staged_count;
    }
    xg_render_field_sprite_clear_builder(services);
}

static void xy_override_begin(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderFieldSpriteServices *services) {
    XgRenderFieldSpriteRecord *source;

    xy_override.active = false;
    if (cpu == NULL || render_mode != GUEST_RENDER_RENDER_NATIVE ||
        services == NULL || services->lifecycle == NULL ||
        services->lifecycle->matches == NULL)
        return;
    source = find_template(cpu->gpr[3]);
    if (source == NULL ||
        !services->lifecycle->matches(&source->lifecycle))
        return;
    xy_override.source = *source;
    xy_override.active = true;
}

static void xy_override_finish(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderFieldSpriteServices *services) {
    static const uint8_t split[2][3] = {{0u, 1u, 2u}, {2u, 1u, 3u}};
    XgRenderFieldSpriteRecord record;

    if (!xy_override.active) return;
    record = xy_override.source;
    xy_override.active = false;
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        render_mode != GUEST_RENDER_RENDER_NATIVE || services == NULL ||
        services->lifecycle == NULL || services->lifecycle->matches == NULL ||
        !physical_address_equals(cpu->gpr[3], record.packet_address) ||
        !services->lifecycle->matches(&record.lifecycle) ||
        (cpu->read_word(record.packet_address + 4u) & UINT32_C(0xff000000)) !=
            UINT32_C(0x2d000000) ||
        cpu->read_half(record.packet_address + 14u) != record.clut ||
        cpu->read_half(record.packet_address + 22u) != record.tpage)
        return;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t xy =
            cpu->read_word(record.packet_address + 8u + vertex * 8u);

        if (cpu->read_half(record.packet_address + 12u + vertex * 8u) !=
                record.uv[vertex])
            return;
        record.xy[vertex] = xy;
    }
    for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
        for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t xy = record.xy[split[triangle][vertex]];
            XgRenderIrVertex *target =
                &record.primitive.triangles[triangle].vertices[vertex];

            target->x = (int32_t)(int16_t)xy * INT32_C(65536);
            target->y = (int32_t)(int16_t)(xy >> 16u) * INT32_C(65536);
        }
    }
    record.semantic_ready = false;
    (void)capture_template(&record, services);
}

bool xg_render_field_sprite_observe(
        CPUState *cpu, uint32_t pc, uint32_t instruction_word,
        GuestRenderRenderMode render_mode, bool scene_active,
        const XgRenderFieldSpriteServices *services) {
    if (physical_address_equals(pc, PRODUCER_PC) &&
        instruction_word == UINT32_C(0x27bdffb0)) {
        (void)begin(cpu, render_mode, scene_active, services);
        return true;
    }
    if (physical_address_equals(pc, UINT32_C(0x800269cc)) &&
        instruction_word == UINT32_C(0x8fa90020)) {
        finish(cpu, render_mode, services);
        return true;
    }
    if (physical_address_equals(pc, UINT32_C(0x801c9984)) &&
        instruction_word == UINT32_C(0xa4620008)) {
        xy_override_begin(cpu, render_mode, services);
        return true;
    }
    if (physical_address_equals(pc, UINT32_C(0x801c9b80)) &&
        instruction_word == UINT32_C(0x02801021)) {
        xy_override_finish(cpu, render_mode, services);
        return true;
    }
    return false;
}

bool xg_render_field_sprite_resolve(
        const GuestRenderNativeStreamMissContext *context,
        GpuRenderSemantic *out_semantic, GuestRenderRenderMode render_mode,
        const XgRenderFieldSpriteServices *services) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);
    XgRenderFieldSpriteRecord *record;
    uint32_t packet_address;
    uint32_t lookup_key;
    uint32_t indexed;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        command_id < 4u || render_mode != GUEST_RENDER_RENDER_NATIVE ||
        services == NULL || services->lifecycle == NULL ||
        services->lifecycle->matches_replay == NULL ||
        services->lifecycle->replay_container_matches_command == NULL)
        return false;
    packet_address = (uint32_t)command_id - 4u;
    indexed = xg_render_lookup_find(
        template_lookup, template_lookup_epoch, packet_address, template_count);
    if (xg_render_lookup_key(packet_address, &lookup_key)) {
        if (indexed == UINT32_MAX) return false;
        record = &templates[indexed];
        if (!physical_address_equals(record->packet_address, packet_address))
            return false;
    } else {
        record = find_template(packet_address);
    }
    if (record == NULL ||
        !services->lifecycle->replay_container_matches_command(context) ||
        !services->lifecycle->matches_replay(&record->lifecycle, context) ||
        ((record->packet_address & UINT32_C(0x1fffffff)) + 4u) !=
            (uint32_t)command_id ||
        !xg_render_primitive_translate_cached(
            &record->primitive, &record->semantic, &record->semantic_ready,
            out_semantic))
        return false;
    if (record->interpolation_identity_valid &&
        services->interpolation_scene != NULL)
        xg_render_semantic_set_interpolation_identity(
            out_semantic, services->interpolation_scene(),
            record->interpolation_producer_id,
            record->interpolation_primitive_id);
    if (telemetry != NULL)
        ++telemetry->field_builder_dma_replay_primitive_count;
    return true;
}

void xg_render_field_sprite_invalidate_code(
        bool code_write, bool shared_data_write,
        const XgRenderFieldSpriteServices *services) {
    PsxXgRenderSpriteFt4ShadowSnapshot *telemetry = snapshot(services);

    if (code_write) {
        if (telemetry != NULL && telemetry->field_builder_pending)
            block_builder(7u, services);
        else
            xg_render_field_sprite_clear_builder(services);
    } else if (shared_data_write) {
        xg_render_field_sprite_clear_builder(services);
    }
    if (code_write || shared_data_write)
        xg_render_field_sprite_clear_templates(services);
}

void xg_render_field_sprite_diagnostics_update_snapshot(
        PsxXgRenderSpriteFt4ShadowSnapshot *in_out_snapshot) {
    const PsxXgRenderSpriteFt4ShadowSnapshot *source =
        &field_sprite_diagnostics;

    if (in_out_snapshot == NULL) return;
#define COPY_FIELD(field) in_out_snapshot->field = source->field
    COPY_FIELD(field_builder_begin_count);
    COPY_FIELD(field_builder_native_cutover_count);
    COPY_FIELD(field_builder_native_primitive_count);
    COPY_FIELD(field_builder_template_capture_count);
    COPY_FIELD(field_builder_template_update_count);
    COPY_FIELD(field_builder_template_invalidation_count);
    COPY_FIELD(field_builder_dma_replay_primitive_count);
    COPY_FIELD(field_builder_primitive_count);
    COPY_FIELD(field_builder_match_count);
    COPY_FIELD(field_builder_mismatch_count);
    COPY_FIELD(field_builder_active_scene_count);
    COPY_FIELD(last_field_builder_caller);
    memcpy(in_out_snapshot->field_builder_caller_candidates,
           source->field_builder_caller_candidates,
           sizeof(source->field_builder_caller_candidates));
    memcpy(in_out_snapshot->field_builder_caller_counts,
           source->field_builder_caller_counts,
           sizeof(source->field_builder_caller_counts));
    COPY_FIELD(field_builder_first_mismatch_packet);
    COPY_FIELD(field_builder_first_mismatch_descriptor);
    COPY_FIELD(field_builder_first_mismatch_caller);
    COPY_FIELD(field_builder_first_mismatch_bits);
    memcpy(in_out_snapshot->field_builder_expected_xy,
           source->field_builder_expected_xy,
           sizeof(source->field_builder_expected_xy));
    memcpy(in_out_snapshot->field_builder_actual_xy,
           source->field_builder_actual_xy,
           sizeof(source->field_builder_actual_xy));
    memcpy(in_out_snapshot->field_builder_expected_uv,
           source->field_builder_expected_uv,
           sizeof(source->field_builder_expected_uv));
    memcpy(in_out_snapshot->field_builder_actual_uv,
           source->field_builder_actual_uv,
           sizeof(source->field_builder_actual_uv));
    COPY_FIELD(field_builder_expected_tpage);
    COPY_FIELD(field_builder_actual_tpage);
    COPY_FIELD(field_builder_expected_clut);
    COPY_FIELD(field_builder_actual_clut);
    COPY_FIELD(field_builder_actual_command);
    COPY_FIELD(field_builder_failure_detail);
    COPY_FIELD(field_builder_blocker);
    COPY_FIELD(field_builder_min_packet);
    COPY_FIELD(field_builder_max_packet);
    COPY_FIELD(field_builder_template_count);
    COPY_FIELD(field_builder_pending);
    COPY_FIELD(field_builder_blocked);
#undef COPY_FIELD
}

void xg_render_field_sprite_diagnostics_snapshot(
        PsxXgRenderSpriteFt4ShadowSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = (PsxXgRenderSpriteFt4ShadowSnapshot){0};
    xg_render_field_sprite_diagnostics_update_snapshot(out_snapshot);
}

void xg_render_field_sprite_diagnostics_reset(void) {
    field_sprite_diagnostics = (PsxXgRenderSpriteFt4ShadowSnapshot){0};
}

void xg_render_field_sprite_reset(void) {
    builder = (XgRenderFieldSpriteBuilder){0};
    template_count = 0u;
    xy_override.active = false;
    xg_render_lookup_reset(template_lookup, &template_lookup_epoch);
    xg_render_field_sprite_diagnostics_reset();
}

void xg_render_field_sprite_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.resource_mutation)
            xg_render_field_sprite_invalidate_overlapping(
                event->address, event->size);
        xg_render_field_sprite_invalidate_code(
            xg_render_invalidation_has_code_class(
                event, PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4),
            xg_render_invalidation_has_code_class(
                event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA),
            services->field_sprite);
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE) {
        xg_render_field_sprite_clear_builder(services->field_sprite);
        xg_render_field_sprite_clear_templates(services->field_sprite);
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        /* Full clear, not retain-resident: a field<->battle transition
         * must not let templates survive across it. */
        xg_render_field_sprite_clear_builder(services->field_sprite);
        xg_render_field_sprite_clear_templates(services->field_sprite);
    } else if (event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
               event->kind == XG_RENDER_INVALIDATION_RESOURCE_OVERLAP) {
        xg_render_field_sprite_retain_resident(services->field_sprite);
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_field_sprite_reset();
    }
}
