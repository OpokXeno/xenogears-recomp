#include "xg_render_local_producer_auth.h"

#include "psx_sha256.h"

#include <string.h>

typedef struct XgRenderLocalProducerAuthority {
    uint64_t generation;
    uint64_t scene_generation;
    uint32_t code_range_start;
    uint32_t code_range_size;
    bool valid;
} XgRenderLocalProducerAuthority;

typedef struct XgRenderLocalProducerPending {
    uint64_t generation;
    uint64_t scene_generation;
    uint32_t code_range_start;
    uint32_t code_range_size;
    uint8_t code_range_identity[32];
    uint32_t writer_count;
    XgRenderLocalProducerKind kind;
    bool valid;
} XgRenderLocalProducerPending;

typedef struct XgRenderLocalProducerDescriptor {
    uint32_t entry_pc;
    uint32_t writer_pc;
    uint32_t commit_pc;
    uint32_t expected_writer_count;
    uint32_t handler_data;
    XgRenderRuntimeVariantCutoverHandler begin_handler;
    XgRenderRuntimeVariantCutoverHandler writer_handler;
    XgRenderRuntimeVariantCutoverHandler commit_handler;
} XgRenderLocalProducerDescriptor;

static const XgRenderLocalProducerDescriptor descriptors[
    XG_RENDER_LOCAL_PRODUCER_COUNT] = {
    [XG_RENDER_LOCAL_PRODUCER_FT4_2C] = {
        .entry_pc = UINT32_C(0x8007a7f4),
        .writer_pc = UINT32_C(0x8007a9f4),
        .commit_pc = UINT32_C(0x8007aa3c),
        .expected_writer_count = 2u,
        .handler_data = 0x2cu,
        .begin_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_BEGIN,
        .writer_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_WRITER,
        .commit_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_COMMIT,
    },
    [XG_RENDER_LOCAL_PRODUCER_FT4_2E] = {
        .entry_pc = UINT32_C(0x8007aa44),
        .writer_pc = UINT32_C(0x8007ab2c),
        .commit_pc = UINT32_C(0x8007ab64),
        .expected_writer_count = 2u,
        .handler_data = 0x2eu,
        .begin_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_BEGIN,
        .writer_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_WRITER,
        .commit_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_RESOURCE_INITIALIZER_COMMIT,
    },
    [XG_RENDER_LOCAL_PRODUCER_ZOOM] = {
        .entry_pc = UINT32_C(0x800a663c),
        .writer_pc = UINT32_C(0x800a6884),
        .commit_pc = UINT32_C(0x800a68f0),
        .expected_writer_count = 10u,
        .begin_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_BEGIN,
        .writer_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_WRITER,
        .commit_handler =
            XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_INITIALIZER_COMMIT,
    },
};

static XgRenderLocalProducerAuthHandler handlers[
    XG_RENDER_LOCAL_PRODUCER_COUNT];
static XgRenderLocalProducerAuthority authorities[
    XG_RENDER_LOCAL_PRODUCER_COUNT];
static XgRenderLocalProducerPending pending;
static uint64_t next_generation = 1u;

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static bool handlers_equal(const XgRenderLocalProducerAuthHandler *left,
                           const XgRenderLocalProducerAuthHandler *right) {
    return left != NULL && right != NULL &&
        left->preflight == right->preflight && left->begin == right->begin &&
        left->writer == right->writer && left->commit == right->commit &&
        left->cancel == right->cancel;
}

static bool range_contains(uint32_t range_start, uint32_t range_size,
                           uint32_t value, uint32_t value_size) {
    const uint64_t start = range_start & UINT32_C(0x1fffffff);
    const uint64_t end = start + range_size;
    const uint64_t physical_value = value & UINT32_C(0x1fffffff);

    return range_size != 0u && value_size != 0u &&
        physical_value >= start && physical_value + value_size <= end;
}

