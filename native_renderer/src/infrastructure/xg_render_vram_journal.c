#include "xg_render_vram_journal.h"

#include "xg_render_resource_repository.h"

#include <stdlib.h>
#include <string.h>

enum {
    XG_RENDER_VRAM_PIXEL_COUNT = 1024u * 512u,
    XG_RENDER_VRAM_BITSET_SIZE = XG_RENDER_VRAM_PIXEL_COUNT / 8u,
    XG_RENDER_VRAM_JOURNAL_CHECKPOINT_MAGIC = 0x4a564758u,
    XG_RENDER_VRAM_JOURNAL_CHECKPOINT_VERSION = 1u,
    XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE = 64u,
};

typedef struct XgRenderVramTransfer {
    XgRenderVramTransferDescription description;
    uint8_t *payload;
    uint8_t *written;
    size_t written_count;
    uint32_t generation;
    bool active;
} XgRenderVramTransfer;

static XgRenderVramTransfer g_transfers[XG_RENDER_VRAM_TRANSFER_CAPACITY];
static XgRenderVramJournalSnapshot g_snapshot;
static XgRenderVramMutation
    g_mutation_history[XG_RENDER_VRAM_MUTATION_HISTORY_CAPACITY];
static uint16_t g_authoritative_vram[XG_RENDER_VRAM_PIXEL_COUNT];
static uint8_t g_authoritative_vram_bits[XG_RENDER_VRAM_BITSET_SIZE];
static uint8_t g_observed_vram_bits[XG_RENDER_VRAM_BITSET_SIZE];

struct XgRenderVramJournalCheckpointRestore {
    uint16_t values[XG_RENDER_VRAM_PIXEL_COUNT];
    uint8_t authoritative_bits[XG_RENDER_VRAM_BITSET_SIZE];
    uint8_t observed_bits[XG_RENDER_VRAM_BITSET_SIZE];
    uint64_t saved_mutation_generation;
    uint64_t values_digest;
};

static void checkpoint_write_u32(uint8_t **cursor, uint32_t value) {
    for (uint32_t index = 0u; index < 4u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 4u;
}

static void checkpoint_write_u64(uint8_t **cursor, uint64_t value) {
    for (uint32_t index = 0u; index < 8u; ++index)
        (*cursor)[index] = (uint8_t)(value >> (index * 8u));
    *cursor += 8u;
}

static uint32_t checkpoint_read_u32(const uint8_t **cursor) {
    uint32_t value = 0u;

    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)(*cursor)[index] << (index * 8u);
    *cursor += 4u;
    return value;
}

static uint64_t checkpoint_read_u64(const uint8_t **cursor) {
    uint64_t value = 0u;

    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)(*cursor)[index] << (index * 8u);
    *cursor += 8u;
    return value;
}

static size_t bit_count(const uint8_t *bits) {
    size_t count = 0u;

    for (size_t index = 0u; index < XG_RENDER_VRAM_PIXEL_COUNT; ++index)
        if ((bits[index >> 3u] &
             (uint8_t)(1u << (index & 7u))) != 0u)
            ++count;
    return count;
}

static void set_authoritative_vram_bit(size_t index, bool authoritative) {
    const uint8_t mask = (uint8_t)(1u << (index & 7u));

    if (authoritative)
        g_authoritative_vram_bits[index >> 3u] |= mask;
    else
        g_authoritative_vram_bits[index >> 3u] &= (uint8_t)~mask;
}

static bool authoritative_vram_bit(size_t index) {
    return (g_authoritative_vram_bits[index >> 3u] &
            (uint8_t)(1u << (index & 7u))) != 0u;
}

static void set_observed_vram_bit(size_t index, bool observed) {
    const uint8_t mask = (uint8_t)(1u << (index & 7u));

    if (observed)
        g_observed_vram_bits[index >> 3u] |= mask;
    else
        g_observed_vram_bits[index >> 3u] &= (uint8_t)~mask;
}

static bool observed_vram_bit(size_t index) {
    return (g_observed_vram_bits[index >> 3u] &
            (uint8_t)(1u << (index & 7u))) != 0u;
}

