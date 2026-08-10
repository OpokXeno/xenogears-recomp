#include "xg_render_backend.h"

#include <limits.h>
#include <string.h>

typedef enum MaterialValidation {
    MATERIAL_VALID = 0,
    MATERIAL_INVALID,
    MATERIAL_UNSUPPORTED,
} MaterialValidation;

static XgRenderBackendResult result_with_status(
        XgRenderBackendStatus status,
        XgRenderBackendFallbackReason fallback_reason) {
    XgRenderBackendResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.fallback_reason = fallback_reason;
    result.transaction_status = GPU_RENDER_TRANSACTION_OK;
    result.failed_final_ordinal = XG_RENDER_BACKEND_NO_FINAL_ORDINAL;
    return result;
}

static bool state_ids_equal(GuestRenderVisualStateId left,
                            GuestRenderVisualStateId right) {
    return left.scene_epoch == right.scene_epoch &&
           left.state_sequence == right.state_sequence;
}

static bool base_is_valid(const XgRenderIrItemBase *base,
                          GuestRenderVisualStateId state_id,
                          bool provenance_required) {
    if (provenance_required && !base->has_provenance) return false;
    if (!base->has_provenance) {
        return base->provenance_key.state_id.scene_epoch == 0u &&
               base->provenance_key.state_id.state_sequence == 0u &&
               base->provenance_key.slot_index == 0u &&
               base->source_primitive_index == 0u;
    }
    return state_ids_equal(base->provenance_key.state_id, state_id);
}

static MaterialValidation validate_material(
        const XgRenderIrMaterialState *material) {
    uint16_t encoded_depth;

    switch (material->texture_depth) {
    case XG_RENDER_IR_TEXTURE_4_BIT:
    case XG_RENDER_IR_TEXTURE_8_BIT:
    case XG_RENDER_IR_TEXTURE_15_BIT:
        break;
    default:
        return MATERIAL_UNSUPPORTED;
    }
    switch (material->blend_mode) {
    case XG_RENDER_IR_BLEND_AVERAGE:
    case XG_RENDER_IR_BLEND_ADD:
    case XG_RENDER_IR_BLEND_SUBTRACT:
    case XG_RENDER_IR_BLEND_ADD_QUARTER:
        break;
    default:
        return MATERIAL_UNSUPPORTED;
    }
    switch (material->shading) {
    case XG_RENDER_IR_SHADING_FLAT:
    case XG_RENDER_IR_SHADING_GOURAUD:
        break;
    default:
        return MATERIAL_UNSUPPORTED;
    }

    encoded_depth = (uint16_t)((material->tpage >> 7u) & 3u);
    if (material->tpage > UINT16_C(0x01ff) || encoded_depth == 3u ||
        material->texture_page_x != (material->tpage & UINT16_C(0x000f)) ||
        material->texture_page_y != ((material->tpage >> 4u) & 1u) ||
        material->blend_mode !=
            (XgRenderIrBlendMode)((material->tpage >> 5u) & 3u) ||
        material->texture_depth != (XgRenderIrTextureDepth)encoded_depth ||
        material->clut_x > 1023u || (material->clut_x & 15u) != 0u ||
        material->clut_y > 511u ||
        material->draw_area_left > material->draw_area_right ||
        material->draw_area_top > material->draw_area_bottom ||
        material->draw_area_right > 1023u ||
        material->draw_area_bottom > 1023u ||
        material->draw_offset_x < -1024 || material->draw_offset_x > 1023 ||
        material->draw_offset_y < -1024 || material->draw_offset_y > 1023 ||
        material->texture_window_mask_x > 31u ||
        material->texture_window_mask_y > 31u ||
        material->texture_window_offset_x > 31u ||
        material->texture_window_offset_y > 31u ||
        (material->raw_texture && !material->textured))
        return MATERIAL_INVALID;
    return MATERIAL_VALID;
}