static bool ranges_overlap(uint32_t left_start, uint32_t left_size,
                           uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & UINT32_C(0x1fffffff);
    const uint64_t left_end = left_begin + left_size;
    const uint64_t right_begin = right_start & UINT32_C(0x1fffffff);
    const uint64_t right_end = right_begin + right_size;

    return left_size != 0u && right_size != 0u &&
        left_begin < right_end && right_begin < left_end;
}

static void clear_pending(void) {
    if (pending.valid && pending.kind > XG_RENDER_LOCAL_PRODUCER_NONE &&
        pending.kind < XG_RENDER_LOCAL_PRODUCER_COUNT &&
        handlers[pending.kind].cancel != NULL)
        handlers[pending.kind].cancel();
    pending = (XgRenderLocalProducerPending){0};
}

static XgRenderLocalProducerKind kind_for_entry(uint32_t pc) {
    for (uint32_t kind = XG_RENDER_LOCAL_PRODUCER_FT4_2C;
         kind < XG_RENDER_LOCAL_PRODUCER_COUNT; ++kind) {
        if (handlers[kind].begin != NULL &&
            physical_address_equals(pc, descriptors[kind].entry_pc))
            return (XgRenderLocalProducerKind)kind;
    }
    return XG_RENDER_LOCAL_PRODUCER_NONE;
}

static bool code_range_matches(
        CPUState *cpu, const XgRenderRuntimeVariantCutover *cutover) {
    psx_sha256_ctx digest;
    uint8_t actual[32];

    if (cpu == NULL || cpu->read_word == NULL || cutover == NULL ||
        cutover->code_range_size == 0u ||
        (cutover->code_range_start & 3u) != 0u ||
        (cutover->code_range_size & 3u) != 0u)
        return false;
    psx_sha256_init(&digest);
    for (uint32_t offset = 0u; offset < cutover->code_range_size; offset += 4u) {
        const uint32_t word = cpu->read_word(cutover->code_range_start + offset);
        const uint8_t bytes[4] = {
            (uint8_t)word, (uint8_t)(word >> 8u),
            (uint8_t)(word >> 16u), (uint8_t)(word >> 24u),
        };

        psx_sha256_update(&digest, bytes, sizeof(bytes));
    }
    psx_sha256_final(&digest, actual);
    return memcmp(actual, cutover->code_range_identity, sizeof(actual)) == 0;
}

static bool contract_matches_pending(
        const XgRenderRuntimeVariantCutover *cutover) {
    return cutover != NULL && pending.valid &&
        physical_address_equals(cutover->code_range_start,
                                pending.code_range_start) &&
        cutover->code_range_size == pending.code_range_size &&
        memcmp(cutover->code_range_identity, pending.code_range_identity,
               sizeof(pending.code_range_identity)) == 0;
}

static XgRenderLocalProducerAuthContext make_context(
        CPUState *cpu, uint32_t pc, uint64_t generation,
        uint64_t scene_generation, GuestRenderRenderMode render_mode,
        const XgRenderLocalProducerDescriptor *descriptor) {
    return (XgRenderLocalProducerAuthContext){
        .cpu = cpu,
        .generation = generation,
        .scene_generation = scene_generation,
        .pc = pc,
        .handler_data = descriptor->handler_data,
        .render_mode = render_mode,
    };
}

bool xg_render_local_producer_auth_register(
        XgRenderLocalProducerKind kind,
        const XgRenderLocalProducerAuthHandler *handler) {
    if (kind <= XG_RENDER_LOCAL_PRODUCER_NONE ||
        kind >= XG_RENDER_LOCAL_PRODUCER_COUNT || handler == NULL ||
        handler->preflight == NULL || handler->begin == NULL ||
        handler->writer == NULL || handler->commit == NULL ||
        descriptors[kind].entry_pc == 0u ||
        descriptors[kind].writer_pc == 0u ||
        descriptors[kind].commit_pc == 0u ||
        descriptors[kind].expected_writer_count == 0u ||
        handlers[kind].begin != NULL)
        return false;
    handlers[kind] = *handler;
    return true;
}

