#include "xg_render_model_repository.h"

#include "cpu_state.h"
#include "gpu.h"
#include "xg_render_address_lookup.h"
#include "xg_render_backend.h"
#include "xg_render_primitive_utils.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define XG_RENDER_MODEL_FT3_SOURCE_CAPACITY 16384u
#define XG_RENDER_MODEL_FT4_SOURCE_CAPACITY XG_RENDER_IR_ITEM_CAPACITY
#define XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY 4096u
#define XG_RENDER_MODEL_ANCHOR_CONTEXT_CAPACITY 4096u

typedef struct XgRenderModelFt4TemplateEntry {
    XgRenderModelFt4Template material;
    uint16_t descriptor_next;
    uint32_t table_epoch;
} XgRenderModelFt4TemplateEntry;

typedef struct XgRenderModelPacketCopy {
    CPUState *owner_cpu;
    uint32_t destination;
    uint32_t source;
    uint32_t size;
    bool active;
} XgRenderModelPacketCopy;

typedef struct XgRenderModelAnchorContext {
    GpuRenderMaterial material;
    uint64_t scene_id;
    uint32_t producer_id;
} XgRenderModelAnchorContext;

static XgRenderModelFt3SourceRecord ft3_sources[
    XG_RENDER_MODEL_FT3_SOURCE_CAPACITY];
static XgRenderModelFt4SourceRecord ft4_sources[
    XG_RENDER_MODEL_FT4_SOURCE_CAPACITY];
static uint32_t ft3_source_count;
static uint32_t ft4_source_count;
static XgRenderAddressLookupSlot ft3_source_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static XgRenderAddressLookupSlot ft4_source_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static uint16_t ft3_source_lookup_epoch = 1u;
static uint16_t ft4_source_lookup_epoch = 1u;

static XgRenderModelFt4TemplateEntry packet_templates[
    XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY];
static XgRenderModelFt4TemplateEntry descriptor_templates[
    XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY];
static XgRenderAddressLookupSlot packet_lookup[XG_RENDER_LOOKUP_WORD_CAPACITY];
static XgRenderAddressLookupSlot descriptor_lookup[
    XG_RENDER_LOOKUP_WORD_CAPACITY];
static uint16_t packet_lookup_epoch = 1u;
static uint16_t descriptor_lookup_epoch = 1u;
static uint16_t descriptor_packet_heads[
    XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY];
static uint32_t template_table_epoch = 1u;
static uint32_t packet_template_count;
static uint32_t descriptor_template_count;
static XgRenderModelPacketCopy packet_copy;
static XgRenderModelAnchorContext anchor_contexts[
    XG_RENDER_MODEL_ANCHOR_CONTEXT_CAPACITY];
static uint32_t anchor_context_count;

static const GuestRenderNativeSourceWriter ft4_2c_writers[] = {
    { UINT32_C(0x801e92b4), 0u, UINT32_C(0x801e92a8) },
    { UINT32_C(0x8007a8e4), 0u, UINT32_C(0x8007a830) },
    { UINT32_C(0x8007a9f4), 0u, UINT32_C(0x8007a9d4) },
};

static const GuestRenderNativeSourceWriter ft3_25_writers[] = {
    { UINT32_C(0x8002d9f8), 0u, UINT32_C(0x8002d998) },
};

static const GuestRenderNativeSourceWriter ft4_2e_writers[] = {
    { UINT32_C(0x80043c20), 0u, UINT32_C(0x800a6828) },
    { UINT32_C(0x801cef58), 0u, UINT32_C(0x801cee8c) },
    { UINT32_C(0x801e91f4), 0u, UINT32_C(0x801e91e8) },
    { UINT32_C(0x8007ab2c), 0u, UINT32_C(0x8007aae0) },
    { UINT32_C(0x80043c20), 0u, UINT32_C(0x8007aae0) },
    { UINT32_C(0x800a5690), 0u, UINT32_C(0x8007919c) },
    { UINT32_C(0x801ceddc), 0u, UINT32_C(0x801ced10) },
    { UINT32_C(0x800a6884), 0u, UINT32_C(0x800a6864) },
    { UINT32_C(0x80043c20), 0u, UINT32_C(0x8007a728) },
    { UINT32_C(0x8007a774), 0u, UINT32_C(0x8007a74c) },
    { UINT32_C(0x80043c20), 0u, UINT32_C(0x800a6170) },
};

static bool writer_is_one_of(
        GuestRenderNativeSourceWriter writer,
        const GuestRenderNativeSourceWriter *accepted, size_t count) {
    for (size_t index = 0u; index < count; ++index) {
        if (writer.pc == accepted[index].pc &&
            writer.function == accepted[index].function &&
            writer.return_address == accepted[index].return_address)
            return true;
    }
    return false;
}

bool xg_render_model_repository_ft3_writer_is_authorized(
        const GuestRenderNativeStreamMissContext *context) {
    if (context == NULL) return false;
    if (context->opcode != 0x25u) return true;
    return context->command_writer_valid && writer_is_one_of(
        context->command_writer, ft3_25_writers,
        sizeof(ft3_25_writers) / sizeof(ft3_25_writers[0]));
}

bool xg_render_model_repository_ft4_writer_is_authorized(
        const GuestRenderNativeStreamMissContext *context) {
    const GuestRenderNativeSourceWriter *accepted;
    size_t count;

    if (context == NULL) return false;
    if (context->opcode != 0x2cu && context->opcode != 0x2eu) return true;
    if (!context->command_writer_valid) return false;
    if (context->opcode == 0x2cu) {
        accepted = ft4_2c_writers;
        count = sizeof(ft4_2c_writers) / sizeof(ft4_2c_writers[0]);
    } else {
        accepted = ft4_2e_writers;
        count = sizeof(ft4_2e_writers) / sizeof(ft4_2e_writers[0]);
    }
    return writer_is_one_of(context->command_writer, accepted, count);
}

