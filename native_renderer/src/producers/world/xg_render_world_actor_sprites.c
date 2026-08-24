#include "xg_render_world_pending_services.h"

#include "gpu.h"
#include "xg_field_render_services.h"
#include "xg_world_actor_sprites_source_capture.h"

#include <string.h>

typedef struct XgRenderWorldActorSpritesState {
    XgWorldActorSpriteRecord
        records[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY];
    XgWorldActorSpritesNativePreparation preparation;
    uint32_t payload_words[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY]
                           [XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT];
    uint32_t packet_tags[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY];
    uint32_t ot_addresses[XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY];
    uint32_t ot_heads[XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT];
    bool ot_touched[XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT];
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t ot_base;
    CPUState *owner_cpu;
    bool valid;
} XgRenderWorldActorSpritesState;

typedef struct XgRenderWorldActorContext {
    uint64_t authentication_generation;
    uint32_t entry_stack_pointer;
    uint32_t caller_return;
    CPUState *owner_cpu;
    uint32_t depth;
    bool poisoned;
    bool active;
} XgRenderWorldActorContext;

typedef struct XgRenderWorldActorReaderContext {
    CPUState *cpu;
    const XgRenderWorldPendingServices *services;
} XgRenderWorldActorReaderContext;

static XgRenderWorldActorSpritesState actor;
static XgRenderWorldActorContext context;
static PsxXgRenderWorldNativeSnapshot snapshot;

static bool services_valid(const XgRenderWorldPendingServices *services) {
    return services != NULL && services->cutover_ready != NULL &&
        services->authentication_generation != NULL &&
        services->authorize_guest_range != NULL &&
        services->begin_submission != NULL && services->stage_native != NULL &&
        services->coordinator_fail != NULL;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool read_u8(void *opaque, uint32_t address, uint8_t *out_value) {
    XgRenderWorldActorReaderContext *reader = opaque;
    CPUState *cpu = reader != NULL ? reader->cpu : NULL;

    if (cpu == NULL || cpu->read_byte == NULL || out_value == NULL ||
        !reader->services->authorize_guest_range(address, 1u, 1u, true))
        return false;
    *out_value = cpu->read_byte(address);
    return true;
}

static bool read_u16(void *opaque, uint32_t address, uint16_t *out_value) {
    XgRenderWorldActorReaderContext *reader = opaque;
    CPUState *cpu = reader != NULL ? reader->cpu : NULL;

    if (cpu == NULL || cpu->read_half == NULL || out_value == NULL ||
        !reader->services->authorize_guest_range(address, 2u, 2u, true))
        return false;
    *out_value = cpu->read_half(address);
    return true;
}

static bool read_u32(void *opaque, uint32_t address, uint32_t *out_value) {
    XgRenderWorldActorReaderContext *reader = opaque;
    CPUState *cpu = reader != NULL ? reader->cpu : NULL;

    if (cpu == NULL || cpu->read_word == NULL || out_value == NULL ||
        !reader->services->authorize_guest_range(address, 4u, 4u, true))
        return false;
    *out_value = cpu->read_word(address);
    return true;
}

static bool authorize_source_range(
        void *opaque, XgWorldActorSpritesSourceRangeKind kind,
        uint32_t address, uint32_t size) {
    XgRenderWorldActorReaderContext *reader = opaque;
    uint32_t alignment;

    switch (kind) {
    case XG_WORLD_ACTOR_SPRITES_SOURCE_ACTOR:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_DATA:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_DESCRIPTORS:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_CONTEXT:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_TRIG_TABLE:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_CAMERA:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_SCRATCH_TEMPLATE:
    case XG_WORLD_ACTOR_SPRITES_SOURCE_GLOBAL:
        alignment = 4u;
        break;
    case XG_WORLD_ACTOR_SPRITES_SOURCE_PARTS:
        alignment = 2u;
        break;
    default:
        return false;
    }
    return reader != NULL && reader->services->authorize_guest_range(
        address, size, alignment, false);
}

static bool snapshot_can_record(uint32_t primitive_count) {
    return snapshot.native_cutover_count != UINT64_MAX &&
        snapshot.native_primitive_count <= UINT64_MAX - primitive_count;
}

void xg_render_world_actor_clear_context(void) {
    if (!context.active && !context.poisoned) return;
    context = (XgRenderWorldActorContext){0};
}

void xg_render_world_actor_clear_pending(void) {
    if (!actor.valid) return;
    actor.valid = false;
}

bool xg_render_world_actor_context_active(void) {
    return context.active;
}

bool xg_render_world_actor_pending_valid(void) {
    return actor.valid;
}

void xg_render_world_actor_context_begin(
        CPUState *cpu, const XgRenderWorldPendingServices *services) {
    uint64_t generation;

    if (!services_valid(services)) return;
    if (context.active) {
        if (context.depth != UINT32_MAX) ++context.depth;
        context.poisoned = true;
        services->coordinator_fail();
        xg_render_world_actor_clear_pending();
        return;
    }
    xg_render_world_actor_clear_pending();
    xg_render_world_actor_clear_context();
    if (!services->cutover_ready() || cpu == NULL ||
        !xg_render_runtime_stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] < 0x28u ||
        !services->authorize_guest_range(
            cpu->gpr[29] - 0x28u, 0x28u, 4u, true) ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_ACTOR_SPRITES_WORLD_CALLER_RETURN) ||
        !services->authentication_generation(&generation))
        return;
    context = (XgRenderWorldActorContext){
        .authentication_generation = generation,
        .entry_stack_pointer = cpu->gpr[29],
        .caller_return = cpu->gpr[31],
        .owner_cpu = cpu,
        .depth = 1u,
        .active = true,
    };
}

