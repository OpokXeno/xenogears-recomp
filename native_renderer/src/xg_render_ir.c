#include "xg_render_ir.h"

#include <limits.h>
#include <string.h>

struct XgRenderIr {
    GuestRenderVisualStateId state_id;
    uint64_t vram_mutation_serial;
    size_t item_count;
    size_t compatibility_count;
    size_t native_count;
    XgRenderIrPhase phase;
    XgRenderIrRejectReason reject_reason;
    XgRenderIrItem items[XG_RENDER_IR_ITEM_CAPACITY];
    bool insertion_ordered[XG_RENDER_IR_ITEM_CAPACITY];
};

static XgRenderIr process_owner;

static bool is_process_owner(const XgRenderIr *ir) {
    return ir == &process_owner;
}

static bool state_equal(GuestRenderVisualStateId left,
                        GuestRenderVisualStateId right) {
    return left.scene_epoch == right.scene_epoch &&
           left.state_sequence == right.state_sequence;
}

static bool state_is_zero(GuestRenderVisualStateId state_id) {
    return state_id.scene_epoch == 0u && state_id.state_sequence == 0u;
}

static bool provenance_is_zero(XgRenderIrProvenanceKey provenance_key) {
    return state_is_zero(provenance_key.state_id) && provenance_key.slot_index == 0u;
}

static bool base_is_valid(const XgRenderIrItemBase *base,
                          GuestRenderVisualStateId state_id,
                          bool native_required) {
    if (native_required && !base->has_provenance) return false;
    if (!base->has_provenance)
        return provenance_is_zero(base->provenance_key) &&
               base->source_primitive_index == 0u;
    return state_equal(base->provenance_key.state_id, state_id);
}

static bool packet_address_is_valid(uint32_t packet_guest_address) {
    return packet_guest_address <= UINT32_C(0x001ffffc) &&
           (packet_guest_address & UINT32_C(3)) == 0u;
}

static bool material_is_valid(const XgRenderIrMaterialState *material) {
    const uint16_t encoded_depth = (uint16_t)((material->tpage >> 7u) & 3u);

    if (material->tpage > UINT16_C(0x01ff) || encoded_depth == 3u ||
        material->texture_page_x != (material->tpage & UINT16_C(0x000f)) ||
        material->texture_page_y != ((material->tpage >> 4u) & 1u) ||
        material->blend_mode !=
                (XgRenderIrBlendMode)((material->tpage >> 5u) & 3u) ||
        material->texture_depth != (XgRenderIrTextureDepth)encoded_depth ||
        material->texture_window_mask_x > 31u ||
        material->texture_window_mask_y > 31u ||
        material->texture_window_offset_x > 31u ||
        material->texture_window_offset_y > 31u ||
        material->clut_x > 1023u || material->clut_y > 511u ||
        (material->clut_x & 15u) != 0u ||
        material->draw_area_left > 1023u || material->draw_area_top > 1023u ||
        material->draw_area_right > 1023u || material->draw_area_bottom > 1023u ||
        material->draw_offset_x < -1024 || material->draw_offset_x > 1023 ||
        material->draw_offset_y < -1024 || material->draw_offset_y > 1023 ||
        material->texture_depth > XG_RENDER_IR_TEXTURE_15_BIT ||
        material->blend_mode > XG_RENDER_IR_BLEND_ADD_QUARTER ||
        material->shading > XG_RENDER_IR_SHADING_GOURAUD ||
        (material->raw_texture && !material->textured))
        return false;
    return true;
}

static bool triangle_is_valid(const XgRenderIrTriangle *triangle,
                              size_t triangle_index,
                              uint8_t triangle_count,
                              XgRenderIrShading shading) {
    size_t vertex_index;

    if (triangle->split_index != triangle_index ||
        triangle->split_count != triangle_count)
        return false;
    if (shading != XG_RENDER_IR_SHADING_FLAT) return true;
    for (vertex_index = 1u; vertex_index < 3u; ++vertex_index) {
        if (triangle->vertices[vertex_index].r != triangle->vertices[0].r ||
            triangle->vertices[vertex_index].g != triangle->vertices[0].g ||
            triangle->vertices[vertex_index].b != triangle->vertices[0].b)
            return false;
    }
    return true;
}

