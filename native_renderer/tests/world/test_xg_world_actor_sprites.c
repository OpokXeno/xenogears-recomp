#include "xg_world_actor_sprites.h"
#include "xg_world_actor_sprites_source_capture.h"

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

static void set_identity(XgHost3dMatrix *matrix) {
    memset(matrix, 0, sizeof(*matrix));
    matrix->rotation[0][0] = 4096;
    matrix->rotation[1][1] = 4096;
    matrix->rotation[2][2] = 4096;
}

static XgWorldActorSpritesValueSource fixture(
    XgWorldActorSpriteActorSource *actor,
    XgWorldActorSpriteDescriptor *descriptor) {
    XgWorldActorSpritesValueSource source = { 0 };

    memset(actor, 0, sizeof(*actor));
    memset(descriptor, 0, sizeof(*descriptor));
    source.camera_projection.rotation[0][0] = 4096;
    source.camera_projection.rotation[1][1] = 4096;
    source.camera_projection.rotation[2][2] = 4096;
    source.camera_projection.screen_offset_x = 160 << 16;
    source.camera_projection.screen_offset_y = 120 << 16;
    source.camera_projection.projection_distance = 256u;
    source.camera_projection.average_z_scale4 = 1024;
    source.material_template.draw_area_right = 319u;
    source.material_template.draw_area_bottom = 239u;
    source.material_template.texture_window_mask_x = 1u;
    source.material_template.texture_window_mask_y = 2u;
    source.material_template.texture_window_offset_x = 3u;
    source.material_template.texture_window_offset_y = 4u;
    source.actors = actor;
    source.actor_count = 1u;

    actor->position[2] = -(512 << 12);
    actor->scale = 4096;
    actor->active = true;
    actor->sprite_present = true;
    actor->shadow_enabled = true;
    actor->descriptors = descriptor;
    actor->descriptor_count = 1u;
    set_identity(&actor->resolved_sprite_matrix);

    descriptor->x = -32;
    descriptor->y = -48;
    descriptor->u = 8u;
    descriptor->v = 16u;
    descriptor->width = 64u;
    descriptor->height = 96u;
    descriptor->tpage = 0x0023u;
    descriptor->clut = 0x0042u;
    descriptor->material_word = UINT32_C(0x2d806040);
    return source;
}

