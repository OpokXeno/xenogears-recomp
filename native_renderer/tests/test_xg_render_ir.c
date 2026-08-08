#include "xg_render_ir.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if XG_RENDER_IR_ITEM_CAPACITY != 4u
#error "test_xg_render_ir.c requires XG_RENDER_IR_ITEM_CAPACITY=4"
#endif

#ifndef XG_RENDER_IR_TESTING
#error "test_xg_render_ir.c requires XG_RENDER_IR_TESTING"
#endif

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

enum {
    FIXTURE_ITEM_COUNT = 3u,
    FIXTURE_SERIALIZED_SIZE = XG_RENDER_IR_SERIALIZED_HEADER_SIZE +
                              XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE +
                              (2u * XG_RENDER_IR_SERIALIZED_NATIVE_SIZE),
};

static GuestRenderVisualStateId fixture_state(void) {
    const GuestRenderVisualStateId state_id = { 17u, 3u };

    return state_id;
}

static XgRenderIrOrdering make_ordering(uint32_t ot_bucket,
                                        uint32_t final_ordinal,
                                        uint32_t packet_guest_address,
                                        uint32_t predecessor_guest_address,
                                        uint32_t successor_guest_address,
                                        uint32_t tag_link_guest_address,
                                        uint8_t tag_payload_word_count) {
    XgRenderIrOrdering ordering = { 0 };

    ordering.ot_bucket = ot_bucket;
    ordering.final_ordinal = final_ordinal;
    ordering.packet_guest_address = packet_guest_address;
    ordering.predecessor_guest_address = predecessor_guest_address;
    ordering.successor_guest_address = successor_guest_address;
    ordering.tag_link_guest_address = tag_link_guest_address;
    ordering.tag_payload_word_count = tag_payload_word_count;
    return ordering;
}

static XgRenderIrMaterialState make_gouraud_material(void) {
    XgRenderIrMaterialState material = { 0 };

    material.tpage = 0x153u;
    material.texture_page_x = 3u;
    material.texture_page_y = 1u;
    material.clut_x = 32u;
    material.clut_y = 7u;
    material.draw_area_left = 5u;
    material.draw_area_top = 6u;
    material.draw_area_right = 900u;
    material.draw_area_bottom = 400u;
    material.draw_offset_x = -17;
    material.draw_offset_y = 23;
    material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
    material.texture_window_mask_x = 1u;
    material.texture_window_mask_y = 2u;
    material.texture_window_offset_x = 3u;
    material.texture_window_offset_y = 4u;
    material.shading = XG_RENDER_IR_SHADING_GOURAUD;
    material.textured = true;
    material.raw_texture = true;
    material.semi_transparent = true;
    material.blend_mode = XG_RENDER_IR_BLEND_SUBTRACT;
    material.dither = true;
    material.mask_set = true;
    material.mask_check = true;
    return material;
}

static XgRenderIrMaterialState make_flat_material(void) {
    XgRenderIrMaterialState material = { 0 };

    material.tpage = 0x102u;
    material.texture_page_x = 2u;
    material.texture_page_y = 0u;
    material.clut_x = 48u;
    material.clut_y = 8u;
    material.draw_area_left = 0u;
    material.draw_area_top = 0u;
    material.draw_area_right = 1023u;
    material.draw_area_bottom = 511u;
    material.draw_offset_x = -1024;
    material.draw_offset_y = 1023;
    material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
    material.texture_window_mask_x = 31u;
    material.texture_window_mask_y = 30u;
    material.texture_window_offset_x = 29u;
    material.texture_window_offset_y = 28u;
    material.shading = XG_RENDER_IR_SHADING_FLAT;
    material.textured = false;
    material.raw_texture = false;
    material.semi_transparent = false;
    material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    material.dither = false;
    material.mask_set = false;
    material.mask_check = false;
    return material;
}

static XgRenderIrTriangle make_triangle(uint8_t split_index,
                                        uint8_t split_count,
                                        uint8_t red,
                                        uint8_t green,
                                        uint8_t blue) {
    XgRenderIrTriangle triangle = { 0 };

    triangle.split_index = split_index;
    triangle.split_count = split_count;
    triangle.vertices[0].x = -98304;
    triangle.vertices[0].y = -131072;
    triangle.vertices[0].u = -65536;
    triangle.vertices[0].v = -32768;
    triangle.vertices[0].r = red;
    triangle.vertices[0].g = green;
    triangle.vertices[0].b = blue;
    triangle.vertices[1].x = -65536;
    triangle.vertices[1].y = -16384;
    triangle.vertices[1].u = -8192;
    triangle.vertices[1].v = -4096;
    triangle.vertices[1].r = red;
    triangle.vertices[1].g = green;
    triangle.vertices[1].b = blue;
    triangle.vertices[2].x = -24576;
    triangle.vertices[2].y = -49152;
    triangle.vertices[2].u = -12288;
    triangle.vertices[2].v = -20480;
    triangle.vertices[2].r = red;
    triangle.vertices[2].g = green;
    triangle.vertices[2].b = blue;
    return triangle;
}

static XgRenderIrCompatibilityItem make_compatibility_item(void) {
    XgRenderIrCompatibilityItem item = { 0 };

    item.base.ordering = make_ordering(100u,
                                       0u,
                                       0x00000100u,
                                       XG_RENDER_IR_NO_PACKET_ADDRESS,
                                       0x00000120u,
                                       0x00000120u,
                                       0u);
    return item;
}

static XgRenderIrNativeItem make_one_triangle_item(GuestRenderVisualStateId state_id) {
    XgRenderIrNativeItem item = { 0 };

    item.base.ordering = make_ordering(7u,
                                       1u,
                                       0x00000120u,
                                       0x00000100u,
                                       0x00000140u,
                                       0x00000140u,
                                       9u);
    item.base.has_provenance = true;
    item.base.provenance_key.state_id = state_id;
    item.base.provenance_key.slot_index = 5u;
    item.base.source_primitive_index = 11u;
    item.native.material = make_gouraud_material();
    item.native.triangle_count = 1u;
    item.native.triangles[0] = make_triangle(0u, 1u, 20u, 40u, 60u);
    item.native.triangles[0].vertices[1].r = 21u;
    item.native.triangles[0].vertices[2].b = 61u;
    return item;
}