static bool native_is_valid(const XgRenderIrNativeItem *item,
                            GuestRenderVisualStateId state_id) {
    size_t triangle_index;

    if (!base_is_valid(&item->base, state_id, true) ||
        !material_is_valid(&item->native.material) ||
        item->native.triangle_count == 0u ||
        item->native.triangle_count > XG_RENDER_IR_TRIANGLE_CAPACITY)
        return false;
    for (triangle_index = 0u; triangle_index < item->native.triangle_count;
         ++triangle_index) {
        if (!triangle_is_valid(&item->native.triangles[triangle_index],
                               triangle_index,
                               item->native.triangle_count,
                               item->native.material.shading))
            return false;
    }
    return true;
}

static XgRenderIrResult reject_transaction(XgRenderIr *ir,
                                           XgRenderIrRejectReason reason,
                                           XgRenderIrResult result) {
    ir->phase = XG_RENDER_IR_REJECTED;
    ir->reject_reason = reason;
    return result;
}

static bool serialized_size_for_counts(size_t compatibility_count,
                                       size_t native_count,
                                       size_t *out_size) {
    size_t total_size = XG_RENDER_IR_SERIALIZED_HEADER_SIZE;

    if (compatibility_count >
        (SIZE_MAX - total_size) / XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE)
        return false;
    total_size += compatibility_count * XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE;
    if (native_count > (SIZE_MAX - total_size) / XG_RENDER_IR_SERIALIZED_NATIVE_SIZE)
        return false;
    total_size += native_count * XG_RENDER_IR_SERIALIZED_NATIVE_SIZE;
    if (total_size > UINT32_MAX) return false;
    *out_size = total_size;
    return true;
}

static bool same_native_identity(const XgRenderIrItem *left,
                                 const XgRenderIrItem *right) {
    return state_equal(left->base.provenance_key.state_id,
                       right->base.provenance_key.state_id) &&
           left->base.provenance_key.slot_index == right->base.provenance_key.slot_index &&
           left->base.source_primitive_index == right->base.source_primitive_index;
}

static bool native_split_identities_are_unique(const XgRenderIr *ir,
                                               size_t item_index) {
    const XgRenderIrItem *item = &ir->items[item_index];
    size_t prior_index;
    size_t split_index;

    if (item->kind != XG_RENDER_IR_ITEM_NATIVE) return true;
    for (prior_index = 0u; prior_index < item_index; ++prior_index) {
        const XgRenderIrItem *prior = &ir->items[prior_index];
        size_t prior_split_index;

        if (prior->kind != XG_RENDER_IR_ITEM_NATIVE ||
            !same_native_identity(item, prior))
            continue;
        for (split_index = 0u; split_index < item->native.triangle_count;
             ++split_index) {
            for (prior_split_index = 0u;
                 prior_split_index < prior->native.triangle_count;
                 ++prior_split_index) {
                if (item->native.triangles[split_index].split_index ==
                    prior->native.triangles[prior_split_index].split_index)
                    return false;
            }
        }
    }
    return true;
}

static XgRenderIrResult normalize_insertion_order(XgRenderIr *ir) {
    size_t item_index;
    size_t insertion_count = 0u;

    for (item_index = 0u; item_index < ir->item_count; ++item_index)
        insertion_count += ir->insertion_ordered[item_index] ? 1u : 0u;
    if (insertion_count == 0u) return XG_RENDER_IR_OK;
    if (insertion_count != ir->item_count || ir->compatibility_count != 0u)
        return XG_RENDER_IR_INVALID_ORDER;

    for (item_index = 1u; item_index < ir->item_count; ++item_index) {
        XgRenderIrItem item = ir->items[item_index];
        size_t destination = item_index;

        while (destination > 0u) {
            const XgRenderIrItem *prior = &ir->items[destination - 1u];
            const bool item_precedes =
                item.base.ordering.ot_bucket > prior->base.ordering.ot_bucket ||
                (item.base.ordering.ot_bucket == prior->base.ordering.ot_bucket &&
                 item.base.ordering.final_ordinal >
                     prior->base.ordering.final_ordinal);
            if (!item_precedes) break;
            ir->items[destination] = *prior;
            --destination;
        }
        ir->items[destination] = item;
    }
    for (item_index = 0u; item_index < ir->item_count; ++item_index) {
        XgRenderIrOrdering *ordering = &ir->items[item_index].base.ordering;

        ordering->final_ordinal = (uint32_t)item_index;
        ordering->predecessor_guest_address =
            item_index == 0u
                ? XG_RENDER_IR_NO_PACKET_ADDRESS
                : ir->items[item_index - 1u].base.ordering.packet_guest_address;
        if (item_index + 1u == ir->item_count) {
            ordering->successor_guest_address = XG_RENDER_IR_NO_PACKET_ADDRESS;
            ordering->tag_link_guest_address = XG_RENDER_IR_TAG_LINK_END;
        } else {
            ordering->successor_guest_address =
                ir->items[item_index + 1u].base.ordering.packet_guest_address;
            ordering->tag_link_guest_address = ordering->successor_guest_address;
        }
    }
    return XG_RENDER_IR_OK;
}