static uint32_t physical_word(uint32_t address) {
    return address & UINT32_C(0x001ffffc);
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool ranges_overlap(uint32_t left_start, uint32_t left_size,
                           uint32_t right_start, uint32_t right_size) {
    const uint64_t left = left_start & UINT32_C(0x1fffffff);
    const uint64_t right = right_start & UINT32_C(0x1fffffff);

    return left_size != 0u && right_size != 0u &&
        left < right + right_size && right < left + left_size;
}

static XgRenderModelFt4SourceRecord *ft4_source_upsert(uint32_t source_id) {
    const uint32_t indexed = xg_render_lookup_find(
        ft4_source_lookup, ft4_source_lookup_epoch, source_id,
        ft4_source_count);

    if (indexed != UINT32_MAX && ft4_sources[indexed].valid &&
        ft4_sources[indexed].source_id == source_id) {
        ft4_sources[indexed].semantic_ready = false;
        return &ft4_sources[indexed];
    }
    for (uint32_t index = 0u; index < ft4_source_count; ++index) {
        if (!ft4_sources[index].valid) {
            ft4_sources[index].semantic_ready = false;
            return &ft4_sources[index];
        }
    }
    if (ft4_source_count == XG_RENDER_MODEL_FT4_SOURCE_CAPACITY) return NULL;
    return &ft4_sources[ft4_source_count++];
}

static XgRenderModelFt3SourceRecord *ft3_source_upsert(uint32_t source_id) {
    const uint32_t indexed = xg_render_lookup_find(
        ft3_source_lookup, ft3_source_lookup_epoch, source_id,
        ft3_source_count);
    XgRenderModelFt3SourceRecord *recyclable = NULL;

    if (indexed != UINT32_MAX && ft3_sources[indexed].valid &&
        ft3_sources[indexed].source_id == source_id)
        return &ft3_sources[indexed];
    for (uint32_t index = 0u; index < ft3_source_count; ++index) {
        XgRenderModelFt3SourceRecord *candidate = &ft3_sources[index];

        if (candidate->valid && candidate->source_id == source_id)
            return candidate;
        if (!candidate->valid) return candidate;
        if (recyclable == NULL && !candidate->geometry_ready &&
            !candidate->link_pending)
            recyclable = candidate;
    }
    if (ft3_source_count == XG_RENDER_MODEL_FT3_SOURCE_CAPACITY) {
        if (recyclable == NULL) return NULL;
        xg_render_lookup_remove(
            ft3_source_lookup, ft3_source_lookup_epoch,
            recyclable->source_id,
            (uint32_t)(recyclable - ft3_sources));
        return recyclable;
    }
    return &ft3_sources[ft3_source_count++];
}

static XgRenderModelFt3SourceRecord *ft3_source_find(uint32_t source_id) {
    const uint32_t indexed = xg_render_lookup_find(
        ft3_source_lookup, ft3_source_lookup_epoch, source_id,
        ft3_source_count);

    if (indexed != UINT32_MAX && ft3_sources[indexed].valid &&
        ft3_sources[indexed].source_id == source_id)
        return &ft3_sources[indexed];
    for (uint32_t index = 0u; index < ft3_source_count; ++index) {
        if (ft3_sources[index].valid &&
            ft3_sources[index].source_id == source_id) {
            xg_render_lookup_put(
                ft3_source_lookup, ft3_source_lookup_epoch, source_id, index);
            return &ft3_sources[index];
        }
    }
    return NULL;
}

const XgRenderModelFt3SourceRecord *xg_render_model_repository_find_ft3_source(
        uint32_t source_id) {
    return ft3_source_find(source_id);
}

static bool material_position_matches_context(
        const XgRenderIrMaterialState *material,
        const XgRenderModelAnchorContext *context) {
    return material->draw_area_left == context->material.draw_area_left &&
        material->draw_area_top == context->material.draw_area_top &&
        material->draw_area_right == context->material.draw_area_right &&
        material->draw_area_bottom == context->material.draw_area_bottom &&
        material->draw_offset_x == context->material.draw_offset_x &&
        material->draw_offset_y == context->material.draw_offset_y;
}

static bool semantic_material_position_matches_context(
        const GpuRenderMaterial *material,
        const XgRenderModelAnchorContext *context) {
    return material->draw_area_left == context->material.draw_area_left &&
        material->draw_area_top == context->material.draw_area_top &&
        material->draw_area_right == context->material.draw_area_right &&
        material->draw_area_bottom == context->material.draw_area_bottom &&
        material->draw_offset_x == context->material.draw_offset_x &&
        material->draw_offset_y == context->material.draw_offset_y;
}

static bool source_identity_matches(
        uint32_t producer_id, uint32_t primitive_id,
        const GpuRenderSemantic *resolved) {
    return resolved->interpolation_identity.valid &&
        resolved->interpolation_identity.producer_id == producer_id &&
        resolved->interpolation_identity.primitive_id == primitive_id;
}

static bool record_producer_anchor(
        const XgRenderIrNativePrimitive *primitive, uint64_t scene_id,
        uint32_t producer_id, uint32_t primitive_id,
        const XgRenderModelRepositoryServices *services) {
    GpuRenderSemantic semantic;

    if (xg_render_backend_translate_primitive(primitive, &semantic) !=
            XG_RENDER_BACKEND_OK)
        return false;
    xg_render_semantic_set_interpolation_identity(
        &semantic, scene_id, producer_id, primitive_id);
    return services->submission.record_interpolation_anchors(&semantic);
}

static bool lifecycle_is_current(
        const XgRenderProducerLifecycle *lifecycle,
        const XgRenderModelRepositoryServices *services) {
    return services->lifecycle != NULL &&
        services->lifecycle->matches != NULL &&
        services->lifecycle->matches(lifecycle);
}

static bool record_anchor_context(
        const XgRenderModelAnchorContext *context, uint32_t skipped_primitive_id,
        bool skip_primitive,
        const XgRenderModelRepositoryServices *services) {
    for (uint32_t index = 0u; index < ft3_source_count; ++index) {
        const XgRenderModelFt3SourceRecord *record = &ft3_sources[index];

        if (!record->valid || !record->geometry_ready ||
            !record->interpolation_identity_valid ||
            record->interpolation_producer_id != context->producer_id ||
            (skip_primitive && record->interpolation_primitive_id ==
                skipped_primitive_id) ||
            !material_position_matches_context(
                &record->primitive.material, context) ||
            !lifecycle_is_current(&record->lifecycle, services))
            continue;
        if (!record_producer_anchor(
                &record->primitive, context->scene_id,
                record->interpolation_producer_id,
                record->interpolation_primitive_id, services))
            return false;
    }
    for (uint32_t index = 0u; index < ft4_source_count; ++index) {
        const XgRenderModelFt4SourceRecord *record = &ft4_sources[index];

        if (!record->valid || !record->interpolation_identity_valid ||
            record->interpolation_producer_id != context->producer_id ||
            (skip_primitive && record->interpolation_primitive_id ==
                skipped_primitive_id) ||
            !material_position_matches_context(
                &record->primitive.material, context) ||
            !lifecycle_is_current(&record->lifecycle, services))
            continue;
        if (!record_producer_anchor(
                &record->primitive, context->scene_id,
                record->interpolation_producer_id,
                record->interpolation_primitive_id, services))
            return false;
    }
    return true;
}

static XgRenderModelAnchorContext *observe_anchor_context(
        const GpuRenderSemantic *resolved) {
    XgRenderModelAnchorContext *context = NULL;

    for (uint32_t index = 0u; index < anchor_context_count; ++index) {
        if (anchor_contexts[index].producer_id ==
                resolved->interpolation_identity.producer_id &&
            semantic_material_position_matches_context(
                &resolved->material, &anchor_contexts[index])) {
            context = &anchor_contexts[index];
            break;
        }
    }
    if (context == NULL) {
        if (anchor_context_count == XG_RENDER_MODEL_ANCHOR_CONTEXT_CAPACITY)
            return NULL;
        context = &anchor_contexts[anchor_context_count++];
        memset(context, 0, sizeof(*context));
        context->producer_id = resolved->interpolation_identity.producer_id;
    }
    if (context->scene_id != resolved->interpolation_identity.scene_id) {
        context->scene_id = resolved->interpolation_identity.scene_id;
        context->material = resolved->material;
    }
    return context;
}

bool xg_render_model_repository_record_resolved_producer_anchors(
        uint64_t command_id, const GpuRenderSemantic *resolved,
        const XgRenderModelRepositoryServices *services) {
    bool model_source = false;
    XgRenderModelAnchorContext *context;

    if (command_id > UINT32_MAX || resolved == NULL ||
        !resolved->interpolation_identity.valid)
        return true;
    for (uint32_t index = 0u; index < ft3_source_count; ++index) {
        const XgRenderModelFt3SourceRecord *record = &ft3_sources[index];

        if (record->valid && record->geometry_ready &&
            physical_address_equals(
                record->source_id, (uint32_t)command_id) &&
            record->interpolation_identity_valid &&
            source_identity_matches(
                record->interpolation_producer_id,
                record->interpolation_primitive_id, resolved)) {
            model_source = true;
            break;
        }
    }
    if (!model_source) {
        const uint32_t indexed = xg_render_lookup_find(
            ft4_source_lookup, ft4_source_lookup_epoch,
            (uint32_t)command_id, ft4_source_count);
        const XgRenderModelFt4SourceRecord *record = indexed != UINT32_MAX
            ? &ft4_sources[indexed] : NULL;

        model_source = record != NULL && record->valid &&
            record->interpolation_identity_valid &&
            source_identity_matches(
                record->interpolation_producer_id,
                record->interpolation_primitive_id, resolved);
    }
    if (!model_source) return true;
    if (services == NULL ||
        services->submission.record_interpolation_anchors == NULL)
        return false;
    context = observe_anchor_context(resolved);
    return context != NULL && record_anchor_context(
        context, resolved->interpolation_identity.primitive_id, true, services);
}

bool xg_render_model_repository_record_active_producer_anchors(
        const XgRenderModelRepositoryServices *services) {
    const uint64_t scene_id = services != NULL &&
            services->submission.interpolation_scene != NULL
        ? services->submission.interpolation_scene() : 0u;
    bool recorded = true;

    if (scene_id == 0u || services->submission.record_interpolation_anchors == NULL)
        return anchor_context_count == 0u;
    for (uint32_t index = 0u; index < anchor_context_count; ++index) {
        XgRenderModelAnchorContext *context = &anchor_contexts[index];

        if (recorded && context->scene_id == scene_id)
            recorded = record_anchor_context(context, 0u, false, services);
    }
    return recorded;
}

static void publish_resource(
        uint32_t source_id, bool ft4,
        const XgRenderModelSourcePublication *publication,
        const XgRenderModelRepositoryServices *services) {
    if (publication == NULL || services == NULL) return;
    if (publication->register_replay) {
        if (ft4 && services->submission.register_ft4_replay_source != NULL)
            services->submission.register_ft4_replay_source(source_id);
        if (!ft4 && services->submission.register_ft3_replay_source != NULL)
            services->submission.register_ft3_replay_source(source_id);
    }
    if (publication->resource_size != 0u &&
        services->resources.watch != NULL)
        services->resources.watch(
            publication->resource_address, publication->resource_size);
    if (!ft4 && publication->descriptor_size != 0u &&
        services->resources.watch_ft3_descriptor != NULL)
        services->resources.watch_ft3_descriptor(
            publication->descriptor_address, publication->descriptor_size);
}

bool xg_render_model_repository_store_ft3_source(
        const XgRenderModelFt3SourceRecord *record,
        const XgRenderModelSourcePublication *publication,
        const XgRenderModelRepositoryServices *services) {
    XgRenderModelFt3SourceRecord *target;

    if (record == NULL || !record->valid) return false;
    target = ft3_source_upsert(record->source_id);
    if (target == NULL) return false;
    *target = *record;
    xg_render_lookup_put(
        ft3_source_lookup, ft3_source_lookup_epoch, target->source_id,
        (uint32_t)(target - ft3_sources));
    publish_resource(target->source_id, false, publication, services);
    return true;
}

bool xg_render_model_repository_store_ft4_source(
        const XgRenderModelFt4SourceRecord *record,
        const XgRenderModelSourcePublication *publication,
        const XgRenderModelRepositoryServices *services) {
    XgRenderModelFt4SourceRecord *target;

    if (record == NULL || !record->valid) return false;
    target = ft4_source_upsert(record->source_id);
    if (target == NULL) return false;
    *target = *record;
    target->semantic_ready = false;
    xg_render_lookup_put(
        ft4_source_lookup, ft4_source_lookup_epoch, target->source_id,
        (uint32_t)(target - ft4_sources));
    publish_resource(target->source_id, true, publication, services);
    return true;
}

bool xg_render_model_repository_store_ft4_sources(
        const XgRenderModelFt4SourceRecord *records,
        const XgRenderModelSourcePublication *publications, uint32_t count,
        const XgRenderModelRepositoryServices *services) {
    XgRenderModelFt4SourceRecord *targets[
        XG_RENDER_MODEL_FT4_SOURCE_CAPACITY];
    uint32_t reserved = 0u;

    if (((records == NULL || publications == NULL) && count != 0u) ||
        count > XG_RENDER_MODEL_FT4_SOURCE_CAPACITY)
        return false;
    for (; reserved < count; ++reserved) {
        targets[reserved] = ft4_source_upsert(records[reserved].source_id);
        if (targets[reserved] == NULL) {
            for (uint32_t rollback = 0u; rollback < reserved; ++rollback)
                targets[rollback]->valid = false;
            return false;
        }
        targets[reserved]->source_id = records[reserved].source_id;
        targets[reserved]->valid = true;
    }
    for (uint32_t index = 0u; index < count; ++index) {
        *targets[index] = records[index];
        targets[index]->semantic_ready = false;
        xg_render_lookup_put(
            ft4_source_lookup, ft4_source_lookup_epoch,
            targets[index]->source_id,
            (uint32_t)(targets[index] - ft4_sources));
        publish_resource(
            targets[index]->source_id, true, &publications[index], services);
    }
    return true;
}

static void remove_ft3_source(uint32_t source_id) {
    XgRenderModelFt3SourceRecord *record = ft3_source_find(source_id);

    if (record == NULL) return;
    record->valid = false;
    xg_render_lookup_remove(
        ft3_source_lookup, ft3_source_lookup_epoch, source_id,
        (uint32_t)(record - ft3_sources));
}

void xg_render_model_repository_finish_ft3_link(
        uint32_t source_id, bool linked,
        const XgRenderModelSourcePublication *publication,
        const XgRenderModelRepositoryServices *services) {
    XgRenderModelFt3SourceRecord *record = ft3_source_find(source_id);

    if (record == NULL || !record->link_pending) return;
    if (!linked) {
        remove_ft3_source(source_id);
        return;
    }
    record->link_pending = false;
    record->geometry_ready = true;
    publish_resource(source_id, false, publication, services);
}

void xg_render_model_repository_clear_ft3_sources(
        const XgRenderModelRepositoryServices *services) {
    ft3_source_count = 0u;
    xg_render_lookup_reset(ft3_source_lookup, &ft3_source_lookup_epoch);
    if (services != NULL && services->resources.reset_ft3_descriptors != NULL)
        services->resources.reset_ft3_descriptors();
}

void xg_render_model_repository_clear_ft4_sources(void) {
    ft4_source_count = 0u;
    anchor_context_count = 0u;
    xg_render_lookup_reset(ft4_source_lookup, &ft4_source_lookup_epoch);
}

uint32_t xg_render_model_repository_ft3_source_count(void) {
    return ft3_source_count;
}

void xg_render_model_repository_retain_resident_sources(
        const XgRenderModelRepositoryServices *services) {
    uint32_t retained = 0u;

    for (uint32_t index = 0u; index < ft4_source_count; ++index) {
        const XgRenderModelFt4SourceRecord *record = &ft4_sources[index];

        if (!record->valid || record->lifecycle.scene_resource != 0u) continue;
        if (retained != index) ft4_sources[retained] = *record;
        ++retained;
    }
    ft4_source_count = retained;
    xg_render_lookup_reset(ft4_source_lookup, &ft4_source_lookup_epoch);
    for (uint32_t index = 0u; index < retained; ++index) {
        xg_render_lookup_put(
            ft4_source_lookup, ft4_source_lookup_epoch,
            ft4_sources[index].source_id, index);
        if (services != NULL &&
            services->submission.register_ft4_replay_source != NULL)
            services->submission.register_ft4_replay_source(
                ft4_sources[index].source_id);
    }

    retained = 0u;
    for (uint32_t index = 0u; index < ft3_source_count; ++index) {
        const XgRenderModelFt3SourceRecord *record = &ft3_sources[index];

        if (!record->valid || record->lifecycle.scene_resource != 0u) continue;
        if (retained != index) ft3_sources[retained] = *record;
        ++retained;
    }
    ft3_source_count = retained;
    xg_render_lookup_reset(ft3_source_lookup, &ft3_source_lookup_epoch);
    for (uint32_t index = 0u; index < retained; ++index) {
        xg_render_lookup_put(
            ft3_source_lookup, ft3_source_lookup_epoch,
            ft3_sources[index].source_id, index);
        if (services != NULL &&
            services->submission.register_ft3_replay_source != NULL)
            services->submission.register_ft3_replay_source(
                ft3_sources[index].source_id);
    }
}

static uint32_t template_slot(uint32_t address) {
    return ((address >> 2u) ^ (address >> 13u)) &
        (XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY - 1u);
}

static bool template_is_current(
        const XgRenderModelFt4TemplateEntry *entry) {
    return entry != NULL && entry->material.valid &&
        entry->table_epoch == template_table_epoch;
}

static XgRenderModelFt4TemplateEntry *template_direct_find(
        XgRenderModelFt4TemplateEntry *templates,
        const XgRenderAddressLookupSlot *lookup, uint16_t lookup_epoch,
        uint32_t address, bool descriptor_key) {
    const uint32_t key = physical_word(address);
    const uint32_t index = xg_render_lookup_find(
        lookup, lookup_epoch, key, XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY);
    XgRenderModelFt4TemplateEntry *entry;

    if (index == UINT32_MAX) return NULL;
    entry = &templates[index];
    if (!template_is_current(entry) ||
        (descriptor_key ? entry->material.descriptor_address :
                          entry->material.packet_address) != key)
        return NULL;
    return entry;
}

static XgRenderModelFt4TemplateEntry *template_find(
        XgRenderModelFt4TemplateEntry *templates,
        XgRenderAddressLookupSlot *lookup, uint16_t lookup_epoch,
        uint32_t address, bool descriptor_key, bool insert) {
    const uint32_t key = physical_word(address);
    XgRenderModelFt4TemplateEntry *direct = template_direct_find(
        templates, lookup, lookup_epoch, key, descriptor_key);
    XgRenderModelFt4TemplateEntry *first_invalid = NULL;
    uint32_t slot = template_slot(key);

    if (direct != NULL || !insert) return direct;
    for (uint32_t probe = 0u;
         probe < XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY; ++probe) {
        XgRenderModelFt4TemplateEntry *entry = &templates[slot];

        if (entry->table_epoch != template_table_epoch) {
            if (first_invalid != NULL) return first_invalid;
            entry->material.valid = false;
            entry->table_epoch = template_table_epoch;
            return entry;
        }
        if (!entry->material.valid) {
            if (first_invalid == NULL) first_invalid = entry;
        } else if ((descriptor_key ? entry->material.descriptor_address :
                                    entry->material.packet_address) == key) {
            return entry;
        }
        slot = (slot + 1u) &
            (XG_RENDER_MODEL_FT4_TEMPLATE_CAPACITY - 1u);
    }
    return first_invalid;
}

static void packet_template_unlink(XgRenderModelFt4TemplateEntry *entry) {
    uint16_t *link;

    if (!template_is_current(entry)) return;
    link = &descriptor_packet_heads[
        template_slot(entry->material.descriptor_address)];
    while (*link != 0u) {
        XgRenderModelFt4TemplateEntry *candidate =
            &packet_templates[*link - 1u];

        if (candidate == entry) {
            *link = candidate->descriptor_next;
            candidate->descriptor_next = 0u;
            return;
        }
        link = &candidate->descriptor_next;
    }
}

bool xg_render_model_repository_store_template(
        const XgRenderModelFt4Template *captured) {
    XgRenderModelFt4TemplateEntry *packet;
    XgRenderModelFt4TemplateEntry *descriptor;
    uint32_t packet_index;
    uint32_t descriptor_index;
    uint32_t bucket;
    bool new_packet;
    bool new_descriptor;

    if (captured == NULL || !captured->valid) return false;
    packet = template_find(
        packet_templates, packet_lookup, packet_lookup_epoch,
        captured->packet_address, false, true);
    descriptor = template_find(
        descriptor_templates, descriptor_lookup, descriptor_lookup_epoch,
        captured->descriptor_address, true, true);
    if (packet == NULL || descriptor == NULL) return false;
    new_packet = !template_is_current(packet);
    new_descriptor = !template_is_current(descriptor);
    packet_index = (uint32_t)(packet - packet_templates);
    descriptor_index = (uint32_t)(descriptor - descriptor_templates);
    if (!new_packet) {
        xg_render_lookup_remove(
            packet_lookup, packet_lookup_epoch, packet->material.packet_address,
            packet_index);
        packet_template_unlink(packet);
    }
    packet->material = *captured;
    packet->material.packet_address = physical_word(captured->packet_address);
    packet->material.descriptor_address =
        physical_word(captured->descriptor_address);
    packet->table_epoch = template_table_epoch;
    bucket = template_slot(packet->material.descriptor_address);
    packet->descriptor_next = descriptor_packet_heads[bucket];
    descriptor_packet_heads[bucket] = (uint16_t)(packet_index + 1u);
    xg_render_lookup_put(
        packet_lookup, packet_lookup_epoch, packet->material.packet_address,
        packet_index);

    if (!new_descriptor)
        xg_render_lookup_remove(
            descriptor_lookup, descriptor_lookup_epoch,
            descriptor->material.descriptor_address, descriptor_index);
    descriptor->material = packet->material;
    descriptor->table_epoch = template_table_epoch;
    descriptor->descriptor_next = 0u;
    xg_render_lookup_put(
        descriptor_lookup, descriptor_lookup_epoch,
        descriptor->material.descriptor_address, descriptor_index);
    if (new_packet) ++packet_template_count;
    if (new_descriptor) ++descriptor_template_count;
    return true;
}

static const XgRenderModelFt4Template *find_template(
        uint32_t address, bool descriptor_key,
        GuestRenderRenderMode render_mode,
        const XgRenderModelRepositoryServices *services) {
    XgRenderModelFt4TemplateEntry *entry = template_direct_find(
        descriptor_key ? descriptor_templates : packet_templates,
        descriptor_key ? descriptor_lookup : packet_lookup,
        descriptor_key ? descriptor_lookup_epoch : packet_lookup_epoch,
        address, descriptor_key);

    if (entry == NULL) return NULL;
    if (render_mode == GUEST_RENDER_RENDER_NATIVE &&
        (services == NULL || services->lifecycle == NULL ||
         services->lifecycle->matches == NULL ||
         !services->lifecycle->matches(&entry->material.lifecycle)))
        return NULL;
    return &entry->material;
}

const XgRenderModelFt4Template *xg_render_model_repository_find_packet_template(
        uint32_t packet_address, GuestRenderRenderMode render_mode,
        const XgRenderModelRepositoryServices *services) {
    return find_template(packet_address, false, render_mode, services);
}

const XgRenderModelFt4Template *
xg_render_model_repository_find_descriptor_template(
        uint32_t descriptor_address, GuestRenderRenderMode render_mode,
        const XgRenderModelRepositoryServices *services) {
    return find_template(descriptor_address, true, render_mode, services);
}

bool xg_render_model_repository_packet_template_present(
        uint32_t packet_address) {
    return template_direct_find(
        packet_templates, packet_lookup, packet_lookup_epoch,
        packet_address, false) != NULL;
}

bool xg_render_model_repository_descriptor_template_present(
        uint32_t descriptor_address) {
    return template_direct_find(
        descriptor_templates, descriptor_lookup, descriptor_lookup_epoch,
        descriptor_address, true) != NULL;
}

void xg_render_model_repository_reset_templates(void) {
    packet_template_count = 0u;
    descriptor_template_count = 0u;
    xg_render_lookup_reset(packet_lookup, &packet_lookup_epoch);
    xg_render_lookup_reset(descriptor_lookup, &descriptor_lookup_epoch);
    memset(descriptor_packet_heads, 0, sizeof(descriptor_packet_heads));
    if (template_table_epoch == UINT32_MAX) {
        memset(packet_templates, 0, sizeof(packet_templates));
        memset(descriptor_templates, 0, sizeof(descriptor_templates));
        template_table_epoch = 1u;
    } else {
        ++template_table_epoch;
    }
}

static void invalidate_packet_template(XgRenderModelFt4TemplateEntry *entry) {
    const uint32_t index = (uint32_t)(entry - packet_templates);

    if (!template_is_current(entry)) return;
    xg_render_lookup_remove(
        packet_lookup, packet_lookup_epoch, entry->material.packet_address,
        index);
    packet_template_unlink(entry);
    entry->material.valid = false;
    --packet_template_count;
}

static void invalidate_descriptor_packets(uint32_t descriptor_address) {
    const uint32_t key = physical_word(descriptor_address);
    uint16_t *link = &descriptor_packet_heads[template_slot(key)];

    while (*link != 0u) {
        XgRenderModelFt4TemplateEntry *entry =
            &packet_templates[*link - 1u];

        if (entry->material.descriptor_address == key) {
            *link = entry->descriptor_next;
            entry->descriptor_next = 0u;
            if (template_is_current(entry)) {
                xg_render_lookup_remove(
                    packet_lookup, packet_lookup_epoch,
                    entry->material.packet_address,
                    (uint32_t)(entry - packet_templates));
                entry->material.valid = false;
                --packet_template_count;
            }
        } else {
            link = &entry->descriptor_next;
        }
    }
}

static void invalidate_packet_candidates(uint32_t address, uint32_t size) {
    const uint64_t write_begin = address & UINT32_C(0x1fffffff);
    uint64_t write_end = write_begin + size;
    uint64_t first;
    uint64_t last;

    if (size == 0u || write_begin >= UINT32_C(0x200000)) return;
    if (write_end > UINT32_C(0x200000)) write_end = UINT32_C(0x200000);
    first = write_begin >= 0x28u ? write_begin - 0x28u + 1u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    last = (write_end - 1u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        XgRenderModelFt4TemplateEntry *packet = template_direct_find(
            packet_templates, packet_lookup, packet_lookup_epoch,
            (uint32_t)candidate, false);

        if (packet != NULL) {
            const uint32_t packet_address = packet->material.packet_address;
            const uint32_t descriptor_address =
                packet->material.descriptor_address;
            XgRenderModelFt4TemplateEntry *descriptor = template_direct_find(
                descriptor_templates, descriptor_lookup,
                descriptor_lookup_epoch, descriptor_address, true);
            invalidate_packet_template(packet);
            if (descriptor != NULL &&
                descriptor->material.packet_address == packet_address) {
                xg_render_lookup_remove(
                    descriptor_lookup, descriptor_lookup_epoch,
                    descriptor->material.descriptor_address,
                    (uint32_t)(descriptor - descriptor_templates));
                descriptor->material.valid = false;
                --descriptor_template_count;
            }
        }
    }
}

static void invalidate_descriptor_candidates(uint32_t address, uint32_t size) {
    const uint64_t write_begin = address & UINT32_C(0x1fffffff);
    uint64_t write_end = write_begin + size;
    uint64_t first;
    uint64_t last;

    if (size == 0u || write_begin >= UINT32_C(0x200000)) return;
    if (write_end > UINT32_C(0x200000)) write_end = UINT32_C(0x200000);
    first = write_begin >= 12u ? write_begin - 11u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    last = (write_end - 1u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        XgRenderModelFt4TemplateEntry *descriptor = template_direct_find(
            descriptor_templates, descriptor_lookup, descriptor_lookup_epoch,
            (uint32_t)candidate, true);

        invalidate_descriptor_packets((uint32_t)candidate);
        if (descriptor != NULL) {
            xg_render_lookup_remove(
                descriptor_lookup, descriptor_lookup_epoch,
                descriptor->material.descriptor_address,
                (uint32_t)(descriptor - descriptor_templates));
            descriptor->material.valid = false;
            --descriptor_template_count;
        }
    }
}

void xg_render_model_repository_invalidate_overlapping(
        uint32_t address, uint32_t size,
        const XgRenderModelRepositoryServices *services) {
    const uint64_t write_begin = address & UINT32_C(0x1fffffff);
    uint64_t write_end = write_begin + size;
    uint64_t first;
    uint64_t last;

    if (size == 0u || write_begin >= UINT32_C(0x200000)) return;
    if (write_end > UINT32_C(0x200000)) write_end = UINT32_C(0x200000);
    first = write_begin >= 0x28u ? write_begin - 0x28u + 1u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    last = (write_end - 1u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        const uint32_t source_id = (uint32_t)candidate + 4u;
        const uint32_t index = xg_render_lookup_find(
            ft4_source_lookup, ft4_source_lookup_epoch, source_id,
            ft4_source_count);
        if (index != UINT32_MAX && ranges_overlap(
                ft4_sources[index].source_id, 0x24u, address, size)) {
            ft4_sources[index].valid = false;
            xg_render_lookup_remove(
                ft4_source_lookup, ft4_source_lookup_epoch,
                ft4_sources[index].source_id, index);
        }
    }
    first = write_begin >= 0x20u ? write_begin - 0x20u + 1u : 0u;
    first = (first + 3u) & ~UINT64_C(3);
    for (uint64_t candidate = first; candidate <= last; candidate += 4u) {
        const uint32_t source_id = (uint32_t)candidate + 4u;
        const uint32_t index = xg_render_lookup_find(
            ft3_source_lookup, ft3_source_lookup_epoch, source_id,
            ft3_source_count);
        if (index != UINT32_MAX && ft3_sources[index].geometry_ready &&
            ft3_sources[index].source_id >= 4u && ranges_overlap(
                ft3_sources[index].source_id, 0x1cu, address, size)) {
            ft3_sources[index].geometry_ready = false;
            ft3_sources[index].link_pending = false;
        }
    }
    if (services != NULL &&
        services->resources.ft3_descriptor_overlaps != NULL &&
        services->resources.ft3_descriptor_overlaps(address, size)) {
        for (uint32_t index = 0u; index < ft3_source_count; ++index) {
            XgRenderModelFt3SourceRecord *record = &ft3_sources[index];

            if (!record->valid || record->descriptor_address == 0u ||
                !ranges_overlap(
                    record->descriptor_address, 10u, address, size))
                continue;
            record->valid = false;
            xg_render_lookup_remove(
                ft3_source_lookup, ft3_source_lookup_epoch,
                record->source_id, index);
        }
    }
    if (packet_template_count != 0u || descriptor_template_count != 0u) {
        invalidate_packet_candidates(address, size);
        invalidate_descriptor_candidates(address, size);
    }
}

void xg_render_model_repository_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    const bool model_code = xg_render_invalidation_has_code_class(
        event, PSX_XG_RENDER_CODE_WRITE_MODEL_FT4);

    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.resource_mutation)
            xg_render_model_repository_invalidate_overlapping(
                event->address, event->size, services->model_repository);
        if (model_code) {
            xg_render_model_repository_clear_ft4_sources();
            xg_render_model_repository_clear_ft3_sources(
                services->model_repository);
        }
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE) {
        xg_render_model_repository_clear_ft4_sources();
        xg_render_model_repository_clear_ft3_sources(
            services->model_repository);
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
               event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH ||
               event->kind == XG_RENDER_INVALIDATION_RESOURCE_OVERLAP) {
        xg_render_model_repository_retain_resident_sources(
            services->model_repository);
    }
}