bool xg_render_local_producer_auth_unregister(
        XgRenderLocalProducerKind kind,
        const XgRenderLocalProducerAuthHandler *handler) {
    if (kind <= XG_RENDER_LOCAL_PRODUCER_NONE ||
        kind >= XG_RENDER_LOCAL_PRODUCER_COUNT ||
        !handlers_equal(&handlers[kind], handler))
        return false;
    if (pending.valid && pending.kind == kind) clear_pending();
    handlers[kind] = (XgRenderLocalProducerAuthHandler){0};
    authorities[kind] = (XgRenderLocalProducerAuthority){0};
    return true;
}

bool xg_render_local_producer_auth_begin(
        CPUState *cpu, const XgRenderRuntimeVariantCutover *cutover,
        uint64_t scene_generation, GuestRenderRenderMode render_mode) {
    XgRenderLocalProducerKind kind;
    XgRenderLocalProducerAuthHandler *handler;
    const XgRenderLocalProducerDescriptor *descriptor;
    XgRenderLocalProducerAuthContext context;

    clear_pending();
    if (cutover == NULL) return false;
    kind = kind_for_entry(cutover->pc);
    if (kind == XG_RENDER_LOCAL_PRODUCER_NONE) return false;
    handler = &handlers[kind];
    descriptor = &descriptors[kind];
    if (cutover->handler != descriptor->begin_handler) return false;
    context = make_context(cpu, cutover->pc, 0u, scene_generation,
                           render_mode, descriptor);
    if (!handler->preflight(&context) || next_generation == 0u ||
        next_generation == UINT64_MAX || !code_range_matches(cpu, cutover))
        return false;
    pending = (XgRenderLocalProducerPending){
        .generation = next_generation++,
        .scene_generation = scene_generation,
        .code_range_start = cutover->code_range_start,
        .code_range_size = cutover->code_range_size,
        .kind = kind,
        .valid = true,
    };
    memcpy(pending.code_range_identity, cutover->code_range_identity,
           sizeof(pending.code_range_identity));
    context = make_context(cpu, cutover->pc, pending.generation,
                           scene_generation, render_mode, descriptor);
    if (!handler->begin(&context)) {
        clear_pending();
        return false;
    }
    return true;
}

bool xg_render_local_producer_auth_writer(
        CPUState *cpu, const XgRenderRuntimeVariantCutover *cutover,
        uint64_t scene_generation, GuestRenderRenderMode render_mode) {
    XgRenderLocalProducerAuthHandler *handler;
    const XgRenderLocalProducerDescriptor *descriptor;
    XgRenderLocalProducerAuthContext context;

    if (!contract_matches_pending(cutover) ||
        pending.scene_generation != scene_generation)
        goto reject;
    handler = &handlers[pending.kind];
    descriptor = &descriptors[pending.kind];
    if (cutover->handler != descriptor->writer_handler ||
        !physical_address_equals(cutover->pc, descriptor->writer_pc) ||
        pending.writer_count >= descriptor->expected_writer_count)
        goto reject;
    context = make_context(cpu, cutover->pc, pending.generation,
                           scene_generation, render_mode, descriptor);
    if (!handler->writer(&context, pending.writer_count)) goto reject;
    ++pending.writer_count;
    return true;

reject:
    clear_pending();
    return false;
}