void xg_render_world_actor_context_finish(
        CPUState *cpu, const XgRenderWorldPendingServices *services) {
    if (!context.active) return;
    if (context.depth > 1u) {
        if (context.owner_cpu != cpu) context.poisoned = true;
        --context.depth;
        return;
    }
    if (context.poisoned || context.owner_cpu != cpu || actor.valid ||
        cpu == NULL || cpu->read_word == NULL ||
        context.entry_stack_pointer < 0x28u ||
        cpu->gpr[29] != context.entry_stack_pointer - 0x28u ||
        services == NULL || services->authorize_guest_range == NULL ||
        !services->authorize_guest_range(cpu->gpr[29], 0x28u, 4u, true) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + 0x20u), context.caller_return)) {
        if (services != NULL && services->coordinator_fail != NULL)
            services->coordinator_fail();
        xg_render_world_actor_clear_pending();
        xg_render_world_actor_clear_context();
        return;
    }
    xg_render_world_actor_clear_context();
}

static bool scratch_output_matches(
        CPUState *cpu, const XgWorldActorSpritesScratchOutput *scratch) {
    uint32_t vertex;

    if (!scratch->written) return true;
    if (cpu == NULL || cpu->read_half == NULL) return false;
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        const uint32_t address = scratch->address + vertex * 8u;

        if (cpu->read_half(address) !=
                (uint16_t)scratch->vertices[vertex].x ||
            cpu->read_half(address + 2u) !=
                (uint16_t)scratch->vertices[vertex].y ||
            cpu->read_half(address + 4u) !=
                (uint16_t)scratch->vertices[vertex].z ||
            cpu->read_half(address + 6u) != scratch->vertices[vertex].pad)
            return false;
    }
    return true;
}