static XgRenderIrNativeItem make_two_triangle_item(GuestRenderVisualStateId state_id) {
    XgRenderIrNativeItem item = { 0 };

    item.base.ordering = make_ordering(100u,
                                       2u,
                                       0x00000140u,
                                       0x00000120u,
                                       XG_RENDER_IR_NO_PACKET_ADDRESS,
                                       XG_RENDER_IR_TAG_LINK_END,
                                       5u);
    item.base.has_provenance = true;
    item.base.provenance_key.state_id = state_id;
    item.base.provenance_key.slot_index = 6u;
    item.base.source_primitive_index = 12u;
    item.native.material = make_flat_material();
    item.native.triangle_count = 2u;
    item.native.triangles[0] = make_triangle(0u, 2u, 80u, 90u, 100u);
    item.native.triangles[1] = make_triangle(1u, 2u, 101u, 102u, 103u);
    return item;
}

static void make_fixture_items(GuestRenderVisualStateId state_id,
                               XgRenderIrCompatibilityItem *out_compatibility,
                               XgRenderIrNativeItem *out_one_triangle,
                               XgRenderIrNativeItem *out_two_triangle) {
    *out_compatibility = make_compatibility_item();
    *out_one_triangle = make_one_triangle_item(state_id);
    *out_two_triangle = make_two_triangle_item(state_id);
}

static int ordering_equal(const XgRenderIrOrdering *left,
                          const XgRenderIrOrdering *right) {
    return left->ot_bucket == right->ot_bucket &&
           left->final_ordinal == right->final_ordinal &&
           left->packet_guest_address == right->packet_guest_address &&
           left->predecessor_guest_address == right->predecessor_guest_address &&
           left->successor_guest_address == right->successor_guest_address &&
           left->tag_link_guest_address == right->tag_link_guest_address &&
           left->tag_payload_word_count == right->tag_payload_word_count;
}

static int item_base_equal(const XgRenderIrItemBase *left,
                           const XgRenderIrItemBase *right) {
    return ordering_equal(&left->ordering, &right->ordering) &&
           left->has_provenance == right->has_provenance &&
           left->provenance_key.state_id.scene_epoch ==
                   right->provenance_key.state_id.scene_epoch &&
           left->provenance_key.state_id.state_sequence ==
                   right->provenance_key.state_id.state_sequence &&
           left->provenance_key.slot_index == right->provenance_key.slot_index &&
           left->source_primitive_index == right->source_primitive_index;
}

static int material_equal(const XgRenderIrMaterialState *left,
                          const XgRenderIrMaterialState *right) {
    return left->tpage == right->tpage &&
           left->texture_page_x == right->texture_page_x &&
           left->texture_page_y == right->texture_page_y &&
           left->clut_x == right->clut_x &&
           left->clut_y == right->clut_y &&
           left->draw_area_left == right->draw_area_left &&
           left->draw_area_top == right->draw_area_top &&
           left->draw_area_right == right->draw_area_right &&
           left->draw_area_bottom == right->draw_area_bottom &&
           left->draw_offset_x == right->draw_offset_x &&
           left->draw_offset_y == right->draw_offset_y &&
           left->texture_depth == right->texture_depth &&
           left->texture_window_mask_x == right->texture_window_mask_x &&
           left->texture_window_mask_y == right->texture_window_mask_y &&
           left->texture_window_offset_x == right->texture_window_offset_x &&
           left->texture_window_offset_y == right->texture_window_offset_y &&
           left->shading == right->shading &&
           left->textured == right->textured &&
           left->raw_texture == right->raw_texture &&
           left->semi_transparent == right->semi_transparent &&
           left->blend_mode == right->blend_mode &&
           left->dither == right->dither &&
           left->mask_set == right->mask_set &&
           left->mask_check == right->mask_check;
}

static int triangle_equal(const XgRenderIrTriangle *left,
                          const XgRenderIrTriangle *right) {
    size_t vertex_index;

    if (left->split_index != right->split_index ||
        left->split_count != right->split_count)
        return 0;
    for (vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
        const XgRenderIrVertex *left_vertex = &left->vertices[vertex_index];
        const XgRenderIrVertex *right_vertex = &right->vertices[vertex_index];

        if (left_vertex->x != right_vertex->x ||
            left_vertex->y != right_vertex->y ||
            left_vertex->u != right_vertex->u ||
            left_vertex->v != right_vertex->v ||
            left_vertex->r != right_vertex->r ||
            left_vertex->g != right_vertex->g ||
            left_vertex->b != right_vertex->b)
            return 0;
    }
    return 1;
}

static int native_equal(const XgRenderIrNativePrimitive *left,
                        const XgRenderIrNativePrimitive *right) {
    size_t triangle_index;

    if (!material_equal(&left->material, &right->material) ||
        left->triangle_count != right->triangle_count)
        return 0;
    for (triangle_index = 0u; triangle_index < XG_RENDER_IR_TRIANGLE_CAPACITY;
         ++triangle_index) {
        if (!triangle_equal(&left->triangles[triangle_index],
                            &right->triangles[triangle_index]))
            return 0;
    }
    return 1;
}

static int compatibility_matches(const XgRenderIrItem *item,
                                 const XgRenderIrCompatibilityItem *expected) {
    return item->kind == XG_RENDER_IR_ITEM_COMPATIBILITY &&
           item_base_equal(&item->base, &expected->base);
}

static int native_matches(const XgRenderIrItem *item,
                          const XgRenderIrNativeItem *expected) {
    return item->kind == XG_RENDER_IR_ITEM_NATIVE &&
           item_base_equal(&item->base, &expected->base) &&
           native_equal(&item->native, &expected->native);
}

