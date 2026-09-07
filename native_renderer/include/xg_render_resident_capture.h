#ifndef XG_RENDER_RESIDENT_CAPTURE_H
#define XG_RENDER_RESIDENT_CAPTURE_H

#include "xg_render_ir.h"

#include <stdbool.h>
#include <stdint.h>

#define XG_RENDER_RESIDENT_RESOURCE_TEMPLATE_CAPACITY 256u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderResidentCaptureResult {
    XG_RENDER_RESIDENT_CAPTURE_OK = 0,
    XG_RENDER_RESIDENT_CAPTURE_INVALID_ARGUMENT,
    XG_RENDER_RESIDENT_CAPTURE_CAPACITY_EXCEEDED = 3,
    XG_RENDER_RESIDENT_CAPTURE_EMIT_FAILED = 12,
} XgRenderResidentCaptureResult;

typedef struct CPUState CPUState;

typedef struct XgRenderResidentResourceTemplate {
    XgRenderIrNativePrimitive primitive;
    uint32_t descriptor_address;
    /* Binding identity only: never read as a geometry/material source. */
    uint32_t destination_address;
} XgRenderResidentResourceTemplate;

typedef struct XgRenderResidentResourceTemplateServices {
    void *context;
    /* Validate the active artifact/code range, not just the instruction word.
     * Source validation must include ownership and mapped/aligned bounds. */
    bool (*authorize)(void *context, uint32_t pc, uint32_t instruction_word);
    bool (*source_range_valid)(void *context, uint32_t address,
                               uint32_t byte_count, uint32_t alignment);
    bool (*capture_draw_state)(void *context, XgRenderIrMaterialState *out_state);
    /* Atomically copy the entire batch into private pending lifecycle storage.
     * Activate at the builder return (80026a04 or 800372c4, both 03e00008),
     * after guest writes, without reading target payloads. Submit order belongs
     * to the later caller that links these templates. Zero count clears the
     * prior pending batch. Buffers are borrowed only for this callback. */
    bool (*publish_templates)(void *context,
        uint32_t destination_base, uint32_t parity,
        const XgRenderResidentResourceTemplate *templates, uint32_t count);
} XgRenderResidentResourceTemplateServices;

/* Pre-instruction hook at resident 8002675c / 27bdffb0. Shared by UI resources
 * and Battle's polygon-composed font; does not interpret a dialog stream. */
XgRenderResidentCaptureResult xg_render_resident_capture_resource_templates(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    const XgRenderResidentResourceTemplateServices *services);
/* Resident font/debug text at 800370dc / 3c068006. Captures one admitted glyph
 * from the live font context, including proportional width/case/wrap rules.
 * Controls/spaces are guest-only updates and return OK without publication.
 * FontDrawLetters (80037324) later prepends admitted glyphs in reverse order. */
XgRenderResidentCaptureResult xg_render_resident_capture_font_character(
    CPUState *cpu, uint32_t pc, uint32_t instruction_word,
    const XgRenderResidentResourceTemplateServices *services);

#ifdef __cplusplus
}
#endif

#endif
