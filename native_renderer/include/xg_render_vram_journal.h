#ifndef XG_RENDER_VRAM_JOURNAL_H
#define XG_RENDER_VRAM_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XG_RENDER_VRAM_TRANSFER_CAPACITY
#define XG_RENDER_VRAM_TRANSFER_CAPACITY 4u
#endif

#define XG_RENDER_VRAM_MUTATION_HISTORY_CAPACITY 8192u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgRenderVramOperation {
    XG_RENDER_VRAM_UPLOAD = 0,
    XG_RENDER_VRAM_READBACK,
    XG_RENDER_VRAM_MOVE,
    XG_RENDER_VRAM_CLEAR,
    XG_RENDER_VRAM_MDEC_STRIP,
    XG_RENDER_VRAM_RENDER_TARGET_WRITE,
    XG_RENDER_VRAM_RESTORE,
    XG_RENDER_VRAM_SCANOUT,
} XgRenderVramOperation;

typedef enum XgRenderVramDirection {
    XG_RENDER_VRAM_CPU_TO_VRAM = 0,
    XG_RENDER_VRAM_VRAM_TO_CPU,
    XG_RENDER_VRAM_VRAM_TO_VRAM,
    XG_RENDER_VRAM_GPU_TO_VRAM,
    XG_RENDER_VRAM_VRAM_TO_SCANOUT,
} XgRenderVramDirection;

typedef struct XgRenderVramTransferHandle {
    uint32_t slot;
    uint32_t generation;
} XgRenderVramTransferHandle;

typedef struct XgRenderVramTransferDescription {
    XgRenderVramOperation operation;
    uint64_t guest_cycle;
    uint64_t source_interval;
    uint64_t device_mutation_serial;
    uint64_t required_source_generation;
    uint16_t source_x;
    uint16_t source_y;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    size_t payload_size;
    uint64_t payload_source_receipt;
    uint32_t payload_source;
    uint32_t payload_format;
    uint32_t command_source_address;
    uint32_t command_pc;
    uint32_t command_function;
    uint32_t command_return_address;
    uint32_t command_source_kind;
    uint32_t command_words[4];
    uint8_t command_opcode;
    uint8_t command_word_count;
    bool command_context_valid;
    bool payload_authenticated;
} XgRenderVramTransferDescription;

typedef struct XgRenderVramMutation {
    uint64_t event_serial;
    uint64_t serial;
    uint64_t source_generation;
    uint64_t guest_cycle;
    uint64_t source_interval;
    uint64_t device_mutation_serial;
    uint64_t content_digest;
    XgRenderVramOperation operation;
    XgRenderVramDirection direction;
    uint16_t source_x;
    uint16_t source_y;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    size_t payload_size;
    uint64_t payload_source_receipt;
    uint32_t payload_source;
    uint32_t payload_format;
    uint32_t command_source_address;
    uint32_t command_pc;
    uint32_t command_function;
    uint32_t command_return_address;
    uint32_t command_source_kind;
    uint32_t command_words[4];
    uint8_t command_opcode;
    uint8_t command_word_count;
    bool command_context_valid;
    bool payload_authenticated;
} XgRenderVramMutation;

typedef enum XgRenderVramResult {
    XG_RENDER_VRAM_OK = 0,
    XG_RENDER_VRAM_INVALID_ARGUMENT,
    XG_RENDER_VRAM_CAPACITY_EXCEEDED,
    XG_RENDER_VRAM_OUT_OF_MEMORY,
    XG_RENDER_VRAM_STALE_TRANSFER,
    XG_RENDER_VRAM_INCOMPLETE_TRANSFER,
    XG_RENDER_VRAM_OVERLAPPING_WRITE,
    XG_RENDER_VRAM_STALE_SOURCE_GENERATION,
} XgRenderVramResult;

typedef struct XgRenderVramJournalSnapshot {
    uint64_t event_serial;
    uint64_t mutation_serial;
    uint64_t completed_transfers;
    uint64_t readback_transfers;
    uint64_t scanouts;
    uint64_t restorations;
    uint64_t cancelled_transfers;
    uint64_t partial_publish_attempts;
    uint64_t ownership_violations;
    uint64_t observed_word_count;
    uint64_t authoritative_word_count;
    uint32_t active_transfers;
} XgRenderVramJournalSnapshot;

typedef struct XgRenderVramJournalCheckpointRestore
    XgRenderVramJournalCheckpointRestore;

void xg_render_vram_journal_reset(void);
void xg_render_vram_journal_cancel_transfers(void);
XgRenderVramResult xg_render_vram_transfer_begin(
    const XgRenderVramTransferDescription *description,
    XgRenderVramTransferHandle *out_transfer);
XgRenderVramResult xg_render_vram_transfer_write(
    XgRenderVramTransferHandle transfer,
    size_t payload_offset,
    const void *bytes,
    size_t byte_count);
XgRenderVramResult xg_render_vram_transfer_complete(
    XgRenderVramTransferHandle transfer,
    XgRenderVramMutation *out_mutation);
XgRenderVramResult xg_render_vram_publish_digest(
    const XgRenderVramTransferDescription *description,
    uint64_t content_digest,
    XgRenderVramMutation *out_mutation);
XgRenderVramResult xg_render_vram_transfer_cancel(
    XgRenderVramTransferHandle transfer);
void xg_render_vram_journal_snapshot(
    XgRenderVramJournalSnapshot *out_snapshot);
size_t xg_render_vram_journal_copy_mutations(
    XgRenderVramMutation *out_mutations, size_t mutation_capacity,
    uint64_t *out_mutation_total);
bool xg_render_vram_journal_copy_authoritative_rect(
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    uint16_t *out_pixels, size_t pixel_capacity,
    uint64_t *out_vram_generation);
bool xg_render_vram_journal_authenticate_rect(
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const void *canonical_bytes, size_t byte_count);
size_t xg_render_vram_journal_checkpoint_size(void);
bool xg_render_vram_journal_checkpoint_write(
    void *out_checkpoint, size_t checkpoint_size);
bool xg_render_vram_journal_checkpoint_prepare(
    const void *checkpoint, size_t checkpoint_size,
    XgRenderVramJournalCheckpointRestore **out_restore,
    uint64_t *out_saved_mutation_generation);
void xg_render_vram_journal_checkpoint_commit(
    XgRenderVramJournalCheckpointRestore *restore,
    uint64_t restored_mutation_generation);
void xg_render_vram_journal_checkpoint_cancel(
    XgRenderVramJournalCheckpointRestore *restore);

#ifdef __cplusplus
}
#endif

#endif
