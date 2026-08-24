#ifndef XG_RENDER_INVALIDATION_EVENT_H
#define XG_RENDER_INVALIDATION_EVENT_H

#include "xg_render_auth_candidate_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XgRenderFieldSpriteServices XgRenderFieldSpriteServices;
typedef struct XgRenderModelRepositoryServices XgRenderModelRepositoryServices;
typedef struct XgRenderModelSpritePipelineServices
    XgRenderModelSpritePipelineServices;

typedef enum XgRenderInvalidationKind {
    XG_RENDER_INVALIDATION_DISABLE = 0,
    XG_RENDER_INVALIDATION_SCENE_BOUNDARY,
    XG_RENDER_INVALIDATION_CODE_WRITE,
    XG_RENDER_INVALIDATION_LOADER_MISMATCH,
    XG_RENDER_INVALIDATION_RESOURCE_OVERLAP,
    XG_RENDER_INVALIDATION_AUTHORITY_LOST,
    XG_RENDER_INVALIDATION_RESET,
} XgRenderInvalidationKind;

typedef struct XgRenderMutationProperties {
    bool watched_range_mutation;
    bool runtime_variant_mutation;
    bool executable_mutation;
    bool artifact_mutation;
    bool descriptor_mutation;
    bool resource_mutation;
    bool shared_data_mutation;
    bool semantic_authority_loss;
    bool authentication_mutation;
    bool authority_loss;
    bool interpolation_reset;
    bool reset_runtime_variant;
} XgRenderMutationProperties;

typedef struct XgRenderMutationClassification {
    XgRenderMutationProperties properties;
    uint32_t code_write_mask;
} XgRenderMutationClassification;

typedef struct XgRenderInvalidationEvent {
    XgRenderInvalidationKind kind;
    uint32_t address;
    uint32_t size;
    uint32_t code_write_mask;
    XgRenderMutationProperties mutation;
} XgRenderInvalidationEvent;

typedef struct XgRenderInvalidationServices {
    const XgRenderFieldSpriteServices *field_sprite;
    const XgRenderModelRepositoryServices *model_repository;
    const XgRenderModelSpritePipelineServices *model_sprite_pipeline;
} XgRenderInvalidationServices;

typedef void (*XgRenderInvalidationHandler)(
    const XgRenderInvalidationEvent *event,
    const XgRenderInvalidationServices *services);

typedef struct XgRenderInvalidationModule {
    XgRenderInvalidationHandler handle;
} XgRenderInvalidationModule;

static inline bool xg_render_invalidation_has_code_class(
        const XgRenderInvalidationEvent *event,
        PsxXgRenderCodeWriteClass code_class) {
    return event != NULL &&
        (event->code_write_mask & (UINT32_C(1) << code_class)) != 0u;
}

#endif