static int append_fixture(XgRenderIr *ir,
                          const XgRenderIrCompatibilityItem *compatibility,
                          const XgRenderIrNativeItem *one_triangle,
                          const XgRenderIrNativeItem *two_triangle) {
    CHECK(xg_render_ir_append_compatibility(ir, compatibility) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_native(ir, one_triangle) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_native(ir, two_triangle) == XG_RENDER_IR_OK);
    return 1;
}

static int begin_appended_fixture(XgRenderIr *ir,
                                  GuestRenderVisualStateId state_id,
                                  uint64_t vram_mutation_serial,
                                  XgRenderIrCompatibilityItem *out_compatibility,
                                  XgRenderIrNativeItem *out_one_triangle,
                                  XgRenderIrNativeItem *out_two_triangle) {
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    make_fixture_items(state_id, out_compatibility, out_one_triangle, out_two_triangle);
    CHECK(xg_render_ir_begin(ir, state_id, vram_mutation_serial) == XG_RENDER_IR_OK);
    CHECK(append_fixture(ir, out_compatibility, out_one_triangle, out_two_triangle));
    return 1;
}

static int rejected_transaction(XgRenderIr *ir, XgRenderIrRejectReason reason) {
    XgRenderIrSnapshot snapshot = { 0 };
    XgRenderIrItem item = { 0 };
    size_t serialized_size = 0u;
    uint8_t serialized[1] = { 0 };

    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == XG_RENDER_IR_REJECTED);
    CHECK(snapshot.reject_reason == reason);
    CHECK(!snapshot.usable);
    CHECK(xg_render_ir_item_get(ir, 0u, &item) == XG_RENDER_IR_TRANSACTION_REJECTED);
    CHECK(xg_render_ir_serialized_size(ir, &serialized_size) ==
          XG_RENDER_IR_TRANSACTION_REJECTED);
    CHECK(xg_render_ir_serialize_normalized(ir, serialized, sizeof(serialized),
                                            &serialized_size) ==
          XG_RENDER_IR_TRANSACTION_REJECTED);
    return 1;
}

static int bytes_are_zero(const uint8_t *bytes, size_t length) {
    size_t index;

    for (index = 0u; index < length; ++index) {
        if (bytes[index] != 0u) return 0;
    }
    return 1;
}

static int bytes_are_value(const uint8_t *bytes, size_t length, uint8_t value) {
    size_t index;

    for (index = 0u; index < length; ++index) {
        if (bytes[index] != value) return 0;
    }
    return 1;
}