static XgRenderIrResult validate_finalized_items(const XgRenderIr *ir) {
    size_t item_index;

    for (item_index = 0u; item_index < ir->item_count; ++item_index) {
        const XgRenderIrItem *item = &ir->items[item_index];

        if (item->base.ordering.final_ordinal != item_index)
            return XG_RENDER_IR_INVALID_ORDER;
    }
    for (item_index = 0u; item_index < ir->item_count; ++item_index) {
        const XgRenderIrItem *item = &ir->items[item_index];
        size_t prior_index;

        if (!packet_address_is_valid(item->base.ordering.packet_guest_address))
            return XG_RENDER_IR_INVALID_LINKAGE;
        for (prior_index = 0u; prior_index < item_index; ++prior_index) {
            if (ir->items[prior_index].base.ordering.packet_guest_address ==
                item->base.ordering.packet_guest_address)
                return XG_RENDER_IR_INVALID_LINKAGE;
        }
        if ((item_index == 0u && item->base.ordering.predecessor_guest_address !=
                                      XG_RENDER_IR_NO_PACKET_ADDRESS) ||
            (item_index != 0u && item->base.ordering.predecessor_guest_address !=
                                      ir->items[item_index - 1u]
                                          .base.ordering.packet_guest_address))
            return XG_RENDER_IR_INVALID_LINKAGE;
        if (item_index + 1u == ir->item_count) {
            if (item->base.ordering.successor_guest_address !=
                        XG_RENDER_IR_NO_PACKET_ADDRESS ||
                item->base.ordering.tag_link_guest_address != XG_RENDER_IR_TAG_LINK_END)
                return XG_RENDER_IR_INVALID_LINKAGE;
        } else if (item->base.ordering.successor_guest_address !=
                           ir->items[item_index + 1u].base.ordering.packet_guest_address ||
                   item->base.ordering.tag_link_guest_address !=
                           ir->items[item_index + 1u].base.ordering.packet_guest_address) {
            return XG_RENDER_IR_INVALID_LINKAGE;
        }
    }
    for (item_index = 0u; item_index < ir->item_count; ++item_index) {
        const XgRenderIrItem *item = &ir->items[item_index];

        if (item->kind == XG_RENDER_IR_ITEM_COMPATIBILITY) {
            if (!base_is_valid(&item->base, ir->state_id, false))
                return XG_RENDER_IR_INVALID_ITEM;
        } else if (item->kind == XG_RENDER_IR_ITEM_NATIVE) {
            XgRenderIrNativeItem native_item = { 0 };

            native_item.base = item->base;
            native_item.native = item->native;
            if (!native_is_valid(&native_item, ir->state_id) ||
                !native_split_identities_are_unique(ir, item_index))
                return XG_RENDER_IR_INVALID_ITEM;
        } else {
            return XG_RENDER_IR_INVALID_ITEM;
        }
    }
    return XG_RENDER_IR_OK;
}

static XgRenderIrRejectReason reject_reason_for_result(XgRenderIrResult result) {
    switch (result) {
    case XG_RENDER_IR_CAPACITY_EXCEEDED:
        return XG_RENDER_IR_REJECT_CAPACITY;
    case XG_RENDER_IR_STALE_VRAM_SERIAL:
        return XG_RENDER_IR_REJECT_STALE_VRAM_SERIAL;
    case XG_RENDER_IR_INVALID_ORDER:
        return XG_RENDER_IR_REJECT_INVALID_ORDER;
    case XG_RENDER_IR_INVALID_LINKAGE:
        return XG_RENDER_IR_REJECT_INVALID_LINKAGE;
    default:
        return XG_RENDER_IR_REJECT_INVALID_ITEM;
    }
}