static void apply_authoritative_payload(
        const XgRenderVramTransferDescription *description,
        const uint8_t *payload, bool mutates_vram) {
    const size_t expected_size =
        (size_t)description->width * description->height * sizeof(uint16_t);
    const bool authenticated = mutates_vram &&
        description->payload_authenticated && payload != NULL &&
        description->payload_size == expected_size;

    if (!mutates_vram) return;
    for (uint32_t row = 0u; row < description->height; ++row) {
        for (uint32_t column = 0u; column < description->width; ++column) {
            const size_t source =
                ((size_t)row * description->width + column) * 2u;
            const size_t destination =
                (size_t)(((uint32_t)description->y + row) & 511u) * 1024u +
                (((uint32_t)description->x + column) & 1023u);

            if (payload != NULL && description->payload_size == expected_size) {
                g_authoritative_vram[destination] =
                    (uint16_t)payload[source] |
                    (uint16_t)payload[source + 1u] << 8u;
                set_observed_vram_bit(destination, true);
            } else {
                set_observed_vram_bit(destination, false);
            }
            set_authoritative_vram_bit(destination, authenticated);
        }
    }
}

static bool operation_properties(XgRenderVramOperation operation,
                                 XgRenderVramDirection *out_direction,
                                 bool *out_mutates_vram) {
    switch (operation) {
        case XG_RENDER_VRAM_UPLOAD:
            *out_direction = XG_RENDER_VRAM_CPU_TO_VRAM;
            *out_mutates_vram = true;
            return true;
        case XG_RENDER_VRAM_READBACK:
            *out_direction = XG_RENDER_VRAM_VRAM_TO_CPU;
            *out_mutates_vram = false;
            return true;
        case XG_RENDER_VRAM_MOVE:
            *out_direction = XG_RENDER_VRAM_VRAM_TO_VRAM;
            *out_mutates_vram = true;
            return true;
        case XG_RENDER_VRAM_CLEAR:
        case XG_RENDER_VRAM_MDEC_STRIP:
        case XG_RENDER_VRAM_RENDER_TARGET_WRITE:
            *out_direction = XG_RENDER_VRAM_GPU_TO_VRAM;
            *out_mutates_vram = true;
            return true;
        case XG_RENDER_VRAM_RESTORE:
            *out_direction = XG_RENDER_VRAM_CPU_TO_VRAM;
            *out_mutates_vram = true;
            return true;
        case XG_RENDER_VRAM_SCANOUT:
            *out_direction = XG_RENDER_VRAM_VRAM_TO_SCANOUT;
            *out_mutates_vram = false;
            return true;
    }
    return false;
}

static XgRenderVramTransfer *transfer_from_handle(
        XgRenderVramTransferHandle handle) {
    XgRenderVramTransfer *transfer;
    if (handle.slot >= XG_RENDER_VRAM_TRANSFER_CAPACITY) return NULL;
    transfer = &g_transfers[handle.slot];
    if (!transfer->active || transfer->generation != handle.generation)
        return NULL;
    return transfer;
}

static void release_transfer(XgRenderVramTransfer *transfer) {
    free(transfer->payload);
    free(transfer->written);
    transfer->payload = NULL;
    transfer->written = NULL;
    transfer->active = false;
    transfer->written_count = 0u;
    transfer->generation++;
    if (transfer->generation == 0u) transfer->generation = 1u;
}

void xg_render_vram_journal_reset(void) {
    uint32_t index;
    const uint64_t event_serial = g_snapshot.event_serial;
    const uint64_t mutation_serial = g_snapshot.mutation_serial;
    for (index = 0; index < XG_RENDER_VRAM_TRANSFER_CAPACITY; index++) {
        uint32_t generation = g_transfers[index].generation + 1u;
        free(g_transfers[index].payload);
        free(g_transfers[index].written);
        memset(&g_transfers[index], 0, sizeof(g_transfers[index]));
        g_transfers[index].generation = generation == 0u ? 1u : generation;
    }
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.event_serial = event_serial;
    g_snapshot.mutation_serial = mutation_serial;
    memset(g_authoritative_vram, 0, sizeof(g_authoritative_vram));
    memset(g_authoritative_vram_bits, 0, sizeof(g_authoritative_vram_bits));
    memset(g_observed_vram_bits, 0, sizeof(g_observed_vram_bits));
}