static void put_u16_le(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void put_u32_le(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void put_u64_le(uint8_t *bytes, uint64_t value) {
    size_t index;

    for (index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static void put_i32_le(uint8_t *bytes, int32_t value) {
    put_u32_le(bytes, (uint32_t)value);
}

static void write_common_record(uint8_t *record,
                                XgRenderIrItemKind kind,
                                uint16_t record_size,
                                const XgRenderIrItemBase *base) {
    record[0] = (uint8_t)kind;
    record[1] = base->has_provenance ? 1u : 0u;
    put_u16_le(record + 2u, record_size);
    put_u32_le(record + 4u, base->ordering.final_ordinal);
    put_u32_le(record + 8u, base->ordering.ot_bucket);
    put_u32_le(record + 12u, base->ordering.packet_guest_address);
    put_u32_le(record + 16u, base->ordering.predecessor_guest_address);
    put_u32_le(record + 20u, base->ordering.successor_guest_address);
    put_u32_le(record + 24u, base->ordering.tag_link_guest_address);
    record[28] = base->ordering.tag_payload_word_count;
    put_u64_le(record + 32u, base->provenance_key.state_id.scene_epoch);
    put_u64_le(record + 40u, base->provenance_key.state_id.state_sequence);
    put_u64_le(record + 48u, (uint64_t)base->provenance_key.slot_index);
    put_u32_le(record + 56u, base->source_primitive_index);
}

static void write_material_record(uint8_t *record,
                                  const XgRenderIrMaterialState *material) {
    put_u16_le(record + 0u, material->tpage);
    put_u16_le(record + 2u, material->texture_page_x);
    put_u16_le(record + 4u, material->texture_page_y);
    put_u16_le(record + 6u, material->clut_x);
    put_u16_le(record + 8u, material->clut_y);
    put_u16_le(record + 10u, material->draw_area_left);
    put_u16_le(record + 12u, material->draw_area_top);
    put_u16_le(record + 14u, material->draw_area_right);
    put_u16_le(record + 16u, material->draw_area_bottom);
    put_u16_le(record + 18u, (uint16_t)material->draw_offset_x);
    put_u16_le(record + 20u, (uint16_t)material->draw_offset_y);
    record[22] = (uint8_t)material->texture_depth;
    record[23] = material->texture_window_mask_x;
    record[24] = material->texture_window_mask_y;
    record[25] = material->texture_window_offset_x;
    record[26] = material->texture_window_offset_y;
    record[27] = (uint8_t)material->shading;
    record[28] = material->textured ? 1u : 0u;
    record[29] = material->raw_texture ? 1u : 0u;
    record[30] = material->semi_transparent ? 1u : 0u;
    record[31] = (uint8_t)material->blend_mode;
    record[32] = material->dither ? 1u : 0u;
    record[33] = material->mask_set ? 1u : 0u;
    record[34] = material->mask_check ? 1u : 0u;
}

static void write_triangle_record(uint8_t *record, const XgRenderIrTriangle *triangle) {
    size_t vertex_index;

    record[0] = triangle->split_index;
    record[1] = triangle->split_count;
    for (vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
        const XgRenderIrVertex *vertex = &triangle->vertices[vertex_index];
        uint8_t *serialized_vertex = record + 4u + (vertex_index * 20u);

        put_i32_le(serialized_vertex + 0u, vertex->x);
        put_i32_le(serialized_vertex + 4u, vertex->y);
        put_i32_le(serialized_vertex + 8u, vertex->u);
        put_i32_le(serialized_vertex + 12u, vertex->v);
        serialized_vertex[16] = vertex->r;
        serialized_vertex[17] = vertex->g;
        serialized_vertex[18] = vertex->b;
    }
}

static void write_expected_serialization(uint8_t *bytes,
                                         GuestRenderVisualStateId state_id,
                                         uint64_t vram_mutation_serial,
                                         const XgRenderIrCompatibilityItem *compatibility,
                                         const XgRenderIrNativeItem *one_triangle,
                                         const XgRenderIrNativeItem *two_triangle) {
    uint8_t *first_native;
    uint8_t *second_native;

    memset(bytes, 0, FIXTURE_SERIALIZED_SIZE);
    bytes[0] = 'X';
    bytes[1] = 'G';
    bytes[2] = 'I';
    bytes[3] = 'R';
    put_u16_le(bytes + 4u, XG_RENDER_IR_SERIALIZATION_VERSION);
    put_u16_le(bytes + 6u, XG_RENDER_IR_SERIALIZED_HEADER_SIZE);
    put_u64_le(bytes + 8u, state_id.scene_epoch);
    put_u64_le(bytes + 16u, state_id.state_sequence);
    put_u64_le(bytes + 24u, vram_mutation_serial);
    put_u32_le(bytes + 32u, FIXTURE_ITEM_COUNT);
    put_u32_le(bytes + 36u, FIXTURE_SERIALIZED_SIZE);

    write_common_record(bytes + XG_RENDER_IR_SERIALIZED_HEADER_SIZE,
                        XG_RENDER_IR_ITEM_COMPATIBILITY,
                        XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE,
                        &compatibility->base);

    first_native = bytes + XG_RENDER_IR_SERIALIZED_HEADER_SIZE +
                   XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE;
    write_common_record(first_native,
                        XG_RENDER_IR_ITEM_NATIVE,
                        XG_RENDER_IR_SERIALIZED_NATIVE_SIZE,
                        &one_triangle->base);
    write_material_record(first_native + 64u, &one_triangle->native.material);
    first_native[104] = one_triangle->native.triangle_count;
    write_triangle_record(first_native + 112u, &one_triangle->native.triangles[0]);

    second_native = first_native + XG_RENDER_IR_SERIALIZED_NATIVE_SIZE;
    write_common_record(second_native,
                        XG_RENDER_IR_ITEM_NATIVE,
                        XG_RENDER_IR_SERIALIZED_NATIVE_SIZE,
                        &two_triangle->base);
    write_material_record(second_native + 64u, &two_triangle->native.material);
    second_native[104] = two_triangle->native.triangle_count;
    write_triangle_record(second_native + 112u, &two_triangle->native.triangles[0]);
    write_triangle_record(second_native + 176u, &two_triangle->native.triangles[1]);
}

static int test_owner_reset_and_begin_contract(void) {
    XgRenderIr *ir = NULL;
    XgRenderIr *same_ir = NULL;
    XgRenderIrSnapshot snapshot = { 0 };
    XgRenderIrItem item = { 0 };
    GuestRenderVisualStateId invalid_state = { 0u, 3u };
    GuestRenderVisualStateId valid_state = fixture_state();
    size_t serialized_size = 0u;
    uint8_t serialized[1] = { 0 };

    CHECK(XG_RENDER_IR_FIXED_FRACTION_BITS == 16);
    CHECK(XG_RENDER_IR_TRIANGLE_CAPACITY == 2u);
    CHECK(XG_RENDER_IR_SERIALIZATION_VERSION == 1u);
    CHECK(XG_RENDER_IR_SERIALIZED_HEADER_SIZE == 40u);
    CHECK(XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE == 64u);
    CHECK(XG_RENDER_IR_SERIALIZED_NATIVE_SIZE == 240u);
    CHECK(XG_RENDER_IR_TAG_LINK_END == 0x00ffffffu);
    CHECK(xg_render_ir_process_owner(NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_process_owner(&same_ir) == XG_RENDER_IR_OK);
    CHECK(ir == same_ir);
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_item_capacity() == 4u);
    CHECK(xg_render_ir_begin(ir, invalid_state, 44u) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == XG_RENDER_IR_EMPTY);
    CHECK(snapshot.item_count == 0u);
    CHECK(snapshot.state_id.scene_epoch == 0u);
    CHECK(snapshot.state_id.state_sequence == 0u);
    CHECK(snapshot.vram_mutation_serial == 0u);
    CHECK(!snapshot.usable);
    CHECK(xg_render_ir_begin(ir, valid_state, 44u) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == XG_RENDER_IR_BUILDING);
    CHECK(snapshot.state_id.scene_epoch == valid_state.scene_epoch);
    CHECK(snapshot.state_id.state_sequence == valid_state.state_sequence);
    CHECK(snapshot.vram_mutation_serial == 44u);
    CHECK(snapshot.item_count == 0u);
    CHECK(snapshot.compatibility_count == 0u);
    CHECK(snapshot.native_count == 0u);
    CHECK(snapshot.reject_reason == XG_RENDER_IR_REJECT_NONE);
    CHECK(!snapshot.usable);
    CHECK(xg_render_ir_item_get(ir, 0u, &item) == XG_RENDER_IR_NOT_FINALIZED);
    CHECK(xg_render_ir_serialized_size(ir, &serialized_size) == XG_RENDER_IR_NOT_FINALIZED);
    CHECK(xg_render_ir_serialize_normalized(ir, serialized, sizeof(serialized),
                                            &serialized_size) ==
          XG_RENDER_IR_NOT_FINALIZED);
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == XG_RENDER_IR_EMPTY);
    CHECK(snapshot.item_count == 0u);
    CHECK(snapshot.compatibility_count == 0u);
    CHECK(snapshot.native_count == 0u);
    CHECK(snapshot.state_id.scene_epoch == 0u);
    CHECK(snapshot.state_id.state_sequence == 0u);
    CHECK(snapshot.vram_mutation_serial == 0u);
    CHECK(snapshot.reject_reason == XG_RENDER_IR_REJECT_NONE);
    CHECK(!snapshot.usable);
    CHECK(xg_render_ir_test_stored_item_get(ir, 0u, &item) == XG_RENDER_IR_OUT_OF_RANGE);
    return 1;
}

static int test_order_provenance_copy_and_finalize(void) {
    XgRenderIr *ir = NULL;
    GuestRenderVisualStateId state_id = fixture_state();
    XgRenderIrCompatibilityItem compatibility = { 0 };
    XgRenderIrNativeItem one_triangle = { 0 };
    XgRenderIrNativeItem two_triangle = { 0 };
    XgRenderIrCompatibilityItem expected_compatibility = { 0 };
    XgRenderIrNativeItem expected_one_triangle = { 0 };
    XgRenderIrNativeItem expected_two_triangle = { 0 };
    XgRenderIrSnapshot snapshot = { 0 };
    XgRenderIrItem first = { 0 };
    XgRenderIrItem second = { 0 };
    XgRenderIrItem third = { 0 };

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    make_fixture_items(state_id, &compatibility, &one_triangle, &two_triangle);
    expected_compatibility = compatibility;
    expected_one_triangle = one_triangle;
    expected_two_triangle = two_triangle;
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, state_id, 77u) == XG_RENDER_IR_OK);
    CHECK(append_fixture(ir, &compatibility, &one_triangle, &two_triangle));

    compatibility.base.ordering.ot_bucket = 99u;
    compatibility.base.provenance_key.slot_index = 99u;
    one_triangle.native.material.tpage = 0u;
    one_triangle.native.triangles[0].vertices[0].x = 0;
    two_triangle.base.source_primitive_index = 0u;
    two_triangle.native.triangles[1].split_index = 0u;

    CHECK(xg_render_ir_finalize(ir, state_id, 77u) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == XG_RENDER_IR_FINALIZED);
    CHECK(snapshot.usable);
    CHECK(snapshot.reject_reason == XG_RENDER_IR_REJECT_NONE);
    CHECK(snapshot.state_id.scene_epoch == state_id.scene_epoch);
    CHECK(snapshot.state_id.state_sequence == state_id.state_sequence);
    CHECK(snapshot.vram_mutation_serial == 77u);
    CHECK(snapshot.item_count == FIXTURE_ITEM_COUNT);
    CHECK(snapshot.compatibility_count == 1u);
    CHECK(snapshot.native_count == 2u);
    CHECK(xg_render_ir_item_get(ir, 0u, &first) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_item_get(ir, 1u, &second) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_item_get(ir, 2u, &third) == XG_RENDER_IR_OK);
    CHECK(compatibility_matches(&first, &expected_compatibility));
    CHECK(native_matches(&second, &expected_one_triangle));
    CHECK(native_matches(&third, &expected_two_triangle));

    CHECK(first.base.ordering.ot_bucket == 100u);
    CHECK(second.base.ordering.ot_bucket == 7u);
    CHECK(third.base.ordering.ot_bucket == 100u);
    CHECK(first.base.ordering.final_ordinal == 0u);
    CHECK(second.base.ordering.final_ordinal == 1u);
    CHECK(third.base.ordering.final_ordinal == 2u);
    CHECK((first.base.ordering.packet_guest_address & 3u) == 0u);
    CHECK((second.base.ordering.packet_guest_address & 3u) == 0u);
    CHECK((third.base.ordering.packet_guest_address & 3u) == 0u);
    CHECK(first.base.ordering.packet_guest_address !=
          second.base.ordering.packet_guest_address);
    CHECK(second.base.ordering.packet_guest_address !=
          third.base.ordering.packet_guest_address);
    CHECK(first.base.ordering.predecessor_guest_address ==
          XG_RENDER_IR_NO_PACKET_ADDRESS);
    CHECK(first.base.ordering.successor_guest_address ==
          second.base.ordering.packet_guest_address);
    CHECK(first.base.ordering.tag_link_guest_address ==
          second.base.ordering.packet_guest_address);
    CHECK(second.base.ordering.predecessor_guest_address ==
          first.base.ordering.packet_guest_address);
    CHECK(second.base.ordering.successor_guest_address ==
          third.base.ordering.packet_guest_address);
    CHECK(second.base.ordering.tag_link_guest_address ==
          third.base.ordering.packet_guest_address);
    CHECK(third.base.ordering.predecessor_guest_address ==
          second.base.ordering.packet_guest_address);
    CHECK(third.base.ordering.successor_guest_address ==
          XG_RENDER_IR_NO_PACKET_ADDRESS);
    CHECK(third.base.ordering.tag_link_guest_address == XG_RENDER_IR_TAG_LINK_END);
    CHECK(first.base.ordering.tag_payload_word_count == 0u);
    CHECK(!first.base.has_provenance);
    CHECK(first.base.provenance_key.state_id.scene_epoch == 0u);
    CHECK(first.base.provenance_key.state_id.state_sequence == 0u);
    CHECK(first.base.provenance_key.slot_index == 0u);
    CHECK(first.base.source_primitive_index == 0u);
    CHECK(second.base.has_provenance);
    CHECK(second.base.provenance_key.state_id.scene_epoch == state_id.scene_epoch);
    CHECK(second.base.provenance_key.state_id.state_sequence == state_id.state_sequence);
    CHECK(second.base.provenance_key.slot_index == 5u);
    CHECK(second.base.source_primitive_index == 11u);
    CHECK(third.base.has_provenance);
    CHECK(third.base.provenance_key.state_id.scene_epoch == state_id.scene_epoch);
    CHECK(third.base.provenance_key.state_id.state_sequence == state_id.state_sequence);
    CHECK(third.base.provenance_key.slot_index == 6u);
    CHECK(third.base.source_primitive_index == 12u);
    CHECK(second.native.triangle_count == 1u);
    CHECK(second.native.triangles[0].split_index == 0u);
    CHECK(second.native.triangles[0].split_count == 1u);
    CHECK(second.native.triangles[0].vertices[0].x < 0);
    CHECK(second.native.triangles[0].vertices[0].y < 0);
    CHECK(second.native.triangles[0].vertices[0].u < 0);
    CHECK(second.native.triangles[0].vertices[0].v < 0);
    CHECK(third.native.triangle_count == 2u);
    CHECK(third.native.triangles[0].split_index == 0u);
    CHECK(third.native.triangles[0].split_count == 2u);
    CHECK(third.native.triangles[1].split_index == 1u);
    CHECK(third.native.triangles[1].split_count == 2u);
    return 1;
}

static int expect_invalid_material(XgRenderIr *ir, XgRenderIrNativeItem invalid_item) {
    GuestRenderVisualStateId state_id = fixture_state();
    XgRenderIrSnapshot snapshot = { 0 };

    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, state_id, 101u) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_append_native(ir, &invalid_item) == XG_RENDER_IR_INVALID_ITEM);
    CHECK(rejected_transaction(ir, XG_RENDER_IR_REJECT_INVALID_ITEM));
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.item_count == 0u);
    return 1;
}