static void write_zeroes(uint8_t *bytes, size_t length) {
    size_t index;

    for (index = 0u; index < length; ++index)
        bytes[index] = 0u;
}

static void write_u16_le(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void write_u64_le(uint8_t *bytes, uint64_t value) {
    size_t index;

    for (index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static void write_i32_le(uint8_t *bytes, int32_t value) {
    write_u32_le(bytes, (uint32_t)value);
}

static void write_common_record(uint8_t *record,
                                const XgRenderIrItem *item,
                                uint16_t record_size) {
    record[0] = (uint8_t)item->kind;
    record[1] = item->base.has_provenance ? 1u : 0u;
    write_u16_le(record + 2u, record_size);
    write_u32_le(record + 4u, item->base.ordering.final_ordinal);
    write_u32_le(record + 8u, item->base.ordering.ot_bucket);
    write_u32_le(record + 12u, item->base.ordering.packet_guest_address);
    write_u32_le(record + 16u, item->base.ordering.predecessor_guest_address);
    write_u32_le(record + 20u, item->base.ordering.successor_guest_address);
    write_u32_le(record + 24u, item->base.ordering.tag_link_guest_address);
    record[28] = item->base.ordering.tag_payload_word_count;
    write_u64_le(record + 32u, item->base.provenance_key.state_id.scene_epoch);
    write_u64_le(record + 40u, item->base.provenance_key.state_id.state_sequence);
    write_u64_le(record + 48u, (uint64_t)item->base.provenance_key.slot_index);
    write_u32_le(record + 56u, item->base.source_primitive_index);
}

static void write_material_record(uint8_t *record,
                                  const XgRenderIrMaterialState *material) {
    write_u16_le(record + 0u, material->tpage);
    write_u16_le(record + 2u, material->texture_page_x);
    write_u16_le(record + 4u, material->texture_page_y);
    write_u16_le(record + 6u, material->clut_x);
    write_u16_le(record + 8u, material->clut_y);
    write_u16_le(record + 10u, material->draw_area_left);
    write_u16_le(record + 12u, material->draw_area_top);
    write_u16_le(record + 14u, material->draw_area_right);
    write_u16_le(record + 16u, material->draw_area_bottom);
    write_u16_le(record + 18u, (uint16_t)material->draw_offset_x);
    write_u16_le(record + 20u, (uint16_t)material->draw_offset_y);
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

        write_i32_le(serialized_vertex + 0u, vertex->x);
        write_i32_le(serialized_vertex + 4u, vertex->y);
        write_i32_le(serialized_vertex + 8u, vertex->u);
        write_i32_le(serialized_vertex + 12u, vertex->v);
        serialized_vertex[16] = vertex->r;
        serialized_vertex[17] = vertex->g;
        serialized_vertex[18] = vertex->b;
    }
}

static void write_native_record(uint8_t *record, const XgRenderIrItem *item) {
    size_t triangle_index;

    write_common_record(record, item, XG_RENDER_IR_SERIALIZED_NATIVE_SIZE);
    write_material_record(record + 64u, &item->native.material);
    record[104] = item->native.triangle_count;
    for (triangle_index = 0u; triangle_index < item->native.triangle_count;
         ++triangle_index) {
        write_triangle_record(record + 112u + (triangle_index * 64u),
                              &item->native.triangles[triangle_index]);
    }
}

XgRenderIrResult xg_render_ir_process_owner(XgRenderIr **out_ir) {
    if (!out_ir) return XG_RENDER_IR_INVALID_ARGUMENT;
    *out_ir = &process_owner;
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_reset(XgRenderIr *ir) {
    if (!is_process_owner(ir)) return XG_RENDER_IR_INVALID_ARGUMENT;
    memset(ir, 0, sizeof(*ir));
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_begin(XgRenderIr *ir,
                                    GuestRenderVisualStateId state_id,
                                    uint64_t vram_mutation_serial) {
    if (!is_process_owner(ir) || state_id.scene_epoch == 0u)
        return XG_RENDER_IR_INVALID_ARGUMENT;
    if (ir->phase != XG_RENDER_IR_EMPTY) return XG_RENDER_IR_INVALID_TRANSITION;
    ir->state_id = state_id;
    ir->vram_mutation_serial = vram_mutation_serial;
    ir->phase = XG_RENDER_IR_BUILDING;
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_append_compatibility(
    XgRenderIr *ir,
    const XgRenderIrCompatibilityItem *item) {
    XgRenderIrItem *stored_item;

    if (!is_process_owner(ir) || !item) return XG_RENDER_IR_INVALID_ARGUMENT;
    if (ir->phase == XG_RENDER_IR_FINALIZED)
        return reject_transaction(ir, XG_RENDER_IR_REJECT_POST_FINALIZE_MUTATION,
                                  XG_RENDER_IR_FROZEN);
    if (ir->phase == XG_RENDER_IR_REJECTED) return XG_RENDER_IR_TRANSACTION_REJECTED;
    if (ir->phase != XG_RENDER_IR_BUILDING) return XG_RENDER_IR_INVALID_TRANSITION;
    if (ir->item_count == XG_RENDER_IR_ITEM_CAPACITY)
        return reject_transaction(ir, XG_RENDER_IR_REJECT_CAPACITY,
                                  XG_RENDER_IR_CAPACITY_EXCEEDED);
    if (!base_is_valid(&item->base, ir->state_id, false))
        return reject_transaction(ir, XG_RENDER_IR_REJECT_INVALID_ITEM,
                                  XG_RENDER_IR_INVALID_ITEM);
    stored_item = &ir->items[ir->item_count];
    memset(stored_item, 0, sizeof(*stored_item));
    stored_item->kind = XG_RENDER_IR_ITEM_COMPATIBILITY;
    stored_item->base = item->base;
    ++ir->item_count;
    ++ir->compatibility_count;
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_append_native(XgRenderIr *ir,
                                             const XgRenderIrNativeItem *item) {
    XgRenderIrItem *stored_item;

    if (!is_process_owner(ir) || !item) return XG_RENDER_IR_INVALID_ARGUMENT;
    if (ir->phase == XG_RENDER_IR_FINALIZED)
        return reject_transaction(ir, XG_RENDER_IR_REJECT_POST_FINALIZE_MUTATION,
                                  XG_RENDER_IR_FROZEN);
    if (ir->phase == XG_RENDER_IR_REJECTED) return XG_RENDER_IR_TRANSACTION_REJECTED;
    if (ir->phase != XG_RENDER_IR_BUILDING) return XG_RENDER_IR_INVALID_TRANSITION;
    if (ir->item_count == XG_RENDER_IR_ITEM_CAPACITY)
        return reject_transaction(ir, XG_RENDER_IR_REJECT_CAPACITY,
                                  XG_RENDER_IR_CAPACITY_EXCEEDED);
    if (!native_is_valid(item, ir->state_id))
        return reject_transaction(ir, XG_RENDER_IR_REJECT_INVALID_ITEM,
                                  XG_RENDER_IR_INVALID_ITEM);
    stored_item = &ir->items[ir->item_count];
    memset(stored_item, 0, sizeof(*stored_item));
    stored_item->kind = XG_RENDER_IR_ITEM_NATIVE;
    stored_item->base = item->base;
    stored_item->native = item->native;
    ++ir->item_count;
    ++ir->native_count;
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_append_native_insertion(
    XgRenderIr *ir, const XgRenderIrNativeItem *item, uint32_t ot_bucket,
    uint32_t packet_guest_address, uint8_t tag_payload_word_count) {
    XgRenderIrNativeItem insertion;
    XgRenderIrResult result;
    size_t stored_index;

    if (!is_process_owner(ir) || item == NULL ||
        !packet_address_is_valid(packet_guest_address) ||
        tag_payload_word_count == 0u)
        return XG_RENDER_IR_INVALID_ARGUMENT;
    insertion = *item;
    insertion.base.ordering.ot_bucket = ot_bucket;
    insertion.base.ordering.final_ordinal = (uint32_t)ir->item_count;
    insertion.base.ordering.packet_guest_address = packet_guest_address;
    insertion.base.ordering.predecessor_guest_address =
        XG_RENDER_IR_NO_PACKET_ADDRESS;
    insertion.base.ordering.successor_guest_address =
        XG_RENDER_IR_NO_PACKET_ADDRESS;
    insertion.base.ordering.tag_link_guest_address = XG_RENDER_IR_TAG_LINK_END;
    insertion.base.ordering.tag_payload_word_count = tag_payload_word_count;
    stored_index = ir->item_count;
    result = xg_render_ir_append_native(ir, &insertion);
    if (result == XG_RENDER_IR_OK) ir->insertion_ordered[stored_index] = true;
    return result;
}

XgRenderIrResult xg_render_ir_finalize(XgRenderIr *ir,
                                       GuestRenderVisualStateId state_id,
                                       uint64_t current_vram_mutation_serial) {
    XgRenderIrResult validation_result;
    size_t serialized_size;

    if (!is_process_owner(ir)) return XG_RENDER_IR_INVALID_ARGUMENT;
    if (ir->phase == XG_RENDER_IR_REJECTED) return XG_RENDER_IR_TRANSACTION_REJECTED;
    if (ir->phase != XG_RENDER_IR_BUILDING) return XG_RENDER_IR_INVALID_TRANSITION;
    if (!state_equal(state_id, ir->state_id))
        return reject_transaction(ir, XG_RENDER_IR_REJECT_VISUAL_STATE_MISMATCH,
                                  XG_RENDER_IR_INVALID_TRANSITION);
    if (current_vram_mutation_serial != ir->vram_mutation_serial)
        return reject_transaction(ir, XG_RENDER_IR_REJECT_STALE_VRAM_SERIAL,
                                  XG_RENDER_IR_STALE_VRAM_SERIAL);
    validation_result = normalize_insertion_order(ir);
    if (validation_result == XG_RENDER_IR_OK)
        validation_result = validate_finalized_items(ir);
    if (validation_result != XG_RENDER_IR_OK)
        return reject_transaction(ir, reject_reason_for_result(validation_result),
                                  validation_result);
    if (!serialized_size_for_counts(ir->compatibility_count, ir->native_count,
                                    &serialized_size))
        return reject_transaction(ir, XG_RENDER_IR_REJECT_CAPACITY,
                                  XG_RENDER_IR_CAPACITY_EXCEEDED);
    ir->phase = XG_RENDER_IR_FINALIZED;
    ir->reject_reason = XG_RENDER_IR_REJECT_NONE;
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_snapshot(const XgRenderIr *ir,
                                       XgRenderIrSnapshot *out_snapshot) {
    if (!is_process_owner(ir) || !out_snapshot) return XG_RENDER_IR_INVALID_ARGUMENT;
    out_snapshot->state_id = ir->state_id;
    out_snapshot->vram_mutation_serial = ir->vram_mutation_serial;
    out_snapshot->item_count = ir->item_count;
    out_snapshot->compatibility_count = ir->compatibility_count;
    out_snapshot->native_count = ir->native_count;
    out_snapshot->phase = ir->phase;
    out_snapshot->reject_reason = ir->reject_reason;
    out_snapshot->usable = ir->phase == XG_RENDER_IR_FINALIZED;
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_item_get(const XgRenderIr *ir,
                                       size_t index,
                                       XgRenderIrItem *out_item) {
    if (!is_process_owner(ir) || !out_item) return XG_RENDER_IR_INVALID_ARGUMENT;
    if (ir->phase == XG_RENDER_IR_REJECTED) return XG_RENDER_IR_TRANSACTION_REJECTED;
    if (ir->phase != XG_RENDER_IR_FINALIZED) return XG_RENDER_IR_NOT_FINALIZED;
    if (index >= ir->item_count) return XG_RENDER_IR_OUT_OF_RANGE;
    *out_item = ir->items[index];
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_serialized_size(const XgRenderIr *ir,
                                              size_t *out_size) {
    if (!is_process_owner(ir) || !out_size) return XG_RENDER_IR_INVALID_ARGUMENT;
    if (ir->phase == XG_RENDER_IR_REJECTED) return XG_RENDER_IR_TRANSACTION_REJECTED;
    if (ir->phase != XG_RENDER_IR_FINALIZED) return XG_RENDER_IR_NOT_FINALIZED;
    if (!serialized_size_for_counts(ir->compatibility_count, ir->native_count,
                                    out_size))
        return XG_RENDER_IR_TRANSACTION_REJECTED;
    return XG_RENDER_IR_OK;
}

XgRenderIrResult xg_render_ir_serialize_normalized(const XgRenderIr *ir,
                                                    uint8_t *out_bytes,
                                                    size_t out_capacity,
                                                    size_t *out_size) {
    size_t required_size;
    size_t item_index;
    size_t offset;

    if (!is_process_owner(ir) || !out_bytes || !out_size)
        return XG_RENDER_IR_INVALID_ARGUMENT;
    if (ir->phase == XG_RENDER_IR_REJECTED) return XG_RENDER_IR_TRANSACTION_REJECTED;
    if (ir->phase != XG_RENDER_IR_FINALIZED) return XG_RENDER_IR_NOT_FINALIZED;
    if (!serialized_size_for_counts(ir->compatibility_count, ir->native_count,
                                    &required_size))
        return XG_RENDER_IR_TRANSACTION_REJECTED;
    if (out_capacity < required_size) {
        *out_size = required_size;
        return XG_RENDER_IR_BUFFER_TOO_SMALL;
    }
    write_zeroes(out_bytes, required_size);
    out_bytes[0] = 'X';
    out_bytes[1] = 'G';
    out_bytes[2] = 'I';
    out_bytes[3] = 'R';
    write_u16_le(out_bytes + 4u, XG_RENDER_IR_SERIALIZATION_VERSION);
    write_u16_le(out_bytes + 6u, XG_RENDER_IR_SERIALIZED_HEADER_SIZE);
    write_u64_le(out_bytes + 8u, ir->state_id.scene_epoch);
    write_u64_le(out_bytes + 16u, ir->state_id.state_sequence);
    write_u64_le(out_bytes + 24u, ir->vram_mutation_serial);
    write_u32_le(out_bytes + 32u, (uint32_t)ir->item_count);
    write_u32_le(out_bytes + 36u, (uint32_t)required_size);
    offset = XG_RENDER_IR_SERIALIZED_HEADER_SIZE;
    for (item_index = 0u; item_index < ir->item_count; ++item_index) {
        const XgRenderIrItem *item = &ir->items[item_index];

        if (item->kind == XG_RENDER_IR_ITEM_COMPATIBILITY) {
            write_common_record(out_bytes + offset, item,
                                XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE);
            offset += XG_RENDER_IR_SERIALIZED_COMPATIBILITY_SIZE;
        } else {
            write_native_record(out_bytes + offset, item);
            offset += XG_RENDER_IR_SERIALIZED_NATIVE_SIZE;
        }
    }
    *out_size = required_size;
    return XG_RENDER_IR_OK;
}

size_t xg_render_ir_item_capacity(void) {
    return XG_RENDER_IR_ITEM_CAPACITY;
}

const char *xg_render_ir_reject_reason_name(uint32_t reason) {
    switch (reason) {
    case XG_RENDER_IR_REJECT_NONE:
        return "none";
    case XG_RENDER_IR_REJECT_CAPACITY:
        return "capacity";
    case XG_RENDER_IR_REJECT_STALE_VRAM_SERIAL:
        return "stale_vram_serial";
    case XG_RENDER_IR_REJECT_INVALID_ITEM:
        return "invalid_item";
    case XG_RENDER_IR_REJECT_INVALID_ORDER:
        return "invalid_order";
    case XG_RENDER_IR_REJECT_INVALID_LINKAGE:
        return "invalid_linkage";
    case XG_RENDER_IR_REJECT_VISUAL_STATE_MISMATCH:
        return "visual_state_mismatch";
    case XG_RENDER_IR_REJECT_POST_FINALIZE_MUTATION:
        return "post_finalize_mutation";
    default:
        return "unknown";
    }
}

#ifdef XG_RENDER_IR_TESTING
void xg_render_ir_test_reset_process_owner(void) {
    (void)xg_render_ir_reset(&process_owner);
}

XgRenderIrResult xg_render_ir_test_stored_item_get(const XgRenderIr *ir,
                                                    size_t index,
                                                    XgRenderIrItem *out_item) {
    if (!is_process_owner(ir) || !out_item) return XG_RENDER_IR_INVALID_ARGUMENT;
    if (index >= ir->item_count) return XG_RENDER_IR_OUT_OF_RANGE;
    *out_item = ir->items[index];
    return XG_RENDER_IR_OK;
}
#endif