void xg_render_vram_journal_cancel_transfers(void) {
    for (uint32_t index = 0u;
         index < XG_RENDER_VRAM_TRANSFER_CAPACITY; ++index) {
        XgRenderVramTransfer *transfer = &g_transfers[index];

        if (!transfer->active) continue;
        release_transfer(transfer);
        g_snapshot.cancelled_transfers++;
    }
    g_snapshot.active_transfers = 0u;
}

XgRenderVramResult xg_render_vram_transfer_begin(
        const XgRenderVramTransferDescription *description,
        XgRenderVramTransferHandle *out_transfer) {
    uint32_t index;
    XgRenderVramDirection direction;
    bool mutates_vram;
    if (description == NULL || out_transfer == NULL || description->width == 0u ||
        description->height == 0u || description->payload_size == 0u ||
        !operation_properties(description->operation, &direction, &mutates_vram))
        return XG_RENDER_VRAM_INVALID_ARGUMENT;
    for (index = 0; index < XG_RENDER_VRAM_TRANSFER_CAPACITY; index++) {
        XgRenderVramTransfer *transfer = &g_transfers[index];
        if (transfer->active) continue;
        transfer->payload = (uint8_t *)malloc(description->payload_size);
        transfer->written = (uint8_t *)calloc(description->payload_size, 1u);
        if (transfer->payload == NULL || transfer->written == NULL) {
            free(transfer->payload);
            free(transfer->written);
            transfer->payload = NULL;
            transfer->written = NULL;
            return XG_RENDER_VRAM_OUT_OF_MEMORY;
        }
        if (transfer->generation == 0u) transfer->generation = 1u;
        transfer->description = *description;
        transfer->written_count = 0u;
        transfer->active = true;
        *out_transfer = (XgRenderVramTransferHandle){ index,
                                                       transfer->generation };
        g_snapshot.active_transfers++;
        return XG_RENDER_VRAM_OK;
    }
    return XG_RENDER_VRAM_CAPACITY_EXCEEDED;
}

XgRenderVramResult xg_render_vram_transfer_write(
        XgRenderVramTransferHandle handle, size_t payload_offset,
        const void *bytes, size_t byte_count) {
    XgRenderVramTransfer *transfer = transfer_from_handle(handle);
    size_t index;
    if (transfer == NULL) return XG_RENDER_VRAM_STALE_TRANSFER;
    if (bytes == NULL || byte_count == 0u ||
        payload_offset > transfer->description.payload_size ||
        byte_count > transfer->description.payload_size - payload_offset)
        return XG_RENDER_VRAM_INVALID_ARGUMENT;
    for (index = 0; index < byte_count; index++) {
        if (transfer->written[payload_offset + index] != 0u)
            return XG_RENDER_VRAM_OVERLAPPING_WRITE;
    }
    memcpy(transfer->payload + payload_offset, bytes, byte_count);
    memset(transfer->written + payload_offset, 1, byte_count);
    transfer->written_count += byte_count;
    return XG_RENDER_VRAM_OK;
}