static int test_invalid_materials_reject_whole_transaction(void) {
    XgRenderIr *ir = NULL;
    GuestRenderVisualStateId state_id = fixture_state();
    XgRenderIrNativeItem item = make_one_triangle_item(state_id);

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    item.native.material.tpage = 0x0200u;
    CHECK(expect_invalid_material(ir, item));

    item = make_one_triangle_item(state_id);
    item.native.material.tpage = 0x0180u;
    item.native.material.texture_page_x = 0u;
    item.native.material.texture_page_y = 0u;
    item.native.material.blend_mode = XG_RENDER_IR_BLEND_AVERAGE;
    item.native.material.texture_depth = (XgRenderIrTextureDepth)3;
    CHECK(expect_invalid_material(ir, item));

    item = make_one_triangle_item(state_id);
    item.native.material.clut_x = 18u;
    CHECK(expect_invalid_material(ir, item));

    item = make_one_triangle_item(state_id);
    item.native.material.texture_window_mask_x = 32u;
    CHECK(expect_invalid_material(ir, item));

    item = make_one_triangle_item(state_id);
    item.native.material.draw_area_right = 1024u;
    CHECK(expect_invalid_material(ir, item));

    item = make_one_triangle_item(state_id);
    item.native.material.draw_offset_x = 1024;
    CHECK(expect_invalid_material(ir, item));

    item = make_one_triangle_item(state_id);
    item.native.material.textured = false;
    item.native.material.raw_texture = true;
    CHECK(expect_invalid_material(ir, item));

    item = make_one_triangle_item(state_id);
    item.native.material.shading = XG_RENDER_IR_SHADING_FLAT;
    item.native.triangles[0].vertices[1].r++;
    CHECK(expect_invalid_material(ir, item));
    return 1;
}

