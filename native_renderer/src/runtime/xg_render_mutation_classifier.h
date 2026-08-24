#ifndef XG_RENDER_MUTATION_CLASSIFIER_H
#define XG_RENDER_MUTATION_CLASSIFIER_H

#include "xg_render_invalidation_event.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct XgRenderMutationContext {
    bool artifact_mutation;
    bool authentication_range_mutation;
    bool completed_authorization;
    bool resource_mutation;
} XgRenderMutationContext;

typedef void (*XgRenderMutationClassifier)(
    uint32_t address, uint32_t size,
    XgRenderMutationClassification *out_classification);
typedef void (*XgRenderMutationWatchRegistrar)(
    void (*set_range)(uint32_t physical_address, uint32_t size));

typedef struct XgRenderMutationSource {
    XgRenderMutationClassifier classify;
    XgRenderMutationWatchRegistrar register_watches;
} XgRenderMutationSource;

void xg_render_mutation_classifier_clear_sources(void);
bool xg_render_mutation_classifier_register_source(
    const XgRenderMutationSource *source);
void xg_render_mutation_classify(
    uint32_t address, uint32_t size,
    const XgRenderMutationContext *context,
    XgRenderMutationClassification *out_classification);
bool xg_render_mutation_executable_overlaps(uint32_t address, uint32_t size);
void xg_render_mutation_register_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size));

#endif
