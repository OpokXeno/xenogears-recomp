#ifndef XG_RENDER_MANIFEST_GENERATED_H
#define XG_RENDER_MANIFEST_GENERATED_H

#include <stdint.h>

enum {
    XG_RENDER_MANIFEST_DIGEST_SIZE = 32,
};

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XgRenderManifestRecord {
    uint32_t record_id;
    const char *id;
    uint32_t kind;
    uint32_t address;
    uint32_t target_address;
    uint8_t image_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint8_t window_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    const char *framebuffer_context;
    const char *ot_context;
} XgRenderManifestRecord;

typedef struct XgRenderManifestValidation {
    uint32_t producer_record_id;
    uint32_t site_record_id;
    uint32_t field_base_crc32;
    uint32_t field_range_crc32;
    uint32_t field_range_start;
    uint32_t field_range_size;
    uint32_t producer_entry;
    uint32_t caller_site;
    uint32_t static_callee;
    uint32_t return_site;
    uint32_t instruction_window_start;
    uint32_t instruction_window_size;
    uint8_t instruction_window_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
    uint32_t required_jal_opcode;
    uint32_t jal_target;
    uint32_t required_delay_slot_instructions;
    uint32_t required_delay_slot_non_control_transfer;
} XgRenderManifestValidation;

extern const uint8_t xg_render_game_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
extern const uint8_t xg_render_manifest_identity[XG_RENDER_MANIFEST_DIGEST_SIZE];
extern const uint32_t xg_render_namespace_crc32;
extern const XgRenderManifestValidation xg_render_manifest_validation;
extern const XgRenderManifestRecord xg_render_manifest_records[];
extern const uint32_t xg_render_manifest_record_count;

#ifdef __cplusplus
}
#endif

#endif