static int test_capacity_rejects_whole_transaction(void) {
    XgRenderIr *ir = NULL;
    GuestRenderVisualStateId state_id = fixture_state();
    size_t index;

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, state_id, 102u) == XG_RENDER_IR_OK);
    for (index = 0u; index < XG_RENDER_IR_ITEM_CAPACITY; ++index) {
        XgRenderIrCompatibilityItem item = { 0 };
        const uint32_t packet_address = 0x00000200u + (uint32_t)(index * 4u);
        const uint32_t predecessor = index == 0u ? XG_RENDER_IR_NO_PACKET_ADDRESS :
                                                     packet_address - 4u;
        const uint32_t successor = index + 1u == XG_RENDER_IR_ITEM_CAPACITY ?
                                       XG_RENDER_IR_NO_PACKET_ADDRESS : packet_address + 4u;
        const uint32_t tag_link = index + 1u == XG_RENDER_IR_ITEM_CAPACITY ?
                                      XG_RENDER_IR_TAG_LINK_END : packet_address + 4u;

        item.base.ordering = make_ordering((uint32_t)(index & 1u ? 7u : 100u),
                                           (uint32_t)index,
                                           packet_address,
                                           predecessor,
                                           successor,
                                           tag_link,
                                           0u);
        CHECK(xg_render_ir_append_compatibility(ir, &item) == XG_RENDER_IR_OK);
    }
    {
        XgRenderIrCompatibilityItem excess = { 0 };

        excess.base.ordering = make_ordering(100u,
                                              XG_RENDER_IR_ITEM_CAPACITY,
                                              0x00000210u,
                                              0x0000020cu,
                                              XG_RENDER_IR_NO_PACKET_ADDRESS,
                                              XG_RENDER_IR_TAG_LINK_END,
                                              0u);
        CHECK(xg_render_ir_append_compatibility(ir, &excess) ==
              XG_RENDER_IR_CAPACITY_EXCEEDED);
    }
    CHECK(rejected_transaction(ir, XG_RENDER_IR_REJECT_CAPACITY));
    return 1;
}