static bool fixed_to_gpu(XgRenderIrFixed16_16 source,
                         GpuRenderFixed16_16 *out_value) {
    const int source_bits = XG_RENDER_IR_FIXED_FRACTION_BITS;
    const int target_bits = GPU_RENDER_FIXED_FRACTION_BITS;
    int64_t converted = source;

    if (source_bits < 0 || source_bits > 30 ||
        target_bits < 0 || target_bits > 30)
        return false;
    if (target_bits > source_bits) {
        converted *= INT64_C(1) << (target_bits - source_bits);
    } else if (source_bits > target_bits) {
        const int64_t divisor =
            INT64_C(1) << (source_bits - target_bits);

        if (converted % divisor != 0) return false;
        converted /= divisor;
    }
    if (converted < INT32_MIN || converted > INT32_MAX) return false;
    *out_value = (GpuRenderFixed16_16)converted;
    return (int64_t)*out_value == converted;
}

static bool copy_material(GpuRenderMaterial *out_material,
                          const XgRenderIrMaterialState *material) {
    memset(out_material, 0, sizeof(*out_material));
    out_material->tpage = material->tpage;
    out_material->texture_page_x = material->texture_page_x;
    out_material->texture_page_y = material->texture_page_y;
    out_material->clut_x = material->clut_x;
    out_material->clut_y = material->clut_y;
    out_material->draw_area_left = material->draw_area_left;
    out_material->draw_area_top = material->draw_area_top;
    out_material->draw_area_right = material->draw_area_right;
    out_material->draw_area_bottom = material->draw_area_bottom;
    out_material->draw_offset_x = material->draw_offset_x;
    out_material->draw_offset_y = material->draw_offset_y;
    switch (material->texture_depth) {
    case XG_RENDER_IR_TEXTURE_4_BIT:
        out_material->texture_depth = GPU_RENDER_TEXTURE_4_BIT;
        break;
    case XG_RENDER_IR_TEXTURE_8_BIT:
        out_material->texture_depth = GPU_RENDER_TEXTURE_8_BIT;
        break;
    case XG_RENDER_IR_TEXTURE_15_BIT:
        out_material->texture_depth = GPU_RENDER_TEXTURE_15_BIT;
        break;
    default:
        return false;
    }
    out_material->texture_window_mask_x = material->texture_window_mask_x;
    out_material->texture_window_mask_y = material->texture_window_mask_y;
    out_material->texture_window_offset_x = material->texture_window_offset_x;
    out_material->texture_window_offset_y = material->texture_window_offset_y;
    switch (material->shading) {
    case XG_RENDER_IR_SHADING_FLAT:
        out_material->shading = GPU_RENDER_SHADING_FLAT;
        break;
    case XG_RENDER_IR_SHADING_GOURAUD:
        out_material->shading = GPU_RENDER_SHADING_GOURAUD;
        break;
    default:
        return false;
    }
    out_material->textured = material->textured ? 1u : 0u;
    out_material->raw_texture = material->raw_texture ? 1u : 0u;
    out_material->semi_transparent = material->semi_transparent ? 1u : 0u;
    switch (material->blend_mode) {
    case XG_RENDER_IR_BLEND_AVERAGE:
        out_material->blend_mode = GPU_RENDER_BLEND_AVERAGE;
        break;
    case XG_RENDER_IR_BLEND_ADD:
        out_material->blend_mode = GPU_RENDER_BLEND_ADD;
        break;
    case XG_RENDER_IR_BLEND_SUBTRACT:
        out_material->blend_mode = GPU_RENDER_BLEND_SUBTRACT;
        break;
    case XG_RENDER_IR_BLEND_ADD_QUARTER:
        out_material->blend_mode = GPU_RENDER_BLEND_ADD_QUARTER;
        break;
    default:
        return false;
    }
    out_material->dither = material->dither ? 1u : 0u;
    out_material->mask_set = material->mask_set ? 1u : 0u;
    out_material->mask_check = material->mask_check ? 1u : 0u;
    return true;
}

