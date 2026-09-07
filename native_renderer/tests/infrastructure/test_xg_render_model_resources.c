#include "xg_render_model_resources.h"

#include <assert.h>
#include <string.h>

static XgRenderModelResource publish(
    const XgRenderModelPublicationDescription *description) {
    XgRenderModelPublication publication;
    XgRenderModelResource resource;

    assert(xg_render_model_resource_publish_begin(description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_publish_commit(publication, &resource) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    return resource;
}

static void authorize_source(XgRenderModelAuthenticatedSourceReceipt *source,
        XgRenderResourceOwnerKind owner_kind, uint64_t owner_generation) {
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = source->source_kind == XG_RENDER_MODEL_SOURCE_ARTIFACT
            ? XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT
            : XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = source->receipt,
        .lifetime = owner_kind == XG_RENDER_RESOURCE_OWNER_SCENE
            ? XG_RENDER_RESOURCE_CAPABILITY_SCENE
            : XG_RENDER_RESOURCE_CAPABILITY_MODULE,
        .owner_kind = owner_kind,
        .owner_generation = owner_generation,
        .artifact = {
            .base = UINT32_C(0x80010000),
            .size = UINT32_C(0x1000),
            .crc32 = 1u,
        },
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE,
            .identity = source->source_identity,
            .range_offset = source->range_offset,
            .range_size = source->range_size,
            .range_content_digest = source->range_content_digest,
        },
    };
    XgRenderResourceProvenance provenance;

    assert(xg_render_resource_capability_register(&metadata, &provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    source->capability = provenance.capability;
}

static void test_model_authority_cold_restore(void) {
    const uint8_t source_bytes[] = {9u, 8u, 7u, 6u};
    const uint8_t payload[] = {1u, 3u, 5u, 7u};
    XgRenderModelAuthenticatedSourceReceipt source = {
        .receipt = 501u,
        .source_kind = XG_RENDER_MODEL_SOURCE_RECEIPT,
        .range_offset = 32u,
        .range_size = sizeof(source_bytes),
    };
    XgRenderResourceCapabilityMetadata metadata = {
        .kind = XG_RENDER_RESOURCE_PROVENANCE_SOURCE,
        .receipt = source.receipt,
        .lifetime = XG_RENDER_RESOURCE_CAPABILITY_SCENE,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 12u,
        .source = {
            .source_class = XG_RENDER_RESOURCE_SOURCE_MODEL_RANGE,
            .range_offset = source.range_offset,
            .range_size = source.range_size,
        },
    };
    XgRenderResourceCapabilityCheckpoint authority;
    XgRenderResourceProvenance provenance;
    XgRenderResourceProvenance restored;
    XgRenderModelPublicationDescription description;
    XgRenderModelResource resource;
    XgRenderModelResourceView view;

    source.source_identity.bytes[0] = 0x5au;
    source.range_content_digest = xg_render_resource_digest(
        source_bytes, sizeof(source_bytes));
    metadata.source.identity = source.source_identity;
    metadata.source.range_content_digest = source.range_content_digest;
    xg_render_model_resources_reset();
    xg_render_resource_repository_reset();
    assert(xg_render_resource_capability_register(&metadata, &provenance) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    source.capability = provenance.capability;
    assert(xg_render_resource_capability_checkpoint(
               &provenance, metadata.owner_kind, metadata.owner_generation,
               &authority) == XG_RENDER_RESOURCE_CAPABILITY_OK);

    xg_render_model_resources_reset();
    xg_render_resource_repository_reset();
    assert(xg_render_resource_capability_restore_begin());
    assert(xg_render_resource_capability_checkpoint_restore_stage(
               &authority, &provenance, metadata.owner_kind,
               metadata.owner_generation, 13u, &restored) ==
           XG_RENDER_RESOURCE_CAPABILITY_OK);
    xg_render_resource_capability_restore_commit();
    assert(restored.capability != source.capability);
    source.capability = restored.capability;
    description = (XgRenderModelPublicationDescription){
        .authenticated_source = &source,
        .provenance_receipt = source.receipt,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE,
        .owner_generation = 13u,
        .source_bytes = source_bytes,
        .source_byte_count = sizeof(source_bytes),
        .payload_bytes = payload,
        .payload_byte_count = sizeof(payload),
        .payload_content_digest = xg_render_resource_digest(
            payload, sizeof(payload)),
    };
    resource = publish(&description);
    assert(xg_render_model_resource_view(resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(view.resource.provenance.capability == restored.capability);
    assert(view.source_range_offset == source.range_offset);
    assert(view.source_range_size == source.range_size);
    assert(memcmp(&view.source_identity, &source.source_identity,
                   sizeof(view.source_identity)) == 0);
}

static void test_model_replace_and_retire_current(void) {
    const uint8_t source_bytes[] = {2u, 4u, 6u, 8u};
    const uint8_t first_payload[] = {1u, 3u, 5u, 7u};
    const uint8_t second_payload[] = {9u, 11u, 13u, 15u};
    XgRenderModelAuthenticatedSourceReceipt source = {
        .receipt = 701u,
        .source_kind = XG_RENDER_MODEL_SOURCE_RECEIPT,
        .range_offset = 64u,
        .range_size = sizeof(source_bytes),
    };
    XgRenderModelPublicationDescription description;
    XgRenderModelResource first;
    XgRenderModelResource second;
    XgRenderModelResource immediate;
    XgRenderModelResourceView model_view;
    XgRenderResourceView resource_view;

    source.source_identity.bytes[0] = 0x71u;
    source.range_content_digest = xg_render_resource_digest(
        source_bytes, sizeof(source_bytes));
    xg_render_model_resources_reset();
    xg_render_resource_repository_reset();
    assert(xg_render_model_resource_retire_current(
               (XgRenderResourceHandle){0}) ==
           XG_RENDER_MODEL_RESOURCE_INVALID_ARGUMENT);
    authorize_source(&source, XG_RENDER_RESOURCE_OWNER_MODULE, 21u);
    description = (XgRenderModelPublicationDescription){
        .authenticated_source = &source,
        .provenance_receipt = source.receipt,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_MODULE,
        .owner_generation = 21u,
        .source_bytes = source_bytes,
        .source_byte_count = sizeof(source_bytes),
        .payload_bytes = first_payload,
        .payload_byte_count = sizeof(first_payload),
        .payload_content_digest = xg_render_resource_digest(
            first_payload, sizeof(first_payload)),
    };
    first = publish(&description);
    assert(xg_render_resource_retain(first.handle) == XG_RENDER_RESOURCE_OK);

    description.payload_bytes = second_payload;
    description.payload_byte_count = sizeof(second_payload);
    description.payload_content_digest = xg_render_resource_digest(
        second_payload, sizeof(second_payload));
    second = publish(&description);
    assert(second.handle.resource_id == first.handle.resource_id);
    assert(second.handle.generation != first.handle.generation);
    assert(xg_render_model_resource_view(first.handle, &model_view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(!model_view.resource.current);
    assert(memcmp(model_view.resource.bytes, first_payload,
                  sizeof(first_payload)) == 0);
    assert(xg_render_resource_release(first.handle) == XG_RENDER_RESOURCE_OK);

    assert(xg_render_resource_retain(second.handle) == XG_RENDER_RESOURCE_OK);
    assert(xg_render_model_resource_retire_current(second.handle) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_view(second.handle, &model_view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(!model_view.resource.current &&
           model_view.resource.retain_count == 1u);
    assert(memcmp(model_view.resource.bytes, second_payload,
                  sizeof(second_payload)) == 0);
    assert(xg_render_resource_acquire_current(
               second.handle, second.content_digest) ==
           XG_RENDER_RESOURCE_STALE);
    assert(xg_render_model_resource_retire_current(second.handle) ==
           XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION);
    assert(xg_render_resource_release(second.handle) == XG_RENDER_RESOURCE_OK);
    assert(xg_render_model_resource_view(second.handle, &model_view) ==
           XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION);
    assert(xg_render_resource_view(second.handle, &resource_view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    immediate = publish(&description);
    assert(immediate.handle.generation != second.handle.generation);
    assert(xg_render_model_resource_retire_current(immediate.handle) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_resource_view(immediate.handle, &resource_view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
}

int main(void) {
    uint8_t source_bytes[] = {1u, 2u, 3u, 4u, 5u, 6u};
    const uint8_t old_payload[] = {10u, 20u, 30u};
    const uint8_t new_payload[] = {40u, 50u, 60u};
    const uint8_t cancelled_payload[] = {70u, 80u, 90u};
    XgRenderModelAuthenticatedSourceReceipt receipt = {
        .receipt = 101u,
        .capability = 1u,
        .source_kind = XG_RENDER_MODEL_SOURCE_ARTIFACT,
        .range_offset = UINT64_C(0x120),
        .range_size = sizeof(source_bytes),
    };
    XgRenderModelPublicationDescription description;
    XgRenderModelPublication publication;
    XgRenderModelPublication other_publication;
    XgRenderModelResource old_resource;
    XgRenderModelResource current_resource;
    XgRenderModelResourceView view;
    XgRenderResourceIdentity identity;
    XgRenderResourceIdentity same_identity;
    XgRenderResourceIdentity other_identity;
    XgRenderModelAuthenticatedSourceReceipt same_source;
    XgRenderModelAuthenticatedSourceReceipt other_range;
    XgRenderResourceView generic_view;

    test_model_authority_cold_restore();

    for (uint32_t index = 0u; index < XG_RENDER_RESOURCE_IDENTITY_SIZE;
         ++index)
        receipt.source_identity.bytes[index] = (uint8_t)(index + 1u);
    receipt.range_content_digest = xg_render_resource_digest(
        source_bytes, sizeof(source_bytes));
    same_source = receipt;
    same_source.receipt = 999u;
    same_source.source_kind = XG_RENDER_MODEL_SOURCE_RECEIPT;
    other_range = receipt;
    other_range.range_offset++;
    assert(xg_render_model_resource_identity(&receipt, &identity) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_identity(&same_source, &same_identity) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(memcmp(&identity, &same_identity, sizeof(identity)) == 0);
    assert(xg_render_model_resource_identity(&other_range, &other_identity) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(memcmp(&identity, &other_identity, sizeof(identity)) != 0);

    xg_render_model_resources_reset();
    xg_render_resource_repository_reset();
    authorize_source(&receipt, XG_RENDER_RESOURCE_OWNER_MODULE, 7u);
    description = (XgRenderModelPublicationDescription){
        .authenticated_source = &receipt,
        .provenance_receipt = receipt.receipt,
        .owner_kind = XG_RENDER_RESOURCE_OWNER_MODULE,
        .owner_generation = 7u,
        .source_bytes = source_bytes,
        .source_byte_count = sizeof(source_bytes),
        .payload_bytes = old_payload,
        .payload_byte_count = sizeof(old_payload),
        .payload_content_digest = xg_render_resource_digest(
            old_payload, sizeof(old_payload)),
    };
    old_resource = publish(&description);
    assert(old_resource.handle.resource_id ==
           xg_render_resource_identity_id(&identity));
    assert(xg_render_model_resource_view(old_resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(view.resource.kind == XG_RENDER_RESOURCE_MODEL);
    assert(view.resource.owner_kind == XG_RENDER_RESOURCE_OWNER_MODULE);
    assert(view.resource.owner_generation == 7u);
    assert(view.resource.provenance.kind ==
           XG_RENDER_RESOURCE_PROVENANCE_ARTIFACT);
    assert(!view.resource.provenance.synthetic);
    assert(view.resource.provenance.receipt == receipt.receipt);
    assert(view.provenance_receipt == receipt.receipt);
    assert(view.source_range_offset == receipt.range_offset);
    assert(view.source_range_size == receipt.range_size);
    assert(memcmp(&view.source_identity, &receipt.source_identity,
                  sizeof(view.source_identity)) == 0);

    assert(xg_render_resource_retain(old_resource.handle) ==
           XG_RENDER_RESOURCE_OK);
    description.payload_bytes = new_payload;
    description.payload_byte_count = sizeof(new_payload);
    description.payload_content_digest = xg_render_resource_digest(
        new_payload, sizeof(new_payload));
    current_resource = publish(&description);
    assert(current_resource.handle.resource_id == old_resource.handle.resource_id);
    assert(current_resource.handle.generation != old_resource.handle.generation);
    assert(xg_render_model_resource_view(old_resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(!view.resource.current);
    assert(xg_render_resource_acquire_current(
               old_resource.handle, old_resource.content_digest) ==
           XG_RENDER_RESOURCE_STALE);
    assert(xg_render_resource_release(old_resource.handle) ==
           XG_RENDER_RESOURCE_OK);

    description.authenticated_source = NULL;
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_AUTHENTICATION_REQUIRED);
    description.authenticated_source = &receipt;
    description.provenance_receipt = 0u;
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_AUTHENTICATION_REQUIRED);
    description.provenance_receipt = receipt.receipt + 1u;
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_RECEIPT_MISMATCH);
    description.provenance_receipt = receipt.receipt;

    description.payload_bytes = cancelled_payload;
    description.payload_byte_count = sizeof(cancelled_payload);
    description.payload_content_digest = xg_render_resource_digest(
        cancelled_payload, sizeof(cancelled_payload));
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_publish_begin(
               &description, &other_publication) ==
           XG_RENDER_MODEL_RESOURCE_INVALID_STATE);
    assert(xg_render_model_resource_publish_cancel(publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_view(current_resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(view.resource.current);
    assert(xg_render_resource_view(publication.resource, &generic_view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);

    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    authorize_source(&other_range, XG_RENDER_RESOURCE_OWNER_MODULE, 7u);
    description.authenticated_source = &other_range;
    assert(xg_render_model_resource_publish_begin(
               &description, &other_publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(publication.resource.resource_id !=
           other_publication.resource.resource_id);
    assert(xg_render_model_resource_publish_cancel(publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_publish_commit(
               other_publication, &old_resource) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_view(old_resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    description.authenticated_source = &receipt;

    description.payload_bytes = cancelled_payload;
    description.payload_content_digest++;
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_PAYLOAD_DIGEST_MISMATCH);
    description.payload_content_digest = xg_render_resource_digest(
        cancelled_payload, sizeof(cancelled_payload));
    receipt.range_content_digest++;
    authorize_source(&receipt, XG_RENDER_RESOURCE_OWNER_MODULE, 7u);
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_SOURCE_DIGEST_MISMATCH);
    receipt.range_content_digest = xg_render_resource_digest(
        source_bytes, sizeof(source_bytes));
    authorize_source(&receipt, XG_RENDER_RESOURCE_OWNER_MODULE, 7u);
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    source_bytes[0] ^= 0xffu;
    assert(xg_render_model_resource_publish_commit(publication, &old_resource) ==
           XG_RENDER_MODEL_RESOURCE_SOURCE_DIGEST_MISMATCH);
    source_bytes[0] ^= 0xffu;
    assert(xg_render_model_resource_view(current_resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(view.resource.current);

    other_range.range_offset++;
    description.authenticated_source = &other_range;
    description.owner_kind = XG_RENDER_RESOURCE_OWNER_SCENE;
    description.owner_generation = 8u;
    authorize_source(&other_range, XG_RENDER_RESOURCE_OWNER_SCENE, 8u);
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    xg_render_model_resources_scene_boundary();
    assert(xg_render_resource_view(publication.resource, &generic_view) ==
           XG_RENDER_RESOURCE_NOT_FOUND);
    assert(xg_render_model_resource_publish_commit(publication, &old_resource) ==
           XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION);
    authorize_source(&other_range, XG_RENDER_RESOURCE_OWNER_SCENE, 8u);
    assert(xg_render_model_resource_publish_begin(&description, &publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(xg_render_model_resource_publish_cancel(publication) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    description.authenticated_source = &receipt;
    description.owner_kind = XG_RENDER_RESOURCE_OWNER_MODULE;
    description.owner_generation = 7u;

    assert(xg_render_resource_retain(current_resource.handle) ==
           XG_RENDER_RESOURCE_OK);
    xg_render_resource_invalidate_owner(
        XG_RENDER_RESOURCE_OWNER_MODULE, 7u);
    assert(xg_render_model_resource_view(current_resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_OK);
    assert(!view.resource.current);
    assert(xg_render_resource_acquire_current(
               current_resource.handle, current_resource.content_digest) ==
           XG_RENDER_RESOURCE_STALE);
    assert(xg_render_resource_release(current_resource.handle) ==
           XG_RENDER_RESOURCE_OK);
    assert(xg_render_model_resource_view(current_resource.handle, &view) ==
           XG_RENDER_MODEL_RESOURCE_STALE_PUBLICATION);
    test_model_replace_and_retire_current();
    return 0;
}