bool xg_render_local_producer_auth_commit(
        CPUState *cpu, const XgRenderRuntimeVariantCutover *cutover,
        uint64_t scene_generation, GuestRenderRenderMode render_mode) {
    XgRenderLocalProducerAuthHandler *handler;
    const XgRenderLocalProducerDescriptor *descriptor;
    XgRenderLocalProducerAuthority *authority;
    XgRenderLocalProducerAuthContext context;
    bool committed;

    if (!contract_matches_pending(cutover) ||
        pending.scene_generation != scene_generation)
        goto reject;
    handler = &handlers[pending.kind];
    descriptor = &descriptors[pending.kind];
    if (cutover->handler != descriptor->commit_handler ||
        !physical_address_equals(cutover->pc, descriptor->commit_pc) ||
        pending.writer_count != descriptor->expected_writer_count)
        goto reject;
    authority = &authorities[pending.kind];
    *authority = (XgRenderLocalProducerAuthority){
        .generation = pending.generation,
        .scene_generation = scene_generation,
        .code_range_start = pending.code_range_start,
        .code_range_size = pending.code_range_size,
        .valid = true,
    };
    context = make_context(cpu, cutover->pc, pending.generation,
                           scene_generation, render_mode, descriptor);
    committed = handler->commit(&context);
    if (!committed) *authority = (XgRenderLocalProducerAuthority){0};
    clear_pending();
    return committed;

reject:
    clear_pending();
    return false;
}

uint64_t xg_render_local_producer_auth_generation_for_pc(
        uint32_t pc, uint64_t scene_generation) {
    for (uint32_t kind = XG_RENDER_LOCAL_PRODUCER_FT4_2C;
         kind < XG_RENDER_LOCAL_PRODUCER_COUNT; ++kind) {
        const XgRenderLocalProducerAuthority *authority = &authorities[kind];

        if (authority->valid && authority->generation != 0u &&
            authority->scene_generation == scene_generation &&
            range_contains(authority->code_range_start,
                           authority->code_range_size, pc, 4u))
            return authority->generation;
    }
    return 0u;
}

bool xg_render_local_producer_auth_matches(
        uint32_t pc, uint64_t generation, uint64_t scene_generation) {
    return generation != 0u &&
        xg_render_local_producer_auth_generation_for_pc(
            pc, scene_generation) == generation;
}

bool xg_render_local_producer_auth_pending_authorizes_pc(uint32_t pc) {
    return pending.valid && range_contains(
        pending.code_range_start, pending.code_range_size, pc, 4u);
}

bool xg_render_local_producer_auth_pending(void) { return pending.valid; }

void xg_render_local_producer_auth_clear_kind(XgRenderLocalProducerKind kind) {
    if (kind <= XG_RENDER_LOCAL_PRODUCER_NONE ||
        kind >= XG_RENDER_LOCAL_PRODUCER_COUNT)
        return;
    if (pending.valid && pending.kind == kind) clear_pending();
    authorities[kind] = (XgRenderLocalProducerAuthority){0};
}

void xg_render_local_producer_auth_invalidate(uint32_t address, uint32_t size) {
    if (pending.valid && ranges_overlap(
            pending.code_range_start, pending.code_range_size, address, size))
        clear_pending();
    for (uint32_t kind = XG_RENDER_LOCAL_PRODUCER_FT4_2C;
         kind < XG_RENDER_LOCAL_PRODUCER_COUNT; ++kind) {
        XgRenderLocalProducerAuthority *authority = &authorities[kind];

        if (authority->valid && ranges_overlap(
                authority->code_range_start, authority->code_range_size,
                address, size))
            *authority = (XgRenderLocalProducerAuthority){0};
    }
}

void xg_render_local_producer_auth_clear(void) {
    clear_pending();
    memset(authorities, 0, sizeof(authorities));
}

static void reset_state(void) {
    xg_render_local_producer_auth_clear();
    next_generation = 1u;
}

void xg_render_local_producer_auth_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (event->mutation.watched_range_mutation)
            xg_render_local_producer_auth_invalidate(
                event->address, event->size);
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE ||
               event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY ||
               event->kind == XG_RENDER_INVALIDATION_LOADER_MISMATCH) {
        xg_render_local_producer_auth_clear();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        reset_state();
    }
}