static int test_finalize_rejections(void) {
    XgRenderIr *ir = NULL;
    GuestRenderVisualStateId state_id = fixture_state();
    GuestRenderVisualStateId wrong_state = fixture_state();
    XgRenderIrCompatibilityItem compatibility = { 0 };
    XgRenderIrNativeItem one_triangle = { 0 };
    XgRenderIrNativeItem two_triangle = { 0 };

    wrong_state.state_sequence++;
    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(begin_appended_fixture(ir, state_id, 103u, &compatibility,
                                 &one_triangle, &two_triangle));
    CHECK(xg_render_ir_finalize(ir, wrong_state, 103u) ==
          XG_RENDER_IR_INVALID_TRANSITION);
    CHECK(rejected_transaction(ir, XG_RENDER_IR_REJECT_VISUAL_STATE_MISMATCH));

    CHECK(begin_appended_fixture(ir, state_id, 104u, &compatibility,
                                 &one_triangle, &two_triangle));
    CHECK(xg_render_ir_finalize(ir, state_id, 105u) == XG_RENDER_IR_STALE_VRAM_SERIAL);
    CHECK(rejected_transaction(ir, XG_RENDER_IR_REJECT_STALE_VRAM_SERIAL));

    make_fixture_items(state_id, &compatibility, &one_triangle, &two_triangle);
    compatibility.base.ordering.final_ordinal = 3u;
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, state_id, 106u) == XG_RENDER_IR_OK);
    CHECK(append_fixture(ir, &compatibility, &one_triangle, &two_triangle));
    CHECK(xg_render_ir_finalize(ir, state_id, 106u) == XG_RENDER_IR_INVALID_ORDER);
    CHECK(rejected_transaction(ir, XG_RENDER_IR_REJECT_INVALID_ORDER));

    make_fixture_items(state_id, &compatibility, &one_triangle, &two_triangle);
    compatibility.base.ordering.successor_guest_address = 0x00000124u;
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, state_id, 107u) == XG_RENDER_IR_OK);
    CHECK(append_fixture(ir, &compatibility, &one_triangle, &two_triangle));
    CHECK(xg_render_ir_finalize(ir, state_id, 107u) == XG_RENDER_IR_INVALID_LINKAGE);
    CHECK(rejected_transaction(ir, XG_RENDER_IR_REJECT_INVALID_LINKAGE));
    return 1;
}

static int test_post_finalize_mutation_preserves_stored_items(void) {
    XgRenderIr *ir = NULL;
    GuestRenderVisualStateId state_id = fixture_state();
    XgRenderIrCompatibilityItem compatibility = { 0 };
    XgRenderIrNativeItem one_triangle = { 0 };
    XgRenderIrNativeItem two_triangle = { 0 };
    XgRenderIrCompatibilityItem mutation = make_compatibility_item();
    XgRenderIrItem before = { 0 };
    XgRenderIrItem after = { 0 };
    XgRenderIrSnapshot snapshot = { 0 };
    size_t serialized_size = 0u;

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(begin_appended_fixture(ir, state_id, 108u, &compatibility,
                                 &one_triangle, &two_triangle));
    CHECK(xg_render_ir_finalize(ir, state_id, 108u) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_test_stored_item_get(ir, 1u, &before) == XG_RENDER_IR_OK);
    mutation.base.ordering.final_ordinal = 3u;
    mutation.base.ordering.packet_guest_address = 0x00000160u;
    mutation.base.ordering.predecessor_guest_address = 0x00000140u;
    CHECK(xg_render_ir_append_compatibility(ir, &mutation) == XG_RENDER_IR_FROZEN);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == XG_RENDER_IR_REJECTED);
    CHECK(snapshot.reject_reason == XG_RENDER_IR_REJECT_POST_FINALIZE_MUTATION);
    CHECK(!snapshot.usable);
    CHECK(snapshot.item_count == FIXTURE_ITEM_COUNT);
    CHECK(xg_render_ir_item_get(ir, 1u, &after) == XG_RENDER_IR_TRANSACTION_REJECTED);
    CHECK(xg_render_ir_serialized_size(ir, &serialized_size) ==
          XG_RENDER_IR_TRANSACTION_REJECTED);
    CHECK(xg_render_ir_test_stored_item_get(ir, 1u, &after) == XG_RENDER_IR_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    return 1;
}