static int16_t low_s16(uint32_t value) {
    return (int16_t)(uint16_t)value;
}

static bool materialize_ft3_packet(
        CPUState *cpu, uint32_t packet_address,
        XgRenderModelFt3SourceRecord *record) {
    XgRenderIrNativePrimitive primitive = {0};
    GpuDrawState draw = {0};
    uint32_t words[8];
    uint8_t opcode;

    if (cpu == NULL || cpu->read_word == NULL || record == NULL ||
        record->descriptor_address == 0u)
        return false;
    for (uint32_t word = 0u; word < 8u; ++word)
        words[word] = cpu->read_word(packet_address + word * 4u);
    opcode = (uint8_t)(words[1] >> 24u);
    if ((words[0] >> 24u) != 7u || opcode < 0x24u || opcode > 0x27u)
        return false;
    gpu_get_draw_state(&draw);
    xg_render_material_apply_draw_state(&primitive.material, &draw);
    primitive.material.tpage = (uint16_t)(words[5] >> 16u);
    primitive.material.texture_page_x = primitive.material.tpage & 0x0fu;
    primitive.material.texture_page_y = (primitive.material.tpage >> 4u) & 1u;
    primitive.material.texture_depth = (XgRenderIrTextureDepth)(
        (primitive.material.tpage >> 7u) & 3u);
    primitive.material.blend_mode = (XgRenderIrBlendMode)(
        (primitive.material.tpage >> 5u) & 3u);
    primitive.material.clut_x = ((uint16_t)(words[3] >> 16u) & 0x3fu) << 4u;
    primitive.material.clut_y = (uint16_t)(words[3] >> 16u) >> 6u;
    primitive.material.shading = XG_RENDER_IR_SHADING_FLAT;
    primitive.material.textured = true;
    primitive.material.raw_texture = (opcode & 1u) != 0u;
    primitive.material.semi_transparent = (opcode & 2u) != 0u;
    primitive.triangle_count = 1u;
    primitive.triangles[0].split_count = 1u;
    for (uint32_t vertex = 0u; vertex < 3u; ++vertex) {
        const uint32_t xy = words[2u + vertex * 2u];
        const uint16_t uv = (uint16_t)words[3u + vertex * 2u];
        XgRenderIrVertex *destination =
            &primitive.triangles[0].vertices[vertex];

        destination->x = (int32_t)low_s16(xy) * INT32_C(65536);
        destination->y = (int32_t)low_s16(xy >> 16u) * INT32_C(65536);
        destination->u = (int32_t)(uint8_t)uv * INT32_C(65536);
        destination->v =
            (int32_t)(uint8_t)(uv >> 8u) * INT32_C(65536);
        destination->r = (uint8_t)words[1];
        destination->g = (uint8_t)(words[1] >> 8u);
        destination->b = (uint8_t)(words[1] >> 16u);
    }
    record->primitive = primitive;
    record->material_word = words[1];
    record->uv[0] = (uint16_t)words[3];
    record->uv[1] = (uint16_t)words[5];
    record->uv[2] = (uint16_t)words[7];
    record->tpage = (uint16_t)(words[5] >> 16u);
    record->clut = (uint16_t)(words[3] >> 16u);
    record->link_pending = false;
    record->geometry_ready = true;
    return true;
}