static XgRenderVramResult publish_digest(
        const XgRenderVramTransferDescription *description,
        uint64_t content_digest, const uint8_t *payload,
        XgRenderVramMutation *out_mutation) {
    XgRenderVramDirection direction;
    bool mutates_vram;
    uint64_t source_generation;

    if (description == NULL || out_mutation == NULL ||
        description->width == 0u || description->height == 0u ||
        description->payload_size == 0u ||
        !operation_properties(
            description->operation, &direction, &mutates_vram))
        return XG_RENDER_VRAM_INVALID_ARGUMENT;
    source_generation = g_snapshot.mutation_serial;
    if (description->required_source_generation != 0u &&
        description->required_source_generation != source_generation) {
        g_snapshot.ownership_violations++;
        return XG_RENDER_VRAM_STALE_SOURCE_GENERATION;
    }
    g_snapshot.event_serial++;
    if (g_snapshot.event_serial == 0u) g_snapshot.event_serial = 1u;
    if (mutates_vram) {
        g_snapshot.mutation_serial++;
        if (g_snapshot.mutation_serial == 0u)
            g_snapshot.mutation_serial = 1u;
    }
    *out_mutation = (XgRenderVramMutation){
        .event_serial = g_snapshot.event_serial,
        .serial = g_snapshot.mutation_serial,
        .source_generation = source_generation,
        .guest_cycle = description->guest_cycle,
        .source_interval = description->source_interval,
        .device_mutation_serial = description->device_mutation_serial,
        .content_digest = content_digest,
        .operation = description->operation,
        .direction = direction,
        .source_x = description->source_x,
        .source_y = description->source_y,
        .x = description->x,
        .y = description->y,
        .width = description->width,
        .height = description->height,
        .payload_size = description->payload_size,
        .payload_source_receipt = description->payload_source_receipt,
        .payload_source = description->payload_source,
        .payload_format = description->payload_format,
        .command_source_address = description->command_source_address,
        .command_pc = description->command_pc,
        .command_function = description->command_function,
        .command_return_address = description->command_return_address,
        .command_source_kind = description->command_source_kind,
        .command_opcode = description->command_opcode,
        .command_word_count = description->command_word_count,
        .command_context_valid = description->command_context_valid,
        .payload_authenticated = description->payload_authenticated,
    };
    memcpy(out_mutation->command_words, description->command_words,
           sizeof(out_mutation->command_words));
    apply_authoritative_payload(description, payload, mutates_vram);
    g_mutation_history[(g_snapshot.event_serial - 1u) %
        XG_RENDER_VRAM_MUTATION_HISTORY_CAPACITY] = *out_mutation;
    g_snapshot.completed_transfers++;
    if (description->operation == XG_RENDER_VRAM_READBACK)
        g_snapshot.readback_transfers++;
    else if (description->operation == XG_RENDER_VRAM_SCANOUT)
        g_snapshot.scanouts++;
    else if (description->operation == XG_RENDER_VRAM_RESTORE)
        g_snapshot.restorations++;
    return XG_RENDER_VRAM_OK;
}

XgRenderVramResult xg_render_vram_transfer_complete(
        XgRenderVramTransferHandle handle, XgRenderVramMutation *out_mutation) {
    XgRenderVramTransfer *transfer = transfer_from_handle(handle);
    XgRenderVramResult result;
    if (transfer == NULL) return XG_RENDER_VRAM_STALE_TRANSFER;
    if (out_mutation == NULL) return XG_RENDER_VRAM_INVALID_ARGUMENT;
    if (transfer->written_count != transfer->description.payload_size) {
        g_snapshot.partial_publish_attempts++;
        return XG_RENDER_VRAM_INCOMPLETE_TRANSFER;
    }
    result = publish_digest(
        &transfer->description,
        xg_render_resource_digest(
            transfer->payload, transfer->description.payload_size),
        transfer->payload, out_mutation);
    if (result != XG_RENDER_VRAM_OK) return result;
    release_transfer(transfer);
    g_snapshot.active_transfers--;
    return XG_RENDER_VRAM_OK;
}

XgRenderVramResult xg_render_vram_publish_digest(
        const XgRenderVramTransferDescription *description,
        uint64_t content_digest,
        XgRenderVramMutation *out_mutation) {
    return publish_digest(description, content_digest, NULL, out_mutation);
}

XgRenderVramResult xg_render_vram_transfer_cancel(
        XgRenderVramTransferHandle handle) {
    XgRenderVramTransfer *transfer = transfer_from_handle(handle);
    if (transfer == NULL) return XG_RENDER_VRAM_STALE_TRANSFER;
    release_transfer(transfer);
    g_snapshot.cancelled_transfers++;
    g_snapshot.active_transfers--;
    return XG_RENDER_VRAM_OK;
}

void xg_render_vram_journal_snapshot(XgRenderVramJournalSnapshot *out_snapshot) {
    if (out_snapshot == NULL) return;
    *out_snapshot = g_snapshot;
    out_snapshot->observed_word_count = bit_count(g_observed_vram_bits);
    out_snapshot->authoritative_word_count =
        bit_count(g_authoritative_vram_bits);
}