bool xg_render_world_actor_prepare(
        CPUState *cpu, const XgRenderWorldPendingServices *services) {
    XgWorldActorSpritesNativePreparation *preparation = &actor.preparation;
    XgWorldActorSpritesNativeRequest request = {0};
    XgRenderWorldActorReaderContext reader_context = {cpu, services};
    XgWorldActorSpritesAuthenticatedReader reader;
    GpuDrawState draw = {0};
    uint32_t resident_caller_return;
    uint32_t context_address;
    uint32_t ot_base;
    uint32_t index;

    if (!services_valid(services) || !services->cutover_ready() ||
        !context.active || context.poisoned || context.depth != 1u ||
        context.owner_cpu != cpu || actor.valid || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL || context.entry_stack_pointer < 0x48u ||
        cpu->gpr[29] != context.entry_stack_pointer - 0x48u ||
        !services->authorize_guest_range(cpu->gpr[29], 0x48u, 4u, true) ||
        !physical_address_equals(
            cpu->gpr[31], XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM) ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + 0x40u), context.caller_return) ||
        context.authentication_generation == 0u)
        return false;
    resident_caller_return = cpu->read_word(cpu->gpr[29] + 0x18u);
    if (!physical_address_equals(
            resident_caller_return,
            XG_WORLD_ACTOR_SPRITES_RESIDENT_CALLER_RETURN))
        return false;
    context_address = cpu->read_word(UINT32_C(0x8009be3c));
    if (!services->authorize_guest_range(
            context_address, 0x74u, 4u, false))
        return false;
    ot_base = cpu->read_word(context_address + 0x70u);
    gpu_get_draw_state(&draw);
    request = (XgWorldActorSpritesNativeRequest){
        .authentication_generation = context.authentication_generation,
        .resident_entry_pc = XG_WORLD_ACTOR_SPRITES_RESIDENT_ENTRY,
        .prepared_seam_pc = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        .resident_caller_return = resident_caller_return,
        .actor_address = cpu->gpr[16],
        .ordering_table_address = cpu->gpr[17],
        .screen_offset_x = (int32_t)cpu->gte_ctrl[24],
        .screen_offset_y = (int32_t)cpu->gte_ctrl[25],
        .projection_distance = (uint16_t)cpu->gte_ctrl[26],
        .depth_cue_a = xg_render_runtime_low_s16(cpu->gte_ctrl[27]),
        .depth_cue_b = (int32_t)cpu->gte_ctrl[28],
        .average_z_scale4 = xg_render_runtime_low_s16(cpu->gte_ctrl[30]),
        .raster = {
            .draw_area_left = draw.left,
            .draw_area_top = draw.top,
            .draw_area_right = draw.right,
            .draw_area_bottom = draw.bottom,
            .draw_offset_x = draw.offset_x,
            .draw_offset_y = draw.offset_y,
            .texture_window_mask_x = draw.texture_window_mask_x,
            .texture_window_mask_y = draw.texture_window_mask_y,
            .texture_window_offset_x = draw.texture_window_offset_x,
            .texture_window_offset_y = draw.texture_window_offset_y,
            .dither = draw.dither,
            .mask_set = draw.mask_set,
            .mask_check = draw.mask_check,
        },
        .resident_context_authenticated = true,
        .projection_state_authenticated = true,
    };
    reader = (XgWorldActorSpritesAuthenticatedReader){
        .context = &reader_context,
        .read_u8 = read_u8,
        .read_u16 = read_u16,
        .read_u32 = read_u32,
        .authorize_source_range = authorize_source_range,
        .authentication_generation = context.authentication_generation,
        .authenticated = true,
    };
    memset(actor.ot_touched, 0, sizeof(actor.ot_touched));
    if (xg_world_actor_sprites_native_prepare(
            &request, &reader, actor.records,
            XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY, preparation) !=
            XG_WORLD_ACTOR_SPRITES_NATIVE_OK ||
        !preparation->authenticated || !preparation->sealed ||
        preparation->authentication_generation !=
            context.authentication_generation ||
        preparation->record_count >
            XG_WORLD_ACTOR_SPRITES_NATIVE_RECORD_CAPACITY ||
        preparation->actor_address != cpu->gpr[16] ||
        !physical_address_equals(
            preparation->continuation_pc,
            XG_WORLD_ACTOR_SPRITES_CONTINUATION) ||
        !snapshot_can_record(preparation->record_count))
        return false;

    if ((preparation->body_scratch.written &&
         !services->authorize_guest_range(
             preparation->body_scratch.address, 0x20u, 2u, false)) ||
        (preparation->shadow_scratch.written &&
         !services->authorize_guest_range(
             preparation->shadow_scratch.address, 0x20u, 2u, false)) ||
        (preparation->packet_cursor_written &&
         !services->authorize_guest_range(
             XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR, 4u, 4u, false)))
        return false;
    actor.ot_base = ot_base;
    for (index = 0u; index < preparation->record_count; ++index) {
        const XgWorldActorSpriteRecord *record = &actor.records[index];
        const uint32_t packet = record->packet_address;
        const uint32_t ot_address = record->ordering_table_address;
        uint32_t bucket;
        uint32_t payload;

        if (record->tag_payload_word_count !=
                XG_WORLD_ACTOR_SPRITE_TAG_PAYLOAD_WORD_COUNT ||
            !services->authorize_guest_range(
                packet, XG_WORLD_ACTOR_SPRITE_PACKET_STRIDE, 4u, false) ||
            !services->authorize_guest_range(ot_address, 4u, 4u, false) ||
            ot_address < ot_base ||
            ot_address > ot_base +
                (XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT - 1u) * 4u ||
            ((ot_address - ot_base) & 3u) != 0u)
            return false;
        bucket = (ot_address - ot_base) / 4u;
        actor.ot_addresses[index] = ot_address;
        for (payload = 0u;
             payload < XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT;
             ++payload) {
            const uint32_t actual = cpu->read_word(
                packet + 4u + payload * 4u);
            const uint32_t mask =
                record->packet_payload_write_masks[payload];

            if (mask != UINT32_MAX && mask != UINT32_C(0x0000ffff))
                return false;
            actor.payload_words[index][payload] = (actual & ~mask) |
                (record->packet_payload_words[payload] & mask);
        }
        if (!actor.ot_touched[bucket]) {
            actor.ot_heads[bucket] = cpu->read_word(ot_address);
            actor.ot_touched[bucket] = true;
        }
        actor.packet_tags[index] = UINT32_C(0x09000000) |
            (actor.ot_heads[bucket] & UINT32_C(0x00ffffff));
        actor.ot_heads[bucket] =
            (actor.ot_heads[bucket] & UINT32_C(0xff000000)) |
            (record->packet_address & UINT32_C(0x00ffffff));
    }
    actor.authentication_generation = context.authentication_generation;
    actor.entry_stack_pointer = cpu->gpr[29];
    actor.owner_cpu = cpu;
    actor.valid = true;
    return true;
}