void xg_render_model_repository_begin_packet_copy(
        CPUState *cpu, GuestRenderRenderMode render_mode) {
    packet_copy.active = false;
    if (cpu == NULL || render_mode != GUEST_RENDER_RENDER_NATIVE ||
        cpu->gpr[6] == 0u || cpu->gpr[4] > UINT32_MAX - cpu->gpr[6] ||
        cpu->gpr[5] > UINT32_MAX - cpu->gpr[6])
        return;
    packet_copy = (XgRenderModelPacketCopy){
        .owner_cpu = cpu,
        .destination = cpu->gpr[4],
        .source = cpu->gpr[5],
        .size = cpu->gpr[6],
        .active = true,
    };
}

void xg_render_model_repository_finish_packet_copy(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderModelRepositoryServices *services) {
    const XgRenderModelPacketCopy copy = packet_copy;
    const uint32_t destination_physical =
        copy.destination & UINT32_C(0x1fffffff);
    const uint32_t source_physical = copy.source & UINT32_C(0x1fffffff);
    const uint32_t original_ft3_count = ft3_source_count;
    const uint32_t original_ft4_count = ft4_source_count;

    (void)render_mode;
    if (!packet_copy.active) return;
    packet_copy.active = false;
    if (cpu == NULL || cpu != copy.owner_cpu || cpu->read_word == NULL ||
        cpu->gpr[2] != copy.destination ||
        cpu->gpr[4] != copy.destination + copy.size ||
        cpu->gpr[5] != copy.source + copy.size || cpu->gpr[6] != 0u ||
        copy.size < 0x20u ||
        source_physical > UINT32_C(0x200000) - copy.size ||
        destination_physical > UINT32_C(0x200000) - copy.size)
        return;
    for (uint32_t index = 0u; index < original_ft3_count; ++index) {
        const XgRenderModelFt3SourceRecord *record = &ft3_sources[index];
        const uint32_t packet = record->source_id - 4u;

        if (!record->valid ||
            (!record->geometry_ready && !record->link_pending &&
             record->descriptor_address == 0u) ||
            record->source_id < 4u || packet < source_physical ||
            packet > source_physical + copy.size - 0x20u)
            continue;
        for (uint32_t word = 0u; word < 8u; ++word) {
            if (cpu->read_word(
                    copy.source + (packet - source_physical) + word * 4u) !=
                cpu->read_word(
                    copy.destination + (packet - source_physical) + word * 4u))
                return;
        }
    }
    for (uint32_t index = 0u; index < original_ft4_count; ++index) {
        const XgRenderModelFt4SourceRecord *record = &ft4_sources[index];
        const uint32_t packet = record->source_id - 4u;

        if (!record->valid || record->source_id < 4u || copy.size < 0x28u ||
            packet < source_physical ||
            packet > source_physical + copy.size - 0x28u)
            continue;
        for (uint32_t word = 0u; word < 10u; ++word) {
            if (cpu->read_word(
                    copy.source + (packet - source_physical) + word * 4u) !=
                cpu->read_word(
                    copy.destination + (packet - source_physical) + word * 4u))
                return;
        }
    }
    for (uint32_t index = 0u; index < original_ft3_count; ++index) {
        const XgRenderModelFt3SourceRecord *record = &ft3_sources[index];
        const uint32_t packet = record->source_id - 4u;
        XgRenderModelFt3SourceRecord cloned;
        XgRenderModelSourcePublication publication;

        if (!record->valid ||
            (!record->geometry_ready && !record->link_pending &&
             record->descriptor_address == 0u) ||
            record->source_id < 4u || packet < source_physical ||
            packet > source_physical + copy.size - 0x20u)
            continue;
        cloned = *record;
        cloned.source_id = destination_physical +
            (packet - source_physical) + 4u;
        if (!cloned.geometry_ready && !materialize_ft3_packet(
                cpu, copy.destination + (packet - source_physical), &cloned))
            continue;
        publication = (XgRenderModelSourcePublication){
            .resource_address = cloned.source_id - 4u,
            .resource_size = 0x20u,
            .register_replay = true,
        };
        if (!xg_render_model_repository_store_ft3_source(
                &cloned, &publication, services))
            return;
    }
    for (uint32_t index = 0u; index < original_ft4_count; ++index) {
        const XgRenderModelFt4SourceRecord *record = &ft4_sources[index];
        const uint32_t packet = record->source_id - 4u;
        XgRenderModelFt4SourceRecord cloned;
        XgRenderModelSourcePublication publication;

        if (!record->valid || record->source_id < 4u || copy.size < 0x28u ||
            packet < source_physical ||
            packet > source_physical + copy.size - 0x28u)
            continue;
        cloned = *record;
        cloned.source_id = destination_physical +
            (packet - source_physical) + 4u;
        cloned.semantic_ready = false;
        publication = (XgRenderModelSourcePublication){
            .resource_address = cloned.source_id - 4u,
            .resource_size = 0x28u,
            .register_replay = true,
        };
        if (!xg_render_model_repository_store_ft4_source(
                &cloned, &publication, services))
            return;
    }
}

