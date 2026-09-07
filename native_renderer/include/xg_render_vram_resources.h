#ifndef XG_RENDER_VRAM_RESOURCES_H
#define XG_RENDER_VRAM_RESOURCES_H

#include "cpu_state.h"
#include "guest_render_types.h"
#include "xg_render_resource_repository.h"
#include "xg_render_scene_snapshot.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XG_RENDER_VRAM_RESOURCE_CAPACITY
#define XG_RENDER_VRAM_RESOURCE_CAPACITY 64u
#endif

#define XG_RENDER_VRAM_RETIREMENT_CAPACITY 16u

#define XG_RENDER_VRAM_CHECKPOINT_VERSION 5u

/* Native producers share one scene-owned VRAM publication registry so draw
 * resolution observes a single authoritative mutation order. */

typedef struct XgRenderVramResourceServices {
    bool (*authorize_guest_range)(uint32_t address, uint32_t size,
                                  uint32_t alignment, bool allow_scratchpad);
    XgRenderResourceProvenance provenance;
} XgRenderVramResourceServices;

typedef enum XgRenderVramResourceResult {
    XG_RENDER_VRAM_RESOURCE_OK = 0,
    XG_RENDER_VRAM_RESOURCE_INVALID_ARGUMENT,
    XG_RENDER_VRAM_RESOURCE_INVALID_TRANSITION,
    XG_RENDER_VRAM_RESOURCE_INVALID_RANGE,
    XG_RENDER_VRAM_RESOURCE_CAPACITY_EXCEEDED,
    XG_RENDER_VRAM_RESOURCE_PUBLICATION_FAILED,
} XgRenderVramResourceResult;

typedef struct XgRenderVramResourceSnapshot {
    uint64_t owner_generation;
    uint64_t vram_generation;
    uint64_t completed_loaders;
    uint64_t published_images;
    uint64_t published_cluts;
    uint64_t rejected_operations;
    uint64_t cache_rebinds;
    uint64_t cache_rebind_failures;
    uint64_t mutation_republications;
    uint64_t mutation_republication_failures;
    uint32_t publication_count;
    uint32_t cached_publication_count;
    uint16_t last_publication_x;
    uint16_t last_publication_y;
    uint16_t last_publication_width;
    uint16_t last_publication_height;
    XgRenderResourceKind last_publication_kind;
    bool loader_active;
    bool loader_blocked;
} XgRenderVramResourceSnapshot;

typedef struct XgRenderVramResourceResolvedResources {
    XgSemanticResourceRef texture;
    XgSemanticResourceRef clut;
    bool has_texture;
    bool has_clut;
} XgRenderVramResourceResolvedResources;

typedef struct XgRenderVramResourcePublicationSnapshot {
    uint64_t resource_id;
    uint64_t generation;
    uint64_t sequence;
    uint64_t content_digest;
    uint64_t owner_generation;
    uint64_t provenance_receipt;
    uint64_t provenance_capability;
    uint64_t zero_word_count;
    uint64_t visible_word_count;
    uint64_t mask_word_count;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    XgRenderResourceKind kind;
    XgRenderResourceProvenanceKind provenance_kind;
    XgRenderResourceSourceClass provenance_source_class;
} XgRenderVramResourcePublicationSnapshot;

typedef struct XgRenderVramResourceRetirementSnapshot {
    uint64_t sequence;
    uint64_t resource_id;
    uint64_t resource_generation;
    uint64_t content_digest;
    uint64_t vram_generation;
    uint64_t mutation_digest;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint16_t mutation_x;
    uint16_t mutation_y;
    uint16_t mutation_width;
    uint16_t mutation_height;
    uint16_t mutation_source_x;
    uint16_t mutation_source_y;
    XgRenderResourceKind kind;
    uint32_t mutation_operation;
    uint32_t mutation_direction;
    uint32_t mutation_command_source_address;
    uint32_t mutation_command_pc;
    uint32_t mutation_command_function;
    uint32_t mutation_command_return_address;
    uint32_t mutation_command_source_kind;
    uint32_t mutation_command_words[4];
    uint8_t mutation_command_opcode;
    uint8_t mutation_command_word_count;
    bool mutation_command_context_valid;
    uint32_t reason;
    bool preserve_exact_content;
    bool cached;
} XgRenderVramResourceRetirementSnapshot;

typedef struct XgRenderVramResourceCheckpointRestore
    XgRenderVramResourceCheckpointRestore;

void xg_render_vram_resources_reset(void);
void xg_render_vram_resources_scene_boundary(uint64_t owner_generation);
bool xg_render_vram_move_command_matches(
    uint16_t source_x, uint16_t source_y,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    uint8_t command_opcode, const uint32_t *command_words,
    uint8_t command_word_count, bool command_context_valid);
