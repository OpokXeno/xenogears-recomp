#include "xg_render_mutation_classifier.h"

#include "xg_render_auth_candidate_types.h"

#include <stddef.h>

enum { XG_RENDER_MUTATION_SOURCE_CAPACITY = 16u };

static XgRenderMutationSource sources[XG_RENDER_MUTATION_SOURCE_CAPACITY];
static uint32_t source_count;
static XgRenderMutationWatchRegistrar registered_watch_sources[
    XG_RENDER_MUTATION_SOURCE_CAPACITY];
static uint32_t registered_watch_source_count;
static void (*watch_callback)(uint32_t physical_address, uint32_t size);

static bool watch_source_is_registered(
        XgRenderMutationWatchRegistrar register_watches) {
    for (uint32_t index = 0u; index < registered_watch_source_count; ++index) {
        if (registered_watch_sources[index] == register_watches) return true;
    }
    return false;
}

static void register_source_watches(const XgRenderMutationSource *source) {
    if (watch_callback == NULL || source->register_watches == NULL ||
        watch_source_is_registered(source->register_watches))
        return;
    source->register_watches(watch_callback);
    registered_watch_sources[registered_watch_source_count++] =
        source->register_watches;
}

static void merge_properties(XgRenderMutationProperties *destination,
                             const XgRenderMutationProperties *source) {
    destination->watched_range_mutation |= source->watched_range_mutation;
    destination->runtime_variant_mutation |= source->runtime_variant_mutation;
    destination->executable_mutation |= source->executable_mutation;
    destination->artifact_mutation |= source->artifact_mutation;
    destination->descriptor_mutation |= source->descriptor_mutation;
    destination->resource_mutation |= source->resource_mutation;
    destination->shared_data_mutation |= source->shared_data_mutation;
    destination->semantic_authority_loss |= source->semantic_authority_loss;
    destination->authentication_mutation |= source->authentication_mutation;
    destination->authority_loss |= source->authority_loss;
    destination->interpolation_reset |= source->interpolation_reset;
    destination->reset_runtime_variant |= source->reset_runtime_variant;
}

void xg_render_mutation_classifier_clear_sources(void) {
    /* The rebuilt table uses the same registrars, so retain which callbacks
     * have already populated the host watch set. */
    source_count = 0u;
}

bool xg_render_mutation_classifier_register_source(
        const XgRenderMutationSource *source) {
    if (source == NULL || source->classify == NULL ||
        source_count >= XG_RENDER_MUTATION_SOURCE_CAPACITY)
        return false;
    sources[source_count++] = *source;
    register_source_watches(source);
    return true;
}

void xg_render_mutation_classify(
        uint32_t address, uint32_t size,
        const XgRenderMutationContext *context,
        XgRenderMutationClassification *out_classification) {
    XgRenderMutationClassification classification = {0};

    if (out_classification == NULL) return;
    for (uint32_t index = 0u; index < source_count; ++index) {
        XgRenderMutationClassification contribution = {0};

        sources[index].classify(address, size, &contribution);
        classification.code_write_mask |= contribution.code_write_mask;
        merge_properties(&classification.properties, &contribution.properties);
    }
    if (context != NULL) {
        classification.properties.resource_mutation |=
            context->resource_mutation;
        classification.properties.authentication_mutation |=
            context->authentication_range_mutation;
        classification.properties.authority_loss |=
            context->authentication_range_mutation;
        classification.properties.reset_runtime_variant |=
            context->completed_authorization;
        if (context->artifact_mutation) {
            classification.code_write_mask |=
                UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_ARTIFACT;
            classification.properties.artifact_mutation = true;
            classification.properties.executable_mutation = true;
            classification.properties.semantic_authority_loss = true;
            classification.properties.authority_loss = true;
            classification.properties.interpolation_reset = true;
        }
    }
    *out_classification = classification;
}

bool xg_render_mutation_executable_overlaps(uint32_t address, uint32_t size) {
    XgRenderMutationClassification classification;

    xg_render_mutation_classify(address, size, NULL, &classification);
    return classification.properties.executable_mutation;
}

void xg_render_mutation_register_watches(
        void (*set_range)(uint32_t physical_address, uint32_t size)) {
    if (watch_callback != set_range) {
        watch_callback = set_range;
        registered_watch_source_count = 0u;
    }
    if (watch_callback == NULL) return;
    for (uint32_t index = 0u; index < source_count; ++index)
        register_source_watches(&sources[index]);
}