bool xg_render_world_actor_commit(
        CPUState *cpu, const XgRenderWorldPendingServices *services) {
    const XgWorldActorSpritesNativePreparation *preparation =
        &actor.preparation;
    uint64_t generation;
    uint32_t index;

    if (!services_valid(services) || !actor.valid || actor.owner_cpu != cpu ||
        !context.active || context.poisoned || context.depth != 1u ||
        context.owner_cpu != cpu || !services->cutover_ready() ||
        !services->authentication_generation(&generation) || cpu == NULL ||
        cpu->read_word == NULL || cpu->read_half == NULL ||
        generation != actor.authentication_generation ||
        cpu->gpr[29] != actor.entry_stack_pointer ||
        !physical_address_equals(
            cpu->read_word(cpu->gpr[29] + 0x40u), context.caller_return) ||
        cpu->read_word(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR) !=
            preparation->final_packet_cursor ||
        !scratch_output_matches(cpu, &preparation->body_scratch) ||
        !scratch_output_matches(cpu, &preparation->shadow_scratch))
        goto fail;

    for (index = 0u; index < preparation->record_count; ++index) {
        const XgWorldActorSpriteRecord *record = &actor.records[index];
        uint32_t payload;

        if (cpu->read_word(record->packet_address) != actor.packet_tags[index])
            goto fail;
        for (payload = 0u;
             payload < XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT;
             ++payload) {
            if (cpu->read_word(
                    record->packet_address + 4u + payload * 4u) !=
                actor.payload_words[index][payload])
                goto fail;
        }
    }
    for (index = 0u; index < XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT; ++index) {
        if (actor.ot_touched[index] &&
            cpu->read_word(actor.ot_base + index * 4u) != actor.ot_heads[index])
            goto fail;
    }
    if (preparation->record_count != 0u && !services->begin_submission())
        goto fail;
    for (index = 0u; index < preparation->record_count; ++index) {
        const XgWorldActorSpriteRecord *record = &actor.records[index];
        uint32_t interpolation_primitive_id;

        if (record->descriptor_index > (UINT32_MAX - 1u) / 2u) goto fail;
        interpolation_primitive_id =
            record->descriptor_index * 2u + (uint32_t)record->family;
        if (!services->stage_native(
                &record->sprite.primitive, record->packet_address,
                UINT32_C(0x67000000) |
                    ((record->packet_address & UINT32_C(0x001ffffc)) >> 2u),
                preparation->actor_address & UINT32_C(0x1fffffff),
                interpolation_primitive_id))
            goto fail;
    }
    ++snapshot.native_cutover_count;
    snapshot.native_primitive_count += preparation->record_count;
    xg_render_world_actor_clear_pending();
    return true;

fail:
    xg_render_world_actor_clear_pending();
    return false;
}