static XgRenderBackendStatus validate_native_primitive(
        const XgRenderIrNativePrimitive *primitive) {
    MaterialValidation material_validation;
    size_t triangle_index;
    const bool native_view_position =
        primitive->triangles[0].vertices[0].native_view_position;

    if (primitive->triangle_count == 0u ||
        primitive->triangle_count > XG_RENDER_IR_TRIANGLE_CAPACITY ||
        primitive->triangle_count > GPU_RENDER_SEMANTIC_TRIANGLE_CAPACITY)
        return XG_RENDER_BACKEND_INVALID_ITEM;

    material_validation = validate_material(&primitive->material);
    if (material_validation == MATERIAL_INVALID)
        return XG_RENDER_BACKEND_INVALID_MATERIAL;
    if (material_validation == MATERIAL_UNSUPPORTED)
        return XG_RENDER_BACKEND_UNSUPPORTED_MATERIAL;

    for (triangle_index = 0u;
         triangle_index < primitive->triangle_count;
         ++triangle_index) {
        const XgRenderIrTriangle *triangle =
            &primitive->triangles[triangle_index];
        size_t vertex_index;

        if (triangle->split_index != triangle_index ||
            triangle->split_count != primitive->triangle_count)
            return XG_RENDER_BACKEND_INVALID_ITEM;
        for (vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const XgRenderIrVertex *vertex = &triangle->vertices[vertex_index];

            if (vertex->native_view_position != native_view_position ||
                (!vertex->native_view_position &&
                 (vertex->native_view_x != 0 || vertex->native_view_y != 0)))
                return XG_RENDER_BACKEND_INVALID_ITEM;
        }
        if (primitive->material.shading == XG_RENDER_IR_SHADING_FLAT) {
            for (vertex_index = 1u; vertex_index < 3u; ++vertex_index) {
                if (triangle->vertices[vertex_index].r !=
                        triangle->vertices[0].r ||
                    triangle->vertices[vertex_index].g !=
                        triangle->vertices[0].g ||
                    triangle->vertices[vertex_index].b !=
                        triangle->vertices[0].b)
                    return XG_RENDER_BACKEND_INVALID_ITEM;
            }
        }
    }
    return XG_RENDER_BACKEND_OK;
}

static XgRenderBackendStatus translate_native_primitive(
        const XgRenderIrNativePrimitive *primitive,
        GpuRenderSemantic *out_semantic) {
    size_t triangle_index;

    memset(out_semantic, 0, sizeof(*out_semantic));
    if (!copy_material(&out_semantic->material, &primitive->material))
        return XG_RENDER_BACKEND_UNSUPPORTED_MATERIAL;
    out_semantic->screen_space_2d = GPU_RENDER_SCREEN_SPACE_2D_NONE;
    out_semantic->triangle_count = primitive->triangle_count;
    for (triangle_index = 0u;
         triangle_index < primitive->triangle_count;
         ++triangle_index) {
        const XgRenderIrTriangle *source_triangle =
            &primitive->triangles[triangle_index];
        GpuRenderSemanticTriangle *target_triangle =
            &out_semantic->triangles[triangle_index];
        size_t vertex_index;

        target_triangle->split_index = source_triangle->split_index;
        target_triangle->split_count = source_triangle->split_count;
        for (vertex_index = 0u; vertex_index < 3u; ++vertex_index) {
            const XgRenderIrVertex *source_vertex =
                &source_triangle->vertices[vertex_index];
            GpuRenderSemanticVertex *target_vertex =
                &target_triangle->vertices[vertex_index];

            if (!fixed_to_gpu(source_vertex->x, &target_vertex->x) ||
                !fixed_to_gpu(source_vertex->y, &target_vertex->y) ||
                !fixed_to_gpu(source_vertex->u, &target_vertex->u) ||
                !fixed_to_gpu(source_vertex->v, &target_vertex->v))
                return XG_RENDER_BACKEND_FIXED_POINT_CONVERSION_FAILED;
            target_vertex->r = source_vertex->r;
            target_vertex->g = source_vertex->g;
            target_vertex->b = source_vertex->b;
            if (source_vertex->native_view_position) {
                if (!fixed_to_gpu(source_vertex->native_view_x,
                                  &target_vertex->native_view_x) ||
                    !fixed_to_gpu(source_vertex->native_view_y,
                                  &target_vertex->native_view_y))
                    return XG_RENDER_BACKEND_FIXED_POINT_CONVERSION_FAILED;
                target_vertex->native_view_position = 1u;
            }
        }
    }
    return XG_RENDER_BACKEND_OK;
}