size_t xg_render_vram_journal_copy_mutations(
        XgRenderVramMutation *out_mutations, size_t mutation_capacity,
        uint64_t *out_mutation_total) {
    const uint64_t available = g_snapshot.event_serial <
            XG_RENDER_VRAM_MUTATION_HISTORY_CAPACITY
        ? g_snapshot.event_serial : XG_RENDER_VRAM_MUTATION_HISTORY_CAPACITY;
    const size_t copied = mutation_capacity < available
        ? mutation_capacity : (size_t)available;
    const uint64_t first = g_snapshot.event_serial - copied;

    if (out_mutation_total != NULL)
        *out_mutation_total = g_snapshot.event_serial;
    if (copied != 0u && out_mutations == NULL) return 0u;
    for (size_t index = 0u; index < copied; ++index) {
        const uint64_t serial = first + index + 1u;
        out_mutations[index] = g_mutation_history[(serial - 1u) %
            XG_RENDER_VRAM_MUTATION_HISTORY_CAPACITY];
    }
    return copied;
}

bool xg_render_vram_journal_copy_authoritative_rect(
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        uint16_t *out_pixels, size_t pixel_capacity,
        uint64_t *out_vram_generation) {
    const size_t pixel_count = (size_t)width * height;

    if (out_pixels == NULL || out_vram_generation == NULL ||
        width == 0u || height == 0u || width > 1024u || height > 512u ||
        pixel_capacity < pixel_count)
        return false;
    for (uint32_t row = 0u; row < height; ++row) {
        for (uint32_t column = 0u; column < width; ++column) {
            const size_t source =
                (size_t)(((uint32_t)y + row) & 511u) * 1024u +
                (((uint32_t)x + column) & 1023u);

            if (!authoritative_vram_bit(source)) return false;
            out_pixels[(size_t)row * width + column] =
                g_authoritative_vram[source];
        }
    }
    *out_vram_generation = g_snapshot.mutation_serial;
    return true;
}

bool xg_render_vram_journal_authenticate_rect(
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        const void *canonical_bytes, size_t byte_count) {
    const uint8_t *bytes = (const uint8_t *)canonical_bytes;
    const size_t expected_size = (size_t)width * height * sizeof(uint16_t);

    if (bytes == NULL || width == 0u || height == 0u || width > 1024u ||
        height > 512u || byte_count != expected_size)
        return false;
    for (uint32_t row = 0u; row < height; ++row) {
        for (uint32_t column = 0u; column < width; ++column) {
            const size_t source = ((size_t)row * width + column) * 2u;
            const size_t destination =
                (size_t)(((uint32_t)y + row) & 511u) * 1024u +
                (((uint32_t)x + column) & 1023u);

            const uint16_t value = (uint16_t)bytes[source] |
                (uint16_t)bytes[source + 1u] << 8u;

            if (!observed_vram_bit(destination) ||
                g_authoritative_vram[destination] != value)
                return false;
        }
    }
    for (uint32_t row = 0u; row < height; ++row)
        for (uint32_t column = 0u; column < width; ++column) {
            const size_t destination =
                (size_t)(((uint32_t)y + row) & 511u) * 1024u +
                (((uint32_t)x + column) & 1023u);

            set_authoritative_vram_bit(destination, true);
        }
    return true;
}

size_t xg_render_vram_journal_checkpoint_size(void) {
    const size_t observed_count = bit_count(g_observed_vram_bits);

    if (g_snapshot.active_transfers != 0u ||
        observed_count > (SIZE_MAX -
            XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE -
            2u * XG_RENDER_VRAM_BITSET_SIZE) / sizeof(uint16_t))
        return 0u;
    return XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE +
        2u * XG_RENDER_VRAM_BITSET_SIZE +
        observed_count * sizeof(uint16_t);
}

