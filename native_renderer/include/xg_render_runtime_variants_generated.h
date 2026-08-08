#ifndef XG_RENDER_RUNTIME_VARIANTS_GENERATED_H
#define XG_RENDER_RUNTIME_VARIANTS_GENERATED_H

#include "xg_render_manifest_generated.h"

#include <stdint.h>

#define XG_RENDER_RUNTIME_VARIANT_SOURCE_SITE_CAP 14u

typedef enum XgRenderRuntimeVariantSourceOperation {
    XG_RENDER_RUNTIME_VARIANT_SOURCE_READ = 0,
    XG_RENDER_RUNTIME_VARIANT_SOURCE_WRITE = 1,
    XG_RENDER_RUNTIME_VARIANT_SOURCE_SWC2 = 2,
    XG_RENDER_RUNTIME_VARIANT_SOURCE_CALL = 3,
    XG_RENDER_RUNTIME_VARIANT_SOURCE_BUCKET = 4,
} XgRenderRuntimeVariantSourceOperation;

typedef enum XgRenderRuntimeVariantSourceAuxiliary {
    XG_RENDER_RUNTIME_VARIANT_SOURCE_EFFECTIVE_ADDRESS = 0,
    XG_RENDER_RUNTIME_VARIANT_SOURCE_NONE = 1,
    XG_RENDER_RUNTIME_VARIANT_SOURCE_RESULT_REGISTER = 2,
} XgRenderRuntimeVariantSourceAuxiliary;

typedef struct XgRenderRuntimeVariantSourceSite {
    uint32_t pc;
    uint32_t instruction;
    uint8_t operation;
    uint8_t width;
    uint8_t auxiliary_rule;
} XgRenderRuntimeVariantSourceSite;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderRuntimeVariantDescriptor {
    uint32_t producer_record_id;
    uint32_t site_record_id;
    uint8_t canonical_game_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint8_t canonical_manifest_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint8_t companion_manifest_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint8_t artifact_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint32_t artifact_crc32;
    uint32_t artifact_size;
    uint32_t artifact_base;
    uint32_t artifact_range_start;
    uint32_t artifact_range_size;
    uint32_t artifact_range_crc32;
    uint8_t artifact_range_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint32_t canonical_producer_entry;
    uint32_t canonical_capture_site;
    uint32_t canonical_static_callee;
    uint32_t canonical_return_site;
    uint32_t activation_window_start;
    uint32_t activation_window_size;
    uint8_t activation_window_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint32_t activation_site;
    uint32_t activation_required_jal_opcode;
    uint32_t activation_jal_target;
    uint32_t activation_delay_instruction;
    uint32_t physical_producer_entry;
    uint32_t capture_window_start;
    uint32_t capture_window_size;
    uint8_t capture_window_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint32_t capture_site;
    uint32_t capture_required_jal_opcode;
    uint32_t capture_jal_target;
    uint32_t capture_delay_instruction;
    uint32_t physical_return_site;
    uint32_t source_site_count;
    XgRenderRuntimeVariantSourceSite
        source_sites[XG_RENDER_RUNTIME_VARIANT_SOURCE_SITE_CAP];
} XgRenderRuntimeVariantDescriptor;

extern const XgRenderRuntimeVariantDescriptor
    xg_render_runtime_variant_descriptors[];
extern const uint32_t xg_render_runtime_variant_descriptor_count;

#ifdef __cplusplus
}
#endif

#endif
