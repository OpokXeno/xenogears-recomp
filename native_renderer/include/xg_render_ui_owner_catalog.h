#ifndef XG_RENDER_UI_OWNER_CATALOG_H
#define XG_RENDER_UI_OWNER_CATALOG_H

#include "xg_render_resource_repository.h"

#include <stddef.h>
#include <stdint.h>

typedef struct XgRenderUiOwnerCatalogEntry {
    uint32_t owner_domain;
    uint32_t root_address;
    XgRenderArtifactIdentity artifact;
} XgRenderUiOwnerCatalogEntry;

extern const XgRenderUiOwnerCatalogEntry xg_render_ui_owner_catalog[];
extern const size_t xg_render_ui_owner_catalog_count;

bool xg_render_ui_owner_catalog_artifact_identity(
    uint32_t base, uint32_t size,
    const uint8_t sha256[XG_RENDER_RESOURCE_IDENTITY_SIZE],
    XgRenderArtifactIdentity *out_identity);

#endif