XgRenderBackendStatus xg_render_backend_translate_primitive(
        const XgRenderIrNativePrimitive *primitive,
        GpuRenderSemantic *out_semantic) {
    XgRenderBackendStatus status;

    if (!primitive || !out_semantic)
        return XG_RENDER_BACKEND_INVALID_ARGUMENT;
    memset(out_semantic, 0, sizeof(*out_semantic));
    status = validate_native_primitive(primitive);
    if (status != XG_RENDER_BACKEND_OK) return status;
    return translate_native_primitive(primitive, out_semantic);
}

static XgRenderBackendFallbackReason fallback_for_status(
        XgRenderBackendStatus status) {
    switch (status) {
    case XG_RENDER_BACKEND_INVALID_MATERIAL:
    case XG_RENDER_BACKEND_UNSUPPORTED_MATERIAL:
        return XG_RENDER_BACKEND_FALLBACK_MATERIAL;
    case XG_RENDER_BACKEND_FIXED_POINT_CONVERSION_FAILED:
        return XG_RENDER_BACKEND_FALLBACK_FIXED_POINT;
    case XG_RENDER_BACKEND_STALE_VRAM_SERIAL:
        return XG_RENDER_BACKEND_FALLBACK_STALE_VRAM_SERIAL;
    case XG_RENDER_BACKEND_MISSING_NATIVE_ITEMS:
        return XG_RENDER_BACKEND_FALLBACK_MISSING_NATIVE_ITEMS;
    case XG_RENDER_BACKEND_MISSING_COMPATIBILITY_CALLBACK:
    case XG_RENDER_BACKEND_COMPATIBILITY_FAILED:
        return XG_RENDER_BACKEND_FALLBACK_MISSING_COMPATIBILITY;
    case XG_RENDER_BACKEND_TRANSACTION_BEGIN_FAILED:
    case XG_RENDER_BACKEND_ORDERING_BARRIER_FAILED:
    case XG_RENDER_BACKEND_NATIVE_DRAW_FAILED:
    case XG_RENDER_BACKEND_COMMIT_FAILED:
        return XG_RENDER_BACKEND_FALLBACK_TRANSACTION;
    default:
        return XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE;
    }
}

static GpuRenderTransactionId transaction_id_for_snapshot(
        const XgRenderIrSnapshot *snapshot) {
    GpuRenderTransactionId transaction_id;

    transaction_id.scene_epoch = snapshot->state_id.scene_epoch;
    transaction_id.state_sequence = snapshot->state_id.state_sequence;
    return transaction_id;
}

static XgRenderBackendResult rollback_failure(
        XgRenderBackendResult result,
        XgRenderBackendStatus status,
        GpuRenderTransactionStatus transaction_status,
        GpuRenderTransactionId transaction_id,
        uint32_t final_ordinal) {
    result.status = status;
    result.fallback_reason = fallback_for_status(status);
    result.transaction_status = transaction_status;
    result.failed_final_ordinal = final_ordinal;
    result.rollback_attempted = true;
    {
        const GpuRenderTransactionStatus rollback_status =
            gr_rollback(transaction_id);
        result.rollback_succeeded =
            rollback_status == GPU_RENDER_TRANSACTION_OK;
        if (!result.rollback_succeeded) {
            result.status = XG_RENDER_BACKEND_ROLLBACK_FAILED;
            result.fallback_reason = XG_RENDER_BACKEND_FALLBACK_TRANSACTION;
            result.transaction_status = rollback_status;
        }
    }
    return result;
}