static int test_null_range_and_buffer_failures_do_not_poison(void) {
    XgRenderIr *ir = NULL;
    GuestRenderVisualStateId state_id = fixture_state();
    XgRenderIrCompatibilityItem compatibility = { 0 };
    XgRenderIrNativeItem one_triangle = { 0 };
    XgRenderIrNativeItem two_triangle = { 0 };
    XgRenderIrSnapshot before = { 0 };
    XgRenderIrSnapshot after = { 0 };
    XgRenderIrItem item = { 0 };
    size_t required_size = 0u;
    uint8_t undersized[FIXTURE_SERIALIZED_SIZE] = { 0 };

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_reset(NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_begin(NULL, state_id, 109u) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_append_compatibility(NULL, &compatibility) ==
          XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_append_native(NULL, &one_triangle) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_finalize(NULL, state_id, 109u) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_snapshot(NULL, &before) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_item_get(NULL, 0u, &item) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_serialized_size(NULL, &required_size) ==
          XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_serialize_normalized(NULL, undersized, sizeof(undersized),
                                            &required_size) ==
          XG_RENDER_IR_INVALID_ARGUMENT);

    CHECK(begin_appended_fixture(ir, state_id, 109u, &compatibility,
                                 &one_triangle, &two_triangle));
    CHECK(xg_render_ir_append_compatibility(ir, NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_append_native(ir, NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_finalize(ir, state_id, 109u) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, &before) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_item_get(ir, FIXTURE_ITEM_COUNT, &item) ==
          XG_RENDER_IR_OUT_OF_RANGE);
    CHECK(xg_render_ir_item_get(ir, 0u, NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_serialized_size(ir, NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_serialized_size(ir, &required_size) == XG_RENDER_IR_OK);
    memset(undersized, 0xa5, sizeof(undersized));
    CHECK(xg_render_ir_serialize_normalized(ir, undersized, required_size - 1u,
                                            &required_size) ==
          XG_RENDER_IR_BUFFER_TOO_SMALL);
    CHECK(required_size == FIXTURE_SERIALIZED_SIZE);
    CHECK(bytes_are_value(undersized, sizeof(undersized), 0xa5u));
    CHECK(xg_render_ir_serialize_normalized(ir, NULL, required_size,
                                            &required_size) ==
          XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_serialize_normalized(ir, undersized, required_size,
                                            NULL) == XG_RENDER_IR_INVALID_ARGUMENT);
    CHECK(xg_render_ir_snapshot(ir, &after) == XG_RENDER_IR_OK);
    CHECK(after.phase == XG_RENDER_IR_FINALIZED);
    CHECK(after.usable);
    CHECK(after.reject_reason == XG_RENDER_IR_REJECT_NONE);
    CHECK(after.item_count == before.item_count);
    CHECK(after.vram_mutation_serial == before.vram_mutation_serial);
    return 1;
}

static int test_normalized_serialization_and_reset(void) {
    XgRenderIr *ir = NULL;
    GuestRenderVisualStateId state_id = fixture_state();
    XgRenderIrCompatibilityItem compatibility = { 0 };
    XgRenderIrNativeItem one_triangle = { 0 };
    XgRenderIrNativeItem two_triangle = { 0 };
    XgRenderIrSnapshot snapshot = { 0 };
    XgRenderIrItem item = { 0 };
    uint8_t expected[FIXTURE_SERIALIZED_SIZE] = { 0 };
    uint8_t first[FIXTURE_SERIALIZED_SIZE] = { 0 };
    uint8_t second[FIXTURE_SERIALIZED_SIZE] = { 0 };
    size_t required_size = 0u;
    size_t written_size = 0u;
    const size_t first_native_offset = XG_RENDER_IR_SERIALIZED_HEADER_SIZE +
                                       XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE;

    CHECK(xg_render_ir_process_owner(&ir) == XG_RENDER_IR_OK);
    make_fixture_items(state_id, &compatibility, &one_triangle, &two_triangle);
    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_begin(ir, state_id, 110u) == XG_RENDER_IR_OK);
    CHECK(append_fixture(ir, &compatibility, &one_triangle, &two_triangle));
    CHECK(xg_render_ir_finalize(ir, state_id, 110u) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_serialized_size(ir, &required_size) == XG_RENDER_IR_OK);
    CHECK(required_size == FIXTURE_SERIALIZED_SIZE);
    CHECK(xg_render_ir_serialize_normalized(ir, first, sizeof(first), &written_size) ==
          XG_RENDER_IR_OK);
    CHECK(written_size == FIXTURE_SERIALIZED_SIZE);
    write_expected_serialization(expected, state_id, 110u, &compatibility,
                                 &one_triangle, &two_triangle);
    CHECK(memcmp(first, expected, sizeof(first)) == 0);
    CHECK(first[0] == 'X' && first[1] == 'G' && first[2] == 'I' && first[3] == 'R');
    CHECK(first[4] == 1u && first[5] == 0u);
    CHECK(first[6] == 40u && first[7] == 0u);
    CHECK(first[32] == FIXTURE_ITEM_COUNT && first[33] == 0u &&
          first[34] == 0u && first[35] == 0u);
    CHECK(first[36] == (uint8_t)FIXTURE_SERIALIZED_SIZE);
    CHECK(first[37] == (uint8_t)(FIXTURE_SERIALIZED_SIZE >> 8u));
    CHECK(bytes_are_zero(first + XG_RENDER_IR_SERIALIZED_HEADER_SIZE + 29u, 3u));
    CHECK(bytes_are_zero(first + XG_RENDER_IR_SERIALIZED_HEADER_SIZE + 60u, 4u));
    CHECK(bytes_are_zero(first + first_native_offset + 64u + 35u, 5u));
    CHECK(bytes_are_zero(first + first_native_offset + 104u + 1u, 7u));
    CHECK(bytes_are_zero(first + first_native_offset + 112u + 2u, 2u));
    CHECK(bytes_are_zero(first + first_native_offset + 112u + 23u, 1u));
    CHECK(bytes_are_zero(first + first_native_offset + 112u + 43u, 1u));
    CHECK(bytes_are_zero(first + first_native_offset + 112u + 63u, 1u));
    CHECK(bytes_are_zero(first + first_native_offset + 176u, 64u));

    CHECK(xg_render_ir_reset(ir) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_snapshot(ir, &snapshot) == XG_RENDER_IR_OK);
    CHECK(snapshot.phase == XG_RENDER_IR_EMPTY);
    CHECK(snapshot.item_count == 0u);
    CHECK(snapshot.compatibility_count == 0u);
    CHECK(snapshot.native_count == 0u);
    CHECK(snapshot.state_id.scene_epoch == 0u);
    CHECK(snapshot.state_id.state_sequence == 0u);
    CHECK(snapshot.vram_mutation_serial == 0u);
    CHECK(!snapshot.usable);
    CHECK(xg_render_ir_item_get(ir, 0u, &item) == XG_RENDER_IR_NOT_FINALIZED);
    CHECK(xg_render_ir_test_stored_item_get(ir, 0u, &item) == XG_RENDER_IR_OUT_OF_RANGE);

    make_fixture_items(state_id, &compatibility, &one_triangle, &two_triangle);
    CHECK(xg_render_ir_begin(ir, state_id, 110u) == XG_RENDER_IR_OK);
    CHECK(append_fixture(ir, &compatibility, &one_triangle, &two_triangle));
    CHECK(xg_render_ir_finalize(ir, state_id, 110u) == XG_RENDER_IR_OK);
    CHECK(xg_render_ir_serialize_normalized(ir, second, sizeof(second), &written_size) ==
          XG_RENDER_IR_OK);
    CHECK(written_size == FIXTURE_SERIALIZED_SIZE);
    CHECK(memcmp(first, second, sizeof(first)) == 0);
    return 1;
}

int main(void) {
    if (!test_owner_reset_and_begin_contract()) return 1;
    if (!test_order_provenance_copy_and_finalize()) return 1;
    if (!test_invalid_materials_reject_whole_transaction()) return 1;
    if (!test_capacity_rejects_whole_transaction()) return 1;
    if (!test_finalize_rejections()) return 1;
    if (!test_post_finalize_mutation_preserves_stored_items()) return 1;
    if (!test_null_range_and_buffer_failures_do_not_poison()) return 1;
    if (!test_normalized_serialization_and_reset()) return 1;
    return 0;
}