void xg_render_world_actor_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = snapshot;
}

bool xg_render_world_actor_pending_metadata_copy(
        CPUState **out_owner_cpu,
        XgWorldActorSpritesNativePreparation *out_preparation) {
    if (!actor.valid || out_owner_cpu == NULL || out_preparation == NULL)
        return false;
    *out_owner_cpu = actor.owner_cpu;
    *out_preparation = actor.preparation;
    return true;
}

bool xg_render_world_actor_pending_packet_copy(
        uint32_t index, uint32_t *out_address, uint32_t *out_tag,
        uint32_t *out_payload_words, uint32_t capacity) {
    if (!actor.valid || out_address == NULL || out_tag == NULL ||
        out_payload_words == NULL ||
        capacity < XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT ||
        index >= actor.preparation.record_count)
        return false;
    *out_address = actor.records[index].packet_address;
    *out_tag = actor.packet_tags[index];
    memcpy(out_payload_words, actor.payload_words[index],
           XG_WORLD_ACTOR_SPRITE_PACKET_PAYLOAD_WORD_COUNT *
               sizeof(out_payload_words[0]));
    return true;
}

bool xg_render_world_actor_pending_ot_copy(
        uint32_t index, uint32_t *out_address, uint32_t *out_value) {
    if (!actor.valid || out_address == NULL || out_value == NULL ||
        index >= XG_WORLD_ACTOR_SPRITE_OT_BUCKET_COUNT ||
        !actor.ot_touched[index])
        return false;
    *out_address = actor.ot_base + index * 4u;
    *out_value = actor.ot_heads[index];
    return true;
}

void xg_render_world_actor_reset(void) {
    actor = (XgRenderWorldActorSpritesState){0};
    context = (XgRenderWorldActorContext){0};
    snapshot = (PsxXgRenderWorldNativeSnapshot){0};
}

void xg_render_world_actor_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    const bool semantic_write = event->mutation.semantic_authority_loss ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_SKY) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);

    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
        event->kind == XG_RENDER_INVALIDATION_AUTHORITY_LOST ||
        event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
        (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE && semantic_write)) {
        xg_render_world_actor_clear_pending();
        xg_render_world_actor_clear_context();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_world_actor_reset();
    }
}