XgRenderVramResourceResult xg_render_vram_resources_begin(
    CPUState *cpu, GuestRenderRenderMode render_mode,
    uint64_t owner_generation, const XgRenderVramResourceServices *services);
XgRenderVramResourceResult xg_render_vram_image_begin(
    GuestRenderRenderMode render_mode, uint64_t owner_generation);
XgRenderVramResourceResult xg_render_vram_clut_begin(
    GuestRenderRenderMode render_mode, uint64_t owner_generation);
XgRenderVramResourceResult xg_render_vram_stream_begin(
    XgRenderResourceKind kind, GuestRenderRenderMode render_mode,
    uint64_t owner_generation);
XgRenderVramResourceResult xg_render_vram_resources_upload(
    CPUState *cpu, XgRenderResourceKind kind, uint64_t owner_generation,
    const XgRenderVramResourceServices *services);
XgRenderVramResourceResult xg_render_vram_resources_upload_region(
    CPUState *cpu, XgRenderResourceKind kind, uint64_t owner_generation,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    uint32_t payload_address, const XgRenderVramResourceServices *services);
XgRenderVramResourceResult xg_render_vram_stream_upload(
    CPUState *cpu, uint64_t owner_generation,
    const XgRenderVramResourceServices *services);
XgRenderVramResourceResult xg_render_vram_stream_finish(
    uint64_t owner_generation);
XgRenderVramResourceResult xg_render_vram_resources_commit(uint64_t owner_generation);
XgRenderVramResourceResult xg_render_vram_resources_commit_optional(
    uint64_t owner_generation);
bool xg_render_vram_resources_lookup(
    XgRenderResourceKind kind, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, XgSemanticResourceRef *out_resource);
void xg_render_vram_resources_note_vram_mutation(
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    uint64_t vram_generation, uint64_t content_digest,
    bool preserve_exact_content);
void xg_render_vram_resources_note_vram_mutation_context(
    uint16_t source_x, uint16_t source_y,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    uint64_t source_vram_generation, uint64_t vram_generation,
    uint64_t content_digest,
    uint32_t operation, uint32_t direction,
    uint32_t command_source_address, uint32_t command_pc,
    uint32_t command_function, uint32_t command_return_address,
    uint32_t command_source_kind, uint8_t command_opcode,
    const uint32_t *command_words, uint8_t command_word_count,
    bool command_context_valid,
    const XgRenderResourceIdentity *mutation_source_identity,
    const XgRenderResourceProvenance *mutation_source_provenance,
    const uint16_t *mutation_pixels, size_t mutation_pixel_count,
    bool preserve_exact_content);
bool xg_render_vram_resources_resolve_draw(
    const XgRenderIrNativePrimitive *primitive,
    XgRenderVramResourceResolvedResources *out_resources);
void xg_render_vram_resources_snapshot(XgRenderVramResourceSnapshot *out_snapshot);
size_t xg_render_vram_resources_publications(
    XgRenderVramResourcePublicationSnapshot *out_publications,
    size_t capacity);
uint64_t xg_render_vram_resources_retirement_total(void);
size_t xg_render_vram_resources_retirements(
    XgRenderVramResourceRetirementSnapshot *out_retirements,
    size_t capacity);
size_t xg_render_vram_resources_checkpoint_size(void);
bool xg_render_vram_resources_checkpoint_source_generation(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t *out_source_vram_generation);
/* Native payload bytes are part of the checkpoint because an in-flight A0 may
 * have changed guest VRAM while the previous publication remains authoritative
 * until transfer commit. Handles and scene generations are never serialized. */
XgRenderVramResourceResult xg_render_vram_resources_checkpoint_write(
    uint64_t source_vram_generation, void *out_checkpoint,
    size_t checkpoint_size);
XgRenderVramResourceResult xg_render_vram_resources_checkpoint_validate(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_vram_generation, uint64_t restored_owner_generation);
XgRenderVramResourceResult xg_render_vram_resources_checkpoint_restore(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_vram_generation, uint64_t restored_owner_generation);
XgRenderVramResourceResult xg_render_vram_resources_checkpoint_prepare(
    const void *checkpoint, size_t checkpoint_size,
    uint64_t restored_vram_generation, uint64_t restored_owner_generation,
    XgRenderVramResourceCheckpointRestore **out_restore);
void xg_render_vram_resources_checkpoint_commit(
    XgRenderVramResourceCheckpointRestore *restore);
void xg_render_vram_resources_checkpoint_cancel(
    XgRenderVramResourceCheckpointRestore *restore);

#ifdef __cplusplus
}
#endif

#endif