XgRenderModelReplayResult xg_render_model_repository_resolve_ft3(
        const GuestRenderNativeStreamMissContext *context,
        GuestRenderRenderMode render_mode, GpuRenderSemantic *out_semantic,
        const XgRenderModelRepositoryServices *services) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    XgRenderModelFt3SourceRecord *record = NULL;
    bool invalid_record = false;
    uint32_t lookup_key;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        context->opcode < 0x24u || context->opcode > 0x27u ||
        context->word_count != 7u ||
        !xg_render_model_repository_ft3_writer_is_authorized(context))
        return XG_RENDER_MODEL_REPLAY_NOT_APPLICABLE;
    if (xg_render_lookup_key((uint32_t)command_id, &lookup_key)) {
        const uint32_t indexed = xg_render_lookup_find(
            ft3_source_lookup, ft3_source_lookup_epoch,
            (uint32_t)command_id, ft3_source_count);
        if (indexed != UINT32_MAX) record = &ft3_sources[indexed];
    }
    if (record == NULL) {
        for (uint32_t slot = 0u; slot < ft3_source_count; ++slot) {
            if (!physical_address_equals(
                    ft3_sources[slot].source_id, (uint32_t)command_id))
                continue;
            if (!ft3_sources[slot].valid) {
                invalid_record = true;
                continue;
            }
            record = &ft3_sources[slot];
            xg_render_lookup_put(
                ft3_source_lookup, ft3_source_lookup_epoch,
                record->source_id, slot);
            break;
        }
        if (record == NULL)
            return invalid_record ? XG_RENDER_MODEL_REPLAY_LOOKUP_INVALID :
                XG_RENDER_MODEL_REPLAY_LOOKUP_ABSENT;
    }
    if (!record->valid || !record->geometry_ready ||
        !physical_address_equals(record->source_id, (uint32_t)command_id))
        return XG_RENDER_MODEL_REPLAY_RECORD_REJECTED;
    if (services == NULL || services->lifecycle == NULL ||
        services->lifecycle->replay_container_matches_command == NULL ||
        !services->lifecycle->replay_container_matches_command(context))
        return XG_RENDER_MODEL_REPLAY_CONTAINER_REJECTED;
    if (services->lifecycle->matches_replay == NULL ||
        !services->lifecycle->matches_replay(&record->lifecycle, context))
        return XG_RENDER_MODEL_REPLAY_LIFECYCLE_REJECTED;
    if (xg_render_backend_translate_primitive(
            &record->primitive, out_semantic) != XG_RENDER_BACKEND_OK)
        return XG_RENDER_MODEL_REPLAY_TRANSLATE_REJECTED;
    if (record->interpolation_identity_valid &&
        services->submission.interpolation_scene != NULL)
        xg_render_semantic_set_interpolation_identity(
            out_semantic, services->submission.interpolation_scene(),
            record->interpolation_producer_id,
            record->interpolation_primitive_id);
    out_semantic->material.raw_texture = (context->opcode & 1u) != 0u;
    out_semantic->material.semi_transparent = (context->opcode & 2u) != 0u;
    return XG_RENDER_MODEL_REPLAY_RESOLVED;
}

