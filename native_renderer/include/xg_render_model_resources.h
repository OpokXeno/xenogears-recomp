#ifndef XG_RENDER_MODEL_RESOURCES_H
#define XG_RENDER_MODEL_RESOURCES_H

#include "xg_render_resource_repository.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderModelSourceKind {
    XG_RENDER_MODEL_SOURCE_ARTIFACT = 1,
    XG_RENDER_MODEL_SOURCE_RECEIPT = 2,
} XgRenderModelSourceKind;

/* Minimal contract supplied by the authenticated artifact/source owner. */
typedef struct XgRenderModelAuthenticatedSourceReceipt {
    uint64_t receipt;
    uint64_t capability;
    XgRenderModelSourceKind source_kind;
    XgRenderResourceIdentity source_identity;
    uint64_t range_offset;
    uint64_t range_size;
    uint64_t range_content_digest;
} XgRenderModelAuthenticatedSourceReceipt;

typedef struct XgRenderModelPublicationDescription {
    const XgRenderModelAuthenticatedSourceReceipt *authenticated_source;
    uint64_t provenance_receipt;
    XgRenderResourceOwnerKind owner_kind;
    uint64_t owner_generation;
    const void *source_bytes;
    size_t source_byte_count;
    const void *payload_bytes;
    size_t payload_byte_count;
    uint64_t payload_content_digest;
} XgRenderModelPublicationDescription;

typedef struct XgRenderModelPublication {
    XgRenderResourceHandle resource;
    uint64_t transaction;
} XgRenderModelPublication;

typedef struct XgRenderModelResource {
    XgRenderResourceHandle handle;
    uint64_t content_digest;
    uint64_t provenance_receipt;
} XgRenderModelResource;

typedef struct XgRenderModelResourceView {
    XgRenderResourceView resource;
    XgRenderModelSourceKind source_kind;
    XgRenderResourceIdentity source_identity;
    uint64_t source_range_offset;
    uint64_t source_range_size;
    uint64_t provenance_receipt;
} XgRenderModelResourceView;

typedef enum XgRenderModelResourceResult {
    XG_RENDER_MODEL_RESOURCE_OK = 0,
    XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT,
    XG_RENDER_MODEL_RESOURCE_AUTHENTICATION_REQUIRED,
    XG_RENDER_MODEL_RESOURCE_RECEIPT_MISMATCH,
    XG_RENDER_MODEL_RESOURCE_SOURCE_DIGEST_MISMATCH,
    XG_RENDER_MODEL_RESOURCE_PAYLOAD_DIGEST_MISMATCH,
    XG_RENDER_MODEL_RESOURCE_CAPACITY_EXCEEDED,
    XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION,
    XG_RENDER_MODEL_RESOURCE_INVALID_STATE,
    XG_RENDER_MODEL_RESOURCE_REPOSITORY_FAILED,
} XgRenderModelResourceResult;

void xg_render_model_resources_reset(void);
void xg_render_model_resources_scene_boundary(void);
XgRenderModelResourceResult xg_render_model_resource_identity(
    const XgRenderModelAuthenticatedSourceReceipt *authenticated_source,
    XgRenderResourceIdentity *out_identity);
XgRenderModelResourceResult xg_render_model_resource_publish_begin(
    const XgRenderModelPublicationDescription *description,
    XgRenderModelPublication *out_publication);
XgRenderModelResourceResult xg_render_model_resource_publish_commit(
    XgRenderModelPublication publication,
    XgRenderModelResource *out_resource);
XgRenderModelResourceResult xg_render_model_resource_publish_cancel(
    XgRenderModelPublication publication);
XgRenderModelResourceResult xg_render_model_resource_retire_current(
    XgRenderResourceHandle handle);
XgRenderModelResourceResult xg_render_model_resource_view(
    XgRenderResourceHandle handle,
    XgRenderModelResourceView *out_view);

#ifdef __cplusplus
}
#endif

#endif