static int test_adapts_world_values_and_builds_body_and_shadow(void) {
    XgWorldActorSpriteActorSource actor;
    XgWorldActorSpriteDescriptor descriptor;
    XgWorldActorSpritesValueSource source = fixture(&actor, &descriptor);
    XgWorldActorSpriteActorSource actor_before = actor;
    XgWorldActorSpriteDescriptor descriptor_before = descriptor;
    XgWorldActorSpritesAdaptedActor adapted;
    XgWorldActorSpriteRecord records[2];
    uint32_t count = 0u;

    CHECK(xg_world_actor_sprites_adapt_actor(&source, 0u, &adapted) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(adapted.accepted);
    CHECK(adapted.actor_position.x == 0);
    CHECK(adapted.actor_position.z == 512);
    CHECK(adapted.depth == 512u);
    CHECK(adapted.ordering_bucket == 32u);
    CHECK(adapted.body_projection.translation[2] == 512);
    CHECK(adapted.shadow_projection.rotation[0][0] == 4096);
    CHECK(adapted.shadow_projection.rotation[1][1] == 2048);
    CHECK(adapted.shadow_projection.rotation[2][2] == 0);

    CHECK(xg_world_actor_sprites_build(&source, records, 2u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(count == 2u);
    CHECK(records[0].family == XG_WORLD_ACTOR_SPRITE_BODY);
    CHECK(records[0].ordering_bucket == 32u);
    CHECK(records[0].sprite.vertices[0].x == 144);
    CHECK(records[0].sprite.vertices[0].y == 96);
    CHECK(records[0].sprite.vertices[3].x == 176);
    CHECK(records[0].sprite.vertices[3].y == 144);
    CHECK(records[0].sprite.primitive.triangles[0].vertices[0].r == 0x40u);
    CHECK(records[0].sprite.primitive.triangles[0].vertices[0].g == 0x60u);
    CHECK(!records[0].sprite.primitive.triangles[0].vertices[0]
               .projective_position);
    CHECK(records[0].sprite.primitive.triangles[0].vertices[0].u ==
          8 * INT32_C(65536));
    CHECK(records[0].sprite.primitive.triangles[0].vertices[1].u ==
          71 * INT32_C(65536));
    CHECK(records[0].sprite.primitive.material.raw_texture);
    CHECK(!records[0].sprite.primitive.material.semi_transparent);
    CHECK(records[0].sprite.primitive.material.clut_x == 32u);
    CHECK(records[0].sprite.primitive.material.clut_y == 1u);
    CHECK(records[0].sprite.primitive.material.texture_window_mask_x == 1u);
    CHECK(records[0].sprite.primitive.material.texture_window_mask_y == 2u);
    CHECK(records[0].sprite.primitive.material.texture_window_offset_x == 3u);
    CHECK(records[0].sprite.primitive.material.texture_window_offset_y == 4u);

    CHECK(records[1].family == XG_WORLD_ACTOR_SPRITE_SHADOW);
    CHECK(records[1].ordering_bucket == 32u);
    CHECK(records[1].material_word == UINT32_C(0x2c000000));
    CHECK(records[1].sprite.vertices[0].x == 144);
    CHECK(records[1].sprite.vertices[0].y == 120);
    CHECK(records[1].sprite.vertices[3].x == 176);
    CHECK(records[1].sprite.vertices[3].y == 120);
    CHECK(records[1].source_vertices[0].z == -48);
    CHECK(records[1].source_vertices[3].z == 48);
    CHECK(records[1].sprite.primitive.triangles[0].vertices[0].r == 0u);
    CHECK(records[1].sprite.primitive.triangles[0].vertices[1].u ==
          71 * INT32_C(65536));
    CHECK(!records[1].sprite.primitive.material.raw_texture);
    CHECK(!records[1].sprite.primitive.material.semi_transparent);
    CHECK(memcmp(&actor, &actor_before, sizeof(actor)) == 0);
    CHECK(memcmp(&descriptor, &descriptor_before, sizeof(descriptor)) == 0);
    return 1;
}

static int test_part_transform_flips_uv_and_preserves_shadow_geometry(void) {
    XgWorldActorSpriteActorSource actor;
    XgWorldActorSpriteDescriptor descriptor;
    XgWorldActorSpritesValueSource source = fixture(&actor, &descriptor);
    XgWorldActorSpriteRecord records[2];
    uint32_t count = 0u;

    descriptor.part = 2u;
    actor.has_part_transforms = true;
    actor.parts[2].enabled = true;
    set_identity(&actor.parts[2].rotation);
    actor.parts[2].rotation.rotation[0][0] = -4096;
    actor.parts[2].offset_x = 8;
    actor.parts[2].offset_y = 4;
    actor.per_part_ordering = true;
    CHECK(xg_world_actor_sprites_build(&source, records, 2u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(count == 2u);
    CHECK(records[0].ordering_bucket == 30u);
    CHECK(records[0].sprite.vertices[0].x == 180);
    CHECK(records[0].sprite.vertices[0].y == 98);
    CHECK(records[0].sprite.vertices[3].x == 148);
    CHECK(records[0].sprite.vertices[3].y == 146);
    CHECK(records[0].sprite.primitive.triangles[0].vertices[0].u ==
          7 * INT32_C(65536));
    CHECK(records[0].sprite.primitive.triangles[0].vertices[1].u ==
          70 * INT32_C(65536));
    CHECK(records[1].ordering_bucket == 32u);
    CHECK(records[1].sprite.vertices[0].x == 144);
    CHECK(records[1].sprite.vertices[3].x == 176);
    CHECK(records[1].sprite.vertices[0].y == 120);
    CHECK(records[1].sprite.vertices[3].y == 120);
    CHECK(records[1].sprite.primitive.triangles[0].vertices[0].u ==
          8 * INT32_C(65536));
    return 1;
}

static int test_body_and_shadow_keep_distinct_vertical_flip_rules(void) {
    XgWorldActorSpriteActorSource actor;
    XgWorldActorSpriteDescriptor descriptor;
    XgWorldActorSpritesValueSource source = fixture(&actor, &descriptor);
    XgWorldActorSpriteRecord records[2];
    uint32_t count = 0u;

    actor.flip_y = true;
    descriptor.reverse_y = true;
    CHECK(xg_world_actor_sprites_build(&source, records, 2u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(count == 2u);
    CHECK(records[0].sprite.vertices[0].y == 96);
    CHECK(records[0].sprite.vertices[3].y == 144);
    CHECK(records[1].source_vertices[0].z == 48);
    CHECK(records[1].source_vertices[3].z == -48);
    CHECK(records[1].sprite.vertices[0].y == 120);
    CHECK(records[1].sprite.vertices[3].y == 120);
    return 1;
}

#define NATIVE_ACTOR UINT32_C(0x80010000)
#define NATIVE_DATA UINT32_C(0x80010100)
#define NATIVE_DESCRIPTOR UINT32_C(0x80010200)
#define NATIVE_CONTEXT UINT32_C(0x80010300)
#define NATIVE_OT UINT32_C(0x80011000)
#define NATIVE_PACKETS UINT32_C(0x80018000)

static uint8_t native_guest_ram[UINT32_C(0x200000)];

static uint32_t native_physical(uint32_t address) {
    return address & UINT32_C(0x1fffffff);
}

static void native_put_u8(uint32_t address, uint8_t value) {
    native_guest_ram[native_physical(address)] = value;
}

static void native_put_u16(uint32_t address, uint16_t value) {
    const uint32_t physical = native_physical(address);

    native_guest_ram[physical] = (uint8_t)value;
    native_guest_ram[physical + 1u] = (uint8_t)(value >> 8u);
}

static void native_put_u32(uint32_t address, uint32_t value) {
    const uint32_t physical = native_physical(address);

    native_guest_ram[physical] = (uint8_t)value;
    native_guest_ram[physical + 1u] = (uint8_t)(value >> 8u);
    native_guest_ram[physical + 2u] = (uint8_t)(value >> 16u);
    native_guest_ram[physical + 3u] = (uint8_t)(value >> 24u);
}

static bool native_read_u8(void *context, uint32_t address,
                           uint8_t *out_value) {
    const uint32_t physical = native_physical(address);

    (void)context;
    if (out_value == NULL || physical >= sizeof(native_guest_ram)) return false;
    *out_value = native_guest_ram[physical];
    return true;
}

static bool native_read_u16(void *context, uint32_t address,
                            uint16_t *out_value) {
    const uint32_t physical = native_physical(address);

    (void)context;
    if (out_value == NULL || physical > sizeof(native_guest_ram) - 2u)
        return false;
    *out_value = native_guest_ram[physical] |
        ((uint16_t)native_guest_ram[physical + 1u] << 8u);
    return true;
}

static bool native_read_u32(void *context, uint32_t address,
                            uint32_t *out_value) {
    const uint32_t physical = native_physical(address);

    (void)context;
    if (out_value == NULL || physical > sizeof(native_guest_ram) - 4u)
        return false;
    *out_value = native_guest_ram[physical] |
        ((uint32_t)native_guest_ram[physical + 1u] << 8u) |
        ((uint32_t)native_guest_ram[physical + 2u] << 16u) |
        ((uint32_t)native_guest_ram[physical + 3u] << 24u);
    return true;
}

static bool native_authorize_range(
    void *context, XgWorldActorSpritesSourceRangeKind kind,
    uint32_t address, uint32_t size) {
    const uint32_t physical = native_physical(address);

    (void)context;
    (void)kind;
    return size != 0u && physical <= sizeof(native_guest_ram) - size;
}

static void native_put_identity_matrix(uint32_t address, int32_t z) {
    native_put_u32(address, UINT32_C(0x00001000));
    native_put_u32(address + 4u, 0u);
    native_put_u32(address + 8u, UINT32_C(0x00001000));
    native_put_u32(address + 12u, 0u);
    native_put_u32(address + 16u, UINT32_C(0x00001000));
    native_put_u32(address + 20u, 0u);
    native_put_u32(address + 24u, 0u);
    native_put_u32(address + 28u, (uint32_t)z);
}

static void native_prepare_fixture(void) {
    memset(native_guest_ram, 0, sizeof(native_guest_ram));
    native_put_u16(NATIVE_ACTOR + 10u, 512u);
    native_put_u32(NATIVE_ACTOR + 0x20u, NATIVE_DATA);
    native_put_u16(NATIVE_ACTOR + 0x2cu, 4096u);
    native_put_u32(NATIVE_ACTOR + 0x3cu, 4u);
    native_put_u32(NATIVE_ACTOR + 0x40u, 4u);
    native_put_identity_matrix(NATIVE_DATA + 0x0cu, 512);
    native_put_u32(NATIVE_DATA + 0x30u, NATIVE_DESCRIPTOR);
    native_put_u16(NATIVE_DESCRIPTOR, (uint16_t)-32);
    native_put_u16(NATIVE_DESCRIPTOR + 2u, (uint16_t)-48);
    native_put_u8(NATIVE_DESCRIPTOR + 4u, 8u);
    native_put_u8(NATIVE_DESCRIPTOR + 5u, 16u);
    native_put_u8(NATIVE_DESCRIPTOR + 6u, 64u);
    native_put_u8(NATIVE_DESCRIPTOR + 7u, 96u);
    native_put_u16(NATIVE_DESCRIPTOR + 0x0au, 0x0023u);
    native_put_u16(NATIVE_DESCRIPTOR + 0x0cu, 0x0042u);
    native_put_u32(NATIVE_DESCRIPTOR + 0x10u, UINT32_C(0x2d806040));
    native_put_identity_matrix(UINT32_C(0x8004fbb8), 0);
    native_put_u32(UINT32_C(0x8009be3c), NATIVE_CONTEXT);
    native_put_u32(NATIVE_CONTEXT + 0x70u, NATIVE_OT);
    native_put_u32(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR, NATIVE_PACKETS);
    native_put_u32(XG_WORLD_ACTOR_SPRITES_PACKET_LIMIT,
                   NATIVE_PACKETS + 0x100u);
}

static int test_native_prepare_seals_body_shadow_and_compatibility_outputs(void) {
    XgWorldActorSpritesAuthenticatedReader reader = {
        .read_u8 = native_read_u8,
        .read_u16 = native_read_u16,
        .read_u32 = native_read_u32,
        .authorize_source_range = native_authorize_range,
        .authentication_generation = 7u,
        .authenticated = true,
    };
    XgWorldActorSpritesNativeRequest request = {
        .authentication_generation = 7u,
        .resident_entry_pc = XG_WORLD_ACTOR_SPRITES_RESIDENT_ENTRY,
        .prepared_seam_pc = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        .resident_caller_return =
            XG_WORLD_ACTOR_SPRITES_RESIDENT_CALLER_RETURN,
        .actor_address = NATIVE_ACTOR,
        .ordering_table_address = NATIVE_OT + 32u * 4u,
        .screen_offset_x = 160 << 16,
        .screen_offset_y = 120 << 16,
        .projection_distance = 256u,
        .average_z_scale4 = 1024,
        .raster = {
            .draw_area_right = 319u,
            .draw_area_bottom = 239u,
            .texture_window_mask_x = 1u,
            .texture_window_mask_y = 2u,
            .texture_window_offset_x = 3u,
            .texture_window_offset_y = 4u,
        },
        .resident_context_authenticated = true,
        .projection_state_authenticated = true,
    };
    XgWorldActorSpriteRecord records[2];
    XgWorldActorSpritesNativePreparation preparation;

    native_prepare_fixture();
    CHECK(xg_world_actor_sprites_native_prepare(
              &request, &reader, records, 2u, &preparation) ==
          XG_WORLD_ACTOR_SPRITES_NATIVE_OK);
    CHECK(preparation.authenticated && preparation.sealed);
    CHECK(preparation.record_count == 2u);
    CHECK(preparation.body_record_count == 1u);
    CHECK(preparation.shadow_record_count == 1u);
    CHECK(preparation.initial_packet_cursor == NATIVE_PACKETS);
    CHECK(preparation.final_packet_cursor == NATIVE_PACKETS + 0x50u);
    CHECK(preparation.packet_cursor_written);
    CHECK(preparation.continuation_pc ==
          XG_WORLD_ACTOR_SPRITES_CONTINUATION);
    CHECK(preparation.body_scratch.written);
    CHECK(preparation.body_scratch.component_write_mask ==
          (XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_X |
           XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_Y));
    CHECK(preparation.shadow_scratch.written);
    CHECK(preparation.shadow_scratch.component_write_mask ==
          (XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_X |
           XG_WORLD_ACTOR_SPRITE_SCRATCH_WRITE_Z));
    CHECK(records[0].family == XG_WORLD_ACTOR_SPRITE_BODY);
    CHECK(records[0].packet_address == NATIVE_PACKETS);
    CHECK(records[0].ordering_table_address == NATIVE_OT + 32u * 4u);
    CHECK(records[1].family == XG_WORLD_ACTOR_SPRITE_SHADOW);
    CHECK(records[1].packet_address == NATIVE_PACKETS + 0x28u);
    CHECK(records[1].ordering_table_address == NATIVE_OT + 32u * 4u);
    CHECK(records[0].tag_payload_word_count == 9u);
    CHECK(records[0].packet_payload_words[0] == UINT32_C(0x2d806040));
    CHECK(records[1].packet_payload_words[0] == UINT32_C(0x2c000000));
    CHECK(records[0].packet_payload_write_masks[6] ==
          UINT32_C(0x0000ffff));
    CHECK(records[0].packet_payload_write_masks[8] ==
          UINT32_C(0x0000ffff));
    CHECK(records[0].sprite.primitive.material.texture_window_mask_x == 1u);
    CHECK(records[0].sprite.primitive.material.texture_window_mask_y == 2u);
    CHECK(records[0].sprite.primitive.material.texture_window_offset_x == 3u);
    CHECK(records[0].sprite.primitive.material.texture_window_offset_y == 4u);

    native_put_u32(NATIVE_DATA + 0x20u, 1u);
    memset(&preparation, 0xff, sizeof(preparation));
    CHECK(xg_world_actor_sprites_native_prepare(
              &request, &reader, records, 2u, &preparation) ==
          XG_WORLD_ACTOR_SPRITES_NATIVE_SOURCE_MISMATCH);
    CHECK(!preparation.authenticated);
    CHECK(!preparation.sealed);
    return 1;
}

static int test_descriptor_extents_horizontal_flips_and_material_code(void) {
    XgWorldActorSpriteActorSource actor;
    XgWorldActorSpriteDescriptor descriptor;
    XgWorldActorSpritesValueSource source = fixture(&actor, &descriptor);
    XgWorldActorSpritesAdaptedActor adapted;
    XgWorldActorSpriteRecord record;
    uint32_t count = 0u;

    actor.shadow_enabled = false;
    actor.flip_x = true;
    actor.origin_x = 2;
    actor.origin_mirror_x = true;
    descriptor.extent_x_adjust = -4;
    descriptor.extent_y_adjust = -16;
    descriptor.material_word = UINT32_C(0x2e302010);
    CHECK(xg_world_actor_sprites_adapt_actor(&source, 0u, &adapted) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(adapted.body_projection.translation[0] == -2);
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(count == 1u);
    CHECK(record.sprite.vertices[0].x == 175);
    CHECK(record.sprite.vertices[3].x == 145);
    CHECK(record.sprite.vertices[0].y == 96);
    CHECK(record.sprite.vertices[3].y == 136);
    CHECK(record.sprite.primitive.triangles[0].vertices[0].u ==
          7 * INT32_C(65536));
    CHECK(record.sprite.primitive.triangles[0].vertices[0].r == 0x10u);
    CHECK(!record.sprite.primitive.material.raw_texture);
    CHECK(record.sprite.primitive.material.semi_transparent);

    descriptor.reverse_x = true;
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(record.sprite.vertices[0].x == 145);
    CHECK(record.sprite.vertices[3].x == 175);
    CHECK(record.sprite.primitive.triangles[0].vertices[0].u ==
          8 * INT32_C(65536));
    CHECK(record.sprite.primitive.triangles[0].vertices[1].u ==
          71 * INT32_C(65536));
    return 1;
}

static int test_wrap_origin_culling_visibility_and_capacity(void) {
    XgWorldActorSpriteActorSource actor;
    XgWorldActorSpriteDescriptor descriptors[2];
    XgWorldActorSpritesValueSource source = fixture(&actor, &descriptors[0]);
    XgWorldActorSpritesAdaptedActor adapted;
    XgWorldActorSpriteRecord records[4];
    XgWorldActorSpriteRecord zero[4] = { 0 };
    uint32_t count = 99u;

    descriptors[1] = descriptors[0];
    descriptors[1].part = 1u;
    actor.descriptors = descriptors;
    actor.descriptor_count = 2u;
    actor.hidden_part_mask = 2u;
    actor.position[0] = INT32_C(0x04001000);
    source.wrap_x = 16;
    actor.origin_x = 2;
    actor.scale_shift = 1u;
    CHECK(xg_world_actor_sprites_adapt_actor(&source, 0u, &adapted) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(adapted.actor_position.x == -16383);
    CHECK(adapted.body_projection.translation[0] == -16379);
    CHECK(xg_world_actor_sprites_build(&source, records, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_CAPACITY_EXCEEDED);
    CHECK(count == 0u);
    CHECK(memcmp(records, zero, sizeof(records[0])) == 0);

    actor.position[0] = 0;
    actor.position[2] = -(3000 << 12);
    CHECK(xg_world_actor_sprites_build(&source, records, 4u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(count == 0u);
    actor.active = false;
    actor.position[2] = -(512 << 12);
    CHECK(xg_world_actor_sprites_build(&source, records, 4u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(count == 0u);
    actor.active = true;
    actor.sprite_present = false;
    CHECK(xg_world_actor_sprites_build(&source, records, 4u, &count) ==
          XG_WORLD_ACTOR_SPRITES_OK);
    CHECK(count == 0u);
    return 1;
}

static int test_rejects_unbounded_or_unrepresentable_sources(void) {
    XgWorldActorSpriteActorSource actor;
    XgWorldActorSpriteDescriptor descriptor;
    XgWorldActorSpritesValueSource source = fixture(&actor, &descriptor);
    XgWorldActorSpriteRecord record;
    uint32_t count = 0u;

    CHECK(xg_world_actor_sprites_build(NULL, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT);
    CHECK(xg_world_actor_sprites_build(&source, NULL, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT);
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, NULL) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_ARGUMENT);
    actor.descriptor_count =
        XG_WORLD_ACTOR_SPRITES_DESCRIPTOR_CAPACITY + 1u;
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE);
    actor.descriptor_count = 1u;
    descriptor.part = XG_WORLD_ACTOR_SPRITES_PART_CAPACITY;
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE);
    descriptor.part = 0u;
    descriptor.material_word = UINT32_C(0x24806040);
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE);
    descriptor.material_word = UINT32_C(0x2d806040);
    descriptor.tpage = 0x0180u;
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE);
    descriptor.tpage = 0x0023u;
    descriptor.part = 1u;
    actor.per_part_ordering = true;
    actor.position[2] = 0;
    CHECK(xg_world_actor_sprites_build(&source, &record, 1u, &count) ==
          XG_WORLD_ACTOR_SPRITES_INVALID_SOURCE);
    return 1;
}

int main(void) {
    int ok = 1;

    ok &= test_adapts_world_values_and_builds_body_and_shadow();
    ok &= test_part_transform_flips_uv_and_preserves_shadow_geometry();
    ok &= test_body_and_shadow_keep_distinct_vertical_flip_rules();
    ok &= test_descriptor_extents_horizontal_flips_and_material_code();
    ok &= test_wrap_origin_culling_visibility_and_capacity();
    ok &= test_rejects_unbounded_or_unrepresentable_sources();
    ok &= test_native_prepare_seals_body_shadow_and_compatibility_outputs();
    return ok ? 0 : 1;
}