XgRenderBackendResult xg_render_backend_submit(
        const XgRenderIr *ir,
        const XgRenderBackendSubmitInfo *submit_info) {
    XgRenderBackendResult result = result_with_status(
        XG_RENDER_BACKEND_OK, XG_RENDER_BACKEND_FALLBACK_NONE);
    XgRenderIrSnapshot snapshot;
    GpuRenderTransactionId transaction_id;
    size_t observed_compatibility_count = 0u;
    size_t observed_native_count = 0u;
    size_t item_index;

    if (!ir || !submit_info || !submit_info->present) {
        return result_with_status(XG_RENDER_BACKEND_INVALID_ARGUMENT,
                                  XG_RENDER_BACKEND_FALLBACK_INVALID_REQUEST);
    }
    if (xg_render_ir_snapshot(ir, &snapshot) != XG_RENDER_IR_OK)
        return result_with_status(XG_RENDER_BACKEND_IR_UNAVAILABLE,
                                  XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE);
    if (!snapshot.usable || snapshot.phase != XG_RENDER_IR_FINALIZED)
        return result_with_status(XG_RENDER_BACKEND_IR_NOT_FINALIZED,
                                  XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE);
    if (snapshot.vram_mutation_serial !=
        submit_info->current_vram_mutation_serial)
        return result_with_status(XG_RENDER_BACKEND_STALE_VRAM_SERIAL,
                                  XG_RENDER_BACKEND_FALLBACK_STALE_VRAM_SERIAL);
    if (snapshot.native_count == 0u)
        return result_with_status(XG_RENDER_BACKEND_MISSING_NATIVE_ITEMS,
                                  XG_RENDER_BACKEND_FALLBACK_MISSING_NATIVE_ITEMS);
    if (snapshot.compatibility_count != 0u &&
        !submit_info->compatibility_callback)
        return result_with_status(
            XG_RENDER_BACKEND_MISSING_COMPATIBILITY_CALLBACK,
            XG_RENDER_BACKEND_FALLBACK_MISSING_COMPATIBILITY);
    if (snapshot.item_count > UINT32_MAX)
        return result_with_status(XG_RENDER_BACKEND_INVALID_ORDER,
                                  XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE);

    for (item_index = 0u; item_index < snapshot.item_count; ++item_index) {
        XgRenderIrItem item;
        XgRenderBackendStatus validation_status = XG_RENDER_BACKEND_OK;

        if (xg_render_ir_item_get(ir, item_index, &item) != XG_RENDER_IR_OK) {
            result.status = XG_RENDER_BACKEND_IR_UNAVAILABLE;
            result.fallback_reason = XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE;
            result.failed_final_ordinal = (uint32_t)item_index;
            return result;
        }
        if (item.base.ordering.final_ordinal != (uint32_t)item_index) {
            result.status = XG_RENDER_BACKEND_INVALID_ORDER;
            result.fallback_reason = XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE;
            result.failed_final_ordinal = (uint32_t)item_index;
            return result;
        }
        if (item.kind == XG_RENDER_IR_ITEM_COMPATIBILITY) {
            if (!base_is_valid(&item.base, snapshot.state_id, false))
                validation_status = XG_RENDER_BACKEND_INVALID_ITEM;
            ++observed_compatibility_count;
        } else if (item.kind == XG_RENDER_IR_ITEM_NATIVE) {
            GpuRenderSemantic converted;

            if (!base_is_valid(&item.base, snapshot.state_id, true))
                validation_status = XG_RENDER_BACKEND_INVALID_ITEM;
            else
                validation_status = xg_render_backend_translate_primitive(
                    &item.native, &converted);
            ++observed_native_count;
        } else {
            validation_status = XG_RENDER_BACKEND_INVALID_ITEM;
        }
        if (validation_status != XG_RENDER_BACKEND_OK) {
            result.status = validation_status;
            result.fallback_reason = fallback_for_status(validation_status);
            result.failed_final_ordinal = (uint32_t)item_index;
            return result;
        }
        ++result.items_preflighted;
    }
    if (observed_compatibility_count != snapshot.compatibility_count ||
        observed_native_count != snapshot.native_count) {
        result.status = XG_RENDER_BACKEND_INVALID_ITEM;
        result.fallback_reason = XG_RENDER_BACKEND_FALLBACK_IR_UNUSABLE;
        return result;
    }
    if (observed_native_count == 0u) {
        result.status = XG_RENDER_BACKEND_MISSING_NATIVE_ITEMS;
        result.fallback_reason = XG_RENDER_BACKEND_FALLBACK_MISSING_NATIVE_ITEMS;
        return result;
    }
    if (observed_compatibility_count != 0u &&
        !submit_info->compatibility_callback) {
        result.status = XG_RENDER_BACKEND_MISSING_COMPATIBILITY_CALLBACK;
        result.fallback_reason =
            XG_RENDER_BACKEND_FALLBACK_MISSING_COMPATIBILITY;
        return result;
    }

    transaction_id = transaction_id_for_snapshot(&snapshot);
    result.transaction_status = gr_transaction_begin(
        transaction_id, snapshot.vram_mutation_serial);
    if (result.transaction_status != GPU_RENDER_TRANSACTION_OK) {
        result.status = XG_RENDER_BACKEND_TRANSACTION_BEGIN_FAILED;
        result.fallback_reason = XG_RENDER_BACKEND_FALLBACK_TRANSACTION;
        return result;
    }
    result.transaction_began = true;

    for (item_index = 0u; item_index < snapshot.item_count; ++item_index) {
        XgRenderIrItem item;
        XgRenderBackendStatus item_status = XG_RENDER_BACKEND_OK;
        const uint32_t ordinal = (uint32_t)item_index;

        if (xg_render_ir_item_get(ir, item_index, &item) != XG_RENDER_IR_OK)
            return rollback_failure(
                result, XG_RENDER_BACKEND_IR_UNAVAILABLE,
                GPU_RENDER_TRANSACTION_OK, transaction_id, ordinal);
        if (item.base.ordering.final_ordinal != ordinal)
            return rollback_failure(
                result, XG_RENDER_BACKEND_INVALID_ORDER,
                GPU_RENDER_TRANSACTION_OK, transaction_id, ordinal);

        /* Fence each ordinal so compatibility and semantic submissions cannot
         * move across one another even though they use different draw paths. */
        result.transaction_status = gr_ordering_barrier(transaction_id);
        if (result.transaction_status != GPU_RENDER_TRANSACTION_OK)
            return rollback_failure(
                result, XG_RENDER_BACKEND_ORDERING_BARRIER_FAILED,
                result.transaction_status, transaction_id, ordinal);

        if (item.kind == XG_RENDER_IR_ITEM_COMPATIBILITY) {
            XgRenderIrCompatibilityItem compatibility_item;

            memset(&compatibility_item, 0, sizeof(compatibility_item));
            compatibility_item.base = item.base;
            if (!submit_info->compatibility_callback(
                    &compatibility_item,
                    submit_info->compatibility_user_data))
                return rollback_failure(
                    result, XG_RENDER_BACKEND_COMPATIBILITY_FAILED,
                    GPU_RENDER_TRANSACTION_OK, transaction_id, ordinal);
            ++result.compatibility_items_submitted;
        } else if (item.kind == XG_RENDER_IR_ITEM_NATIVE) {
            GpuRenderSemantic semantic;

            if (!base_is_valid(&item.base, snapshot.state_id, true))
                item_status = XG_RENDER_BACKEND_INVALID_ITEM;
            else
                item_status = xg_render_backend_translate_primitive(
                    &item.native, &semantic);
            if (item_status != XG_RENDER_BACKEND_OK)
                return rollback_failure(result, item_status,
                                        GPU_RENDER_TRANSACTION_OK,
                                        transaction_id, ordinal);
            result.transaction_status =
                gr_draw_semantic(transaction_id, &semantic);
            if (result.transaction_status != GPU_RENDER_TRANSACTION_OK)
                return rollback_failure(
                    result, XG_RENDER_BACKEND_NATIVE_DRAW_FAILED,
                    result.transaction_status, transaction_id, ordinal);
            ++result.native_items_submitted;
            ++result.semantic_primitives_submitted;
        } else {
            return rollback_failure(
                result, XG_RENDER_BACKEND_INVALID_ITEM,
                GPU_RENDER_TRANSACTION_OK, transaction_id, ordinal);
        }
    }

    result.transaction_status = gr_ordering_barrier(transaction_id);
    if (result.transaction_status != GPU_RENDER_TRANSACTION_OK)
        return rollback_failure(
            result, XG_RENDER_BACKEND_ORDERING_BARRIER_FAILED,
            result.transaction_status, transaction_id,
            XG_RENDER_BACKEND_NO_FINAL_ORDINAL);

    result.transaction_status = gr_commit_validate(
        transaction_id,
        submit_info->current_vram_mutation_serial,
        submit_info->present);
    if (result.transaction_status != GPU_RENDER_TRANSACTION_READY)
        return rollback_failure(result, XG_RENDER_BACKEND_COMMIT_FAILED,
                                result.transaction_status, transaction_id,
                                XG_RENDER_BACKEND_NO_FINAL_ORDINAL);
    return result;
}