XgRenderModelReplayResult xg_render_model_repository_resolve_ft4(
        const GuestRenderNativeStreamMissContext *context,
        GuestRenderRenderMode render_mode, GpuRenderSemantic *out_semantic,
        const XgRenderModelRepositoryServices *services) {
    const uint64_t command_id = context != NULL ? context->command_id : 0u;
    uint32_t indexed;
    XgRenderModelFt4SourceRecord *record;

    if (out_semantic == NULL || context == NULL || command_id > UINT32_MAX ||
        render_mode != GUEST_RENDER_RENDER_NATIVE ||
        context->opcode < 0x2cu || context->opcode > 0x2fu ||
        context->word_count != 9u ||
        !xg_render_model_repository_ft4_writer_is_authorized(context))
        return XG_RENDER_MODEL_REPLAY_NOT_APPLICABLE;
    indexed = xg_render_lookup_find(
        ft4_source_lookup, ft4_source_lookup_epoch,
        (uint32_t)command_id, ft4_source_count);
    if (indexed == UINT32_MAX) return XG_RENDER_MODEL_REPLAY_LOOKUP_ABSENT;
    record = &ft4_sources[indexed];
    if (!record->valid ||
        !physical_address_equals(record->source_id, (uint32_t)command_id) ||
        record->opcode != context->opcode)
        return XG_RENDER_MODEL_REPLAY_RECORD_REJECTED;
    if (services == NULL || services->lifecycle == NULL ||
        services->lifecycle->replay_container_matches_command == NULL ||
        !services->lifecycle->replay_container_matches_command(context))
        return XG_RENDER_MODEL_REPLAY_CONTAINER_REJECTED;
    if (services->lifecycle->matches_replay == NULL ||
        !services->lifecycle->matches_replay(&record->lifecycle, context))
        return XG_RENDER_MODEL_REPLAY_LIFECYCLE_REJECTED;
    if (!xg_render_primitive_translate_cached(
            &record->primitive, &record->semantic,
            &record->semantic_ready, out_semantic))
        return XG_RENDER_MODEL_REPLAY_TRANSLATE_REJECTED;
    if (record->interpolation_identity_valid &&
        services->submission.interpolation_scene != NULL)
        xg_render_semantic_set_interpolation_identity(
            out_semantic, services->submission.interpolation_scene(),
            record->interpolation_producer_id,
            record->interpolation_primitive_id);
    return XG_RENDER_MODEL_REPLAY_RESOLVED;
}