bool xg_render_vram_journal_checkpoint_write(
        void *out_checkpoint, size_t checkpoint_size) {
    uint8_t *const checkpoint = (uint8_t *)out_checkpoint;
    uint8_t *payload;
    uint8_t *values;
    uint8_t *cursor;
    const size_t required_size = xg_render_vram_journal_checkpoint_size();
    const size_t observed_count = bit_count(g_observed_vram_bits);
    const size_t authoritative_count = bit_count(g_authoritative_vram_bits);

    if (checkpoint == NULL || required_size == 0u ||
        checkpoint_size != required_size ||
        authoritative_count > observed_count)
        return false;
    payload = checkpoint + XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE;
    memcpy(payload, g_observed_vram_bits, XG_RENDER_VRAM_BITSET_SIZE);
    memcpy(payload + XG_RENDER_VRAM_BITSET_SIZE,
           g_authoritative_vram_bits, XG_RENDER_VRAM_BITSET_SIZE);
    values = payload + 2u * XG_RENDER_VRAM_BITSET_SIZE;
    cursor = values;
    for (size_t index = 0u; index < XG_RENDER_VRAM_PIXEL_COUNT; ++index) {
        if (!observed_vram_bit(index)) {
            if (authoritative_vram_bit(index)) return false;
            continue;
        }
        *cursor++ = (uint8_t)g_authoritative_vram[index];
        *cursor++ = (uint8_t)(g_authoritative_vram[index] >> 8u);
    }
    if ((size_t)(cursor - checkpoint) != checkpoint_size) return false;

    cursor = checkpoint;
    checkpoint_write_u32(
        &cursor, XG_RENDER_VRAM_JOURNAL_CHECKPOINT_MAGIC);
    checkpoint_write_u32(
        &cursor, XG_RENDER_VRAM_JOURNAL_CHECKPOINT_VERSION);
    checkpoint_write_u64(&cursor, checkpoint_size);
    checkpoint_write_u64(&cursor, g_snapshot.mutation_serial);
    checkpoint_write_u64(&cursor, observed_count);
    checkpoint_write_u64(&cursor, authoritative_count);
    checkpoint_write_u64(&cursor, xg_render_resource_digest(
        payload, XG_RENDER_VRAM_BITSET_SIZE));
    checkpoint_write_u64(&cursor, xg_render_resource_digest(
        payload + XG_RENDER_VRAM_BITSET_SIZE,
        XG_RENDER_VRAM_BITSET_SIZE));
    checkpoint_write_u64(&cursor, xg_render_resource_digest(
        values, observed_count * sizeof(uint16_t)));
    return (size_t)(cursor - checkpoint) ==
        XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE;
}

bool xg_render_vram_journal_checkpoint_prepare(
        const void *checkpoint, size_t checkpoint_size,
        XgRenderVramJournalCheckpointRestore **out_restore,
        uint64_t *out_saved_mutation_generation) {
    const uint8_t *cursor = (const uint8_t *)checkpoint;
    const uint8_t *observed_bits;
    const uint8_t *authoritative_bits;
    const uint8_t *values;
    uint64_t declared_size;
    uint64_t saved_mutation_generation;
    uint64_t observed_count;
    uint64_t authoritative_count;
    uint64_t observed_digest;
    uint64_t authoritative_digest;
    uint64_t values_digest;
    size_t observed_found;
    size_t authoritative_found;
    XgRenderVramJournalCheckpointRestore *restore;

    if (out_restore == NULL || out_saved_mutation_generation == NULL)
        return false;
    *out_restore = NULL;
    *out_saved_mutation_generation = 0u;
    if (checkpoint == NULL || checkpoint_size <
            XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE +
                2u * XG_RENDER_VRAM_BITSET_SIZE ||
        checkpoint_read_u32(&cursor) !=
            XG_RENDER_VRAM_JOURNAL_CHECKPOINT_MAGIC ||
        checkpoint_read_u32(&cursor) !=
            XG_RENDER_VRAM_JOURNAL_CHECKPOINT_VERSION)
        return false;
    declared_size = checkpoint_read_u64(&cursor);
    saved_mutation_generation = checkpoint_read_u64(&cursor);
    observed_count = checkpoint_read_u64(&cursor);
    authoritative_count = checkpoint_read_u64(&cursor);
    observed_digest = checkpoint_read_u64(&cursor);
    authoritative_digest = checkpoint_read_u64(&cursor);
    values_digest = checkpoint_read_u64(&cursor);
    if (declared_size != checkpoint_size ||
        observed_count > XG_RENDER_VRAM_PIXEL_COUNT ||
        authoritative_count > observed_count ||
        observed_count > (SIZE_MAX -
            XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE -
            2u * XG_RENDER_VRAM_BITSET_SIZE) / sizeof(uint16_t) ||
        checkpoint_size != XG_RENDER_VRAM_JOURNAL_CHECKPOINT_HEADER_SIZE +
            2u * XG_RENDER_VRAM_BITSET_SIZE +
            (size_t)observed_count * sizeof(uint16_t))
        return false;
    observed_bits = cursor;
    authoritative_bits = observed_bits + XG_RENDER_VRAM_BITSET_SIZE;
    values = authoritative_bits + XG_RENDER_VRAM_BITSET_SIZE;
    if (xg_render_resource_digest(
            observed_bits, XG_RENDER_VRAM_BITSET_SIZE) != observed_digest ||
        xg_render_resource_digest(
            authoritative_bits, XG_RENDER_VRAM_BITSET_SIZE) !=
                authoritative_digest ||
        xg_render_resource_digest(
            values, (size_t)observed_count * sizeof(uint16_t)) != values_digest)
        return false;
    observed_found = bit_count(observed_bits);
    authoritative_found = bit_count(authoritative_bits);
    if (observed_found != observed_count ||
        authoritative_found != authoritative_count)
        return false;
    for (size_t index = 0u; index < XG_RENDER_VRAM_BITSET_SIZE; ++index)
        if ((authoritative_bits[index] &
             (uint8_t)~observed_bits[index]) != 0u)
            return false;

    restore = (XgRenderVramJournalCheckpointRestore *)calloc(
        1u, sizeof(*restore));
    if (restore == NULL) return false;
    memcpy(restore->observed_bits,
           observed_bits, XG_RENDER_VRAM_BITSET_SIZE);
    memcpy(restore->authoritative_bits,
           authoritative_bits, XG_RENDER_VRAM_BITSET_SIZE);
    cursor = values;
    for (size_t index = 0u; index < XG_RENDER_VRAM_PIXEL_COUNT; ++index) {
        if ((restore->observed_bits[index >> 3u] &
             (uint8_t)(1u << (index & 7u))) == 0u)
            continue;
        restore->values[index] = (uint16_t)cursor[0] |
            (uint16_t)cursor[1] << 8u;
        cursor += sizeof(uint16_t);
    }
    if ((size_t)(cursor - (const uint8_t *)checkpoint) != checkpoint_size) {
        free(restore);
        return false;
    }
    restore->saved_mutation_generation = saved_mutation_generation;
    restore->values_digest = values_digest;
    *out_saved_mutation_generation = saved_mutation_generation;
    *out_restore = restore;
    return true;
}

void xg_render_vram_journal_checkpoint_commit(
        XgRenderVramJournalCheckpointRestore *restore,
        uint64_t restored_mutation_generation) {
    XgRenderVramMutation mutation;
    uint64_t event_serial;
    uint64_t source_generation;

    if (restore == NULL || restored_mutation_generation == 0u) return;
    source_generation = g_snapshot.mutation_serial;
    event_serial = g_snapshot.event_serial + 1u;
    if (event_serial == 0u) event_serial = 1u;
    xg_render_vram_journal_cancel_transfers();
    memcpy(g_authoritative_vram,
           restore->values, sizeof(g_authoritative_vram));
    memcpy(g_authoritative_vram_bits,
           restore->authoritative_bits, sizeof(g_authoritative_vram_bits));
    memcpy(g_observed_vram_bits,
           restore->observed_bits, sizeof(g_observed_vram_bits));
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.event_serial = event_serial;
    g_snapshot.mutation_serial = restored_mutation_generation;
    g_snapshot.completed_transfers = 1u;
    g_snapshot.restorations = 1u;
    mutation = (XgRenderVramMutation){
        .event_serial = event_serial,
        .serial = restored_mutation_generation,
        .source_generation = source_generation,
        .content_digest = restore->values_digest,
        .operation = XG_RENDER_VRAM_RESTORE,
        .direction = XG_RENDER_VRAM_CPU_TO_VRAM,
        .width = 1024u,
        .height = 512u,
        .payload_size = XG_RENDER_VRAM_PIXEL_COUNT * sizeof(uint16_t),
    };
    g_mutation_history[(event_serial - 1u) %
        XG_RENDER_VRAM_MUTATION_HISTORY_CAPACITY] = mutation;
    free(restore);
}

void xg_render_vram_journal_checkpoint_cancel(
        XgRenderVramJournalCheckpointRestore *restore) {
    free(restore);
}
