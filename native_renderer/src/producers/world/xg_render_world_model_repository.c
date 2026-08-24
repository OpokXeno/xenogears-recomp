#include "xg_render_world_model_repository.h"

#include "xg_render_model_repository.h"

#include <limits.h>
#include <string.h>

enum {
    XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY = 16384u,
    XG_RENDER_WORLD_MODEL_PACKET_COPY_RANGE_CAPACITY = 64u,
};

enum {
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INVALID_ARGUMENT = 1u,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MISSING,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INACTIVE,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_EPOCH,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_OWNER,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MODEL,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_PACKET_BASE,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_FAMILY,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_WORD_COUNT,
    XG_RENDER_WORLD_MODEL_TEMPLATE_READ_ATTRIBUTE,
};

typedef struct XgRenderWorldModelColorWrite {
    uint64_t resource_epoch;
    CPUState *owner_cpu;
    uint32_t model_address;
    uint32_t packet_address;
    uint32_t address;
    uint32_t value;
    bool used;
    bool valid;
} XgRenderWorldModelColorWrite;

typedef struct XgRenderWorldModelInitializerReceipt {
    uint64_t resource_epoch;
    CPUState *owner_cpu;
    uint32_t model_address;
    uint32_t packet_address;
    uint32_t initializer_function;
    uint8_t primitive_family;
    bool valid;
} XgRenderWorldModelInitializerReceipt;

typedef struct XgRenderWorldModelInitializerContext {
    uint64_t resource_epoch;
    uint64_t authentication_generation;
    CPUState *owner_cpu;
    uint32_t model_address;
    uint32_t packet_base;
    uint32_t packet_capacity;
    uint32_t caller_return;
    uint32_t entry_stack_pointer;
    uint32_t dispatch_mode;
    uint32_t receipt_count;
    bool active;
} XgRenderWorldModelInitializerContext;

typedef struct XgRenderWorldModelPacketCopyContext {
    CPUState *owner_cpu;
    uint32_t destination;
    uint32_t source;
    uint32_t size;
    bool active;
} XgRenderWorldModelPacketCopyContext;

typedef struct XgRenderWorldModelPacketCopyRange {
    uint32_t destination;
    uint32_t source;
    uint32_t size;
} XgRenderWorldModelPacketCopyRange;

static XgRenderWorldModelTemplate templates[
    XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY];
static XgRenderWorldModelColorWrite color_writes[
    XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY];
static XgRenderWorldModelInitializerReceipt initializer_receipts[
    XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY];
static XgRenderWorldModelInitializerContext initializer;
static XgRenderWorldModelPacketCopyContext packet_copy;
static XgRenderWorldModelPacketCopyRange packet_copy_ranges[
    XG_RENDER_WORLD_MODEL_PACKET_COPY_RANGE_CAPACITY];
static uint32_t packet_copy_range_count;
static bool templates_populated;
static bool initializer_populated;
static uint32_t template_read_failure;
static uint64_t resource_epoch;
static uint32_t template_epoch = 1u;
static PsxXgRenderWorldNativeSnapshot snapshot;

static const uint32_t initializer_functions[
    XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
    UINT32_C(0x8002cdcc), UINT32_C(0x8002d814),
    UINT32_C(0x8002d6ac), UINT32_C(0x8002da14),
    UINT32_C(0x8002cf34), UINT32_C(0x8002d984),
    UINT32_C(0x8002d77c), UINT32_C(0x8002da14),
    UINT32_C(0x8002cf58), UINT32_C(0x8002d530),
    UINT32_C(0x8002d180), UINT32_C(0x8002d244),
    UINT32_C(0x8002d0c0), UINT32_C(0x8002d0e4),
    UINT32_C(0x8002d180), UINT32_C(0x8002d244),
    UINT32_C(0x8002dafc),
};

static bool services_valid(
        const XgRenderWorldModelRepositoryServices *services) {
    return services != NULL && services->authentication_generation != NULL &&
        services->authorize_guest_range != NULL &&
        services->stack_address_is_valid != NULL &&
        services->coordinator_in_progress != NULL &&
        services->coordinator_fail != NULL;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & UINT32_C(0x1fffffff)) ==
        (right & UINT32_C(0x1fffffff));
}

static uint32_t normalized_word_address(uint32_t address) {
    return address & UINT32_C(0x001ffffc);
}

static bool ranges_overlap(uint32_t left_address, uint32_t left_size,
                           uint32_t right_address, uint32_t right_size) {
    const uint64_t left = left_address & UINT32_C(0x1fffffff);
    const uint64_t right = right_address & UINT32_C(0x1fffffff);

    return left_size != 0u && right_size != 0u &&
        left < right + right_size && right < left + left_size;
}

static uint32_t template_slot(uint32_t address) {
    return ((address >> 2u) ^ (address >> 13u)) &
        (XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY - 1u);
}

static XgRenderWorldModelTemplate *template_find(
        uint32_t packet_address, bool insert) {
    const uint32_t key = normalized_word_address(packet_address);
    uint32_t slot = template_slot(key);
    XgRenderWorldModelTemplate *first_inactive = NULL;

    for (uint32_t probe = 0u;
         probe < XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY; ++probe) {
        XgRenderWorldModelTemplate *entry = &templates[slot];

        if (entry->table_epoch != template_epoch) return insert ? entry : NULL;
        if (!entry->valid)
            return insert && first_inactive != NULL ? first_inactive :
                (insert ? entry : NULL);
        if (entry->packet_address == key) return entry;
        if (!entry->active && first_inactive == NULL) first_inactive = entry;
        slot = (slot + 1u) &
            (XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY - 1u);
    }
    return insert ? first_inactive : NULL;
}

static uint32_t color_write_slot(uint32_t address) {
    return ((address >> 2u) ^ (address >> 11u)) &
        (XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY - 1u);
}

static XgRenderWorldModelColorWrite *color_write_find(
        uint32_t address, bool insert) {
    const uint32_t key = normalized_word_address(address);
    uint32_t slot = color_write_slot(key);

    for (uint32_t probe = 0u;
         probe < XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY; ++probe) {
        XgRenderWorldModelColorWrite *entry = &color_writes[slot];

        if (!entry->valid || entry->resource_epoch != initializer.resource_epoch)
            return insert ? entry : NULL;
        if (entry->address == key) return entry;
        slot = (slot + 1u) &
            (XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY - 1u);
    }
    return NULL;
}

void xg_render_world_model_repository_invalidate(void) {
    packet_copy = (XgRenderWorldModelPacketCopyContext){0};
    packet_copy_range_count = 0u;
    snapshot.packet_copy_range_count = 0u;
    templates_populated = false;
    if (initializer_populated) {
        initializer = (XgRenderWorldModelInitializerContext){0};
        initializer_populated = false;
    }
    if (template_epoch == UINT32_MAX) {
        memset(templates, 0, sizeof(templates));
        template_epoch = 1u;
    } else {
        ++template_epoch;
    }
    xg_render_model_repository_reset_templates();
}

void xg_render_world_model_repository_invalidate_initializer(void) {
    if (initializer.active) {
        xg_render_world_model_repository_invalidate();
        return;
    }
    if (initializer_populated) {
        initializer = (XgRenderWorldModelInitializerContext){0};
        initializer_populated = false;
    }
}

static bool initializer_caller_is_valid(uint32_t address) {
    return physical_address_equals(address, UINT32_C(0x800211bc)) ||
        physical_address_equals(address, UINT32_C(0x80021258)) ||
        physical_address_equals(address, UINT32_C(0x800212e8)) ||
        physical_address_equals(address, UINT32_C(0x800714c4)) ||
        physical_address_equals(address, UINT32_C(0x80084750));
}

void xg_render_world_model_repository_initializer_begin(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderWorldModelRepositoryServices *services) {
    uint64_t authentication_generation;
    uint32_t packet_capacity;
    uint32_t group_count;

    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL) return;
    if (!services_valid(services)) {
        xg_render_world_model_repository_invalidate();
        return;
    }
    if (services->coordinator_in_progress() || initializer.active ||
        resource_epoch == UINT64_MAX) {
        services->coordinator_fail();
        xg_render_world_model_repository_invalidate();
        return;
    }
    if (cpu == NULL || cpu->read_word == NULL || cpu->read_half == NULL ||
        cpu->read_byte == NULL ||
        !services->stack_address_is_valid(cpu->gpr[29]) ||
        cpu->gpr[29] < 0x30u ||
        !services->authorize_guest_range(
            cpu->gpr[29] - 0x30u, 0x30u, 4u, false) ||
        !services->authorize_guest_range(cpu->gpr[4], 0x38u, 4u, false) ||
        !services->authorize_guest_range(cpu->gpr[5], 4u, 4u, false) ||
        cpu->gpr[6] > 3u || !initializer_caller_is_valid(cpu->gpr[31]) ||
        !services->authentication_generation(&authentication_generation)) {
        xg_render_world_model_repository_invalidate();
        return;
    }
    group_count = cpu->read_half(cpu->gpr[4] + 6u);
    packet_capacity = cpu->read_word(cpu->gpr[4] + 0x34u);
    if ((packet_capacity & 3u) != 0u ||
        (group_count != 0u && packet_capacity < 4u) ||
        (packet_capacity != 0u && !services->authorize_guest_range(
            cpu->gpr[5], packet_capacity, 4u, false))) {
        xg_render_world_model_repository_invalidate();
        return;
    }
    ++resource_epoch;
    for (uint32_t index = 0u;
         index < XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY; ++index) {
        XgRenderWorldModelTemplate *entry = &templates[index];

        if (entry->table_epoch == template_epoch && entry->valid &&
            ranges_overlap(entry->packet_address,
                           (uint32_t)entry->word_count * 4u,
                           cpu->gpr[5], packet_capacity))
            entry->active = false;
    }
    initializer = (XgRenderWorldModelInitializerContext){
        .resource_epoch = resource_epoch,
        .authentication_generation = authentication_generation,
        .owner_cpu = cpu,
        .model_address = cpu->gpr[4],
        .packet_base = cpu->gpr[5],
        .packet_capacity = packet_capacity,
        .caller_return = cpu->gpr[31],
        .entry_stack_pointer = cpu->gpr[29],
        .dispatch_mode = cpu->gpr[6],
        .active = true,
    };
    initializer_populated = true;
}

static bool record_color_write(
        CPUState *cpu, uint32_t address, uint32_t value,
        const XgRenderWorldModelRepositoryServices *services) {
    XgRenderWorldModelColorWrite *entry;
    uint32_t packet;

    if (!initializer.active || initializer.owner_cpu != cpu ||
        initializer.packet_capacity < 4u ||
        !services->authorize_guest_range(address, 4u, 4u, false) ||
        address < initializer.packet_base ||
        address - initializer.packet_base > initializer.packet_capacity - 4u)
        return false;
    packet = cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL);
    if (packet < initializer.packet_base ||
        packet - initializer.packet_base >= initializer.packet_capacity ||
        address < packet)
        return false;
    entry = color_write_find(address, true);
    if (entry == NULL) return false;
    *entry = (XgRenderWorldModelColorWrite){
        .resource_epoch = initializer.resource_epoch,
        .owner_cpu = cpu,
        .model_address = normalized_word_address(initializer.model_address),
        .packet_address = normalized_word_address(packet),
        .address = normalized_word_address(address),
        .value = value,
        .valid = true,
    };
    return true;
}

void xg_render_world_model_repository_observe_color_writes(
        CPUState *cpu, uint32_t pc,
        const XgRenderWorldModelRepositoryServices *services) {
    bool ok = true;

    if (!initializer.active) return;
    if (!services_valid(services) || cpu == NULL || cpu->read_word == NULL ||
        initializer.owner_cpu != cpu || services->coordinator_in_progress()) {
        if (services_valid(services)) services->coordinator_fail();
        xg_render_world_model_repository_invalidate();
        return;
    }
    if (physical_address_equals(pc, UINT32_C(0x8004a1ac))) {
        ok = record_color_write(
            cpu, cpu->gpr[5], cpu->read_word(cpu->gpr[5]), services);
    } else if (physical_address_equals(pc, UINT32_C(0x8004a1e8))) {
        ok = record_color_write(
                 cpu, cpu->gpr[7], cpu->read_word(cpu->gpr[7]), services) &&
            record_color_write(
                 cpu, cpu->gpr[8], cpu->read_word(cpu->gpr[8]), services) &&
            record_color_write(
                 cpu, cpu->gpr[9], cpu->read_word(cpu->gpr[9]), services);
    } else if (physical_address_equals(pc, UINT32_C(0x8004a274))) {
        ok = record_color_write(
            cpu, cpu->gpr[6], cpu->read_word(cpu->gpr[6]), services);
    } else if (physical_address_equals(pc, UINT32_C(0x8004a2b8))) {
        ok = record_color_write(
                 cpu, cpu->gpr[8], cpu->read_word(cpu->gpr[8]), services) &&
            record_color_write(
                 cpu, cpu->gpr[9], cpu->read_word(cpu->gpr[9]), services) &&
            record_color_write(
                 cpu, cpu->gpr[10], cpu->read_word(cpu->gpr[10]), services);
    }
    if (!ok) xg_render_world_model_repository_invalidate();
}

void xg_render_world_model_repository_observe_initializer_success(
        CPUState *cpu) {
    enum {
        INITIALIZER_TABLE_BASE = 0x8004fe50u,
        INITIALIZER_TABLE_STRIDE = 0x28u,
    };
    XgRenderWorldModelInitializerReceipt *receipt;
    uint32_t family;
    uint32_t packet;

    if (!initializer.active) return;
    if (cpu == NULL || cpu->read_word == NULL || initializer.owner_cpu != cpu ||
        cpu->gpr[2] == 0u || cpu->gpr[17] < INITIALIZER_TABLE_BASE ||
        (cpu->gpr[17] - INITIALIZER_TABLE_BASE) %
                INITIALIZER_TABLE_STRIDE != 0u) {
        xg_render_world_model_repository_invalidate();
        return;
    }
    family = (cpu->gpr[17] - INITIALIZER_TABLE_BASE) /
        INITIALIZER_TABLE_STRIDE;
    packet = cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL);
    if (family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT ||
        !physical_address_equals(cpu->gpr[19], initializer_functions[family]) ||
        initializer.receipt_count >= XG_RENDER_WORLD_MODEL_TEMPLATE_CAPACITY ||
        packet < initializer.packet_base ||
        packet - initializer.packet_base >= initializer.packet_capacity) {
        xg_render_world_model_repository_invalidate();
        return;
    }
    receipt = &initializer_receipts[initializer.receipt_count++];
    *receipt = (XgRenderWorldModelInitializerReceipt){
        .resource_epoch = initializer.resource_epoch,
        .owner_cpu = cpu,
        .model_address = normalized_word_address(initializer.model_address),
        .packet_address = normalized_word_address(packet),
        .initializer_function = cpu->gpr[19],
        .primitive_family = (uint8_t)family,
        .valid = true,
    };
}

static uint32_t expected_color_word_mask(
        uint32_t family, uint32_t dispatch_mode) {
    switch (family) {
    case 0u:
    case 1u:
    case 8u:
    case 9u:
        return dispatch_mode == 0u ? 0u : UINT32_C(1) << 1u;
    case 2u:
    case 6u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 3u) |
            (UINT32_C(1) << 5u);
    case 3u:
    case 7u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 4u) |
            (UINT32_C(1) << 7u);
    case 10u:
    case 14u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 3u) |
            (UINT32_C(1) << 5u) | (UINT32_C(1) << 7u);
    case 11u:
    case 15u:
        return (UINT32_C(1) << 1u) | (UINT32_C(1) << 4u) |
            (UINT32_C(1) << 7u) | (UINT32_C(1) << 10u);
    default:
        return 0u;
    }
}

static bool seed_templates(
        CPUState *cpu,
        const XgRenderWorldModelRepositoryServices *services) {
    static const uint8_t attribute_sizes[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
        12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
    };
    static const uint8_t packet_strides[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        0x14u, 0x20u, 0x1cu, 0x28u, 0x14u, 0x20u, 0x1cu, 0x28u,
        0x18u, 0x28u, 0x24u, 0x34u, 0x18u, 0x28u, 0x24u, 0x34u,
        0x20u,
    };
    const XgRenderWorldModelInitializerContext context = initializer;
    uint32_t topology;
    uint32_t attribute;
    uint32_t packet = context.packet_base;
    uint32_t captured_total = 0u;
    uint64_t authentication_generation;

    if (!services_valid(services) || cpu == NULL || cpu->read_word == NULL ||
        cpu->read_half == NULL || cpu->read_byte == NULL || !context.active ||
        context.resource_epoch == 0u || context.owner_cpu != cpu ||
        context.authentication_generation == 0u ||
        !services->authentication_generation(&authentication_generation) ||
        authentication_generation != context.authentication_generation ||
        cpu->gpr[29] != context.entry_stack_pointer ||
        !physical_address_equals(cpu->gpr[31], context.caller_return))
        return false;
    topology = cpu->read_word(context.model_address + 0x10u);
    attribute = cpu->read_word(context.model_address + 0x14u);
    const uint32_t group_count = cpu->read_half(context.model_address + 6u);
    if (group_count != 0u &&
        (!services->authorize_guest_range(topology, 4u, 2u, false) ||
         !services->authorize_guest_range(attribute, 4u, 4u, false)))
        return false;

    for (uint32_t group = 0u; group < group_count; ++group) {
        const uint32_t family = cpu->read_byte(topology);
        const uint32_t count = cpu->read_half(topology + 2u);
        const uint32_t table = UINT32_C(0x8004fe50) + family * 0x28u;

        if (family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT || count == 0u ||
            count > (UINT32_MAX - 4u) / 8u ||
            !services->authorize_guest_range(
                topology, 4u + count * 8u, 2u, false) ||
            !physical_address_equals(
                cpu->read_word(table + 0x18u), initializer_functions[family]) ||
            cpu->read_word(table + 0x1cu) != 8u ||
            cpu->read_word(table + 0x20u) != attribute_sizes[family] ||
            cpu->read_word(table + 0x24u) != packet_strides[family])
            return false;
        for (uint32_t primitive = 0u; primitive < count; ++primitive) {
            XgRenderWorldModelTemplate captured = {0};
            const XgRenderWorldModelInitializerReceipt *receipt;
            uint32_t controls;

            for (controls = 0u;
                 controls < XG_WORLD_MODELS_MAX_CONTROL_COMMANDS_PER_PRIMITIVE;
                 ++controls) {
                const uint8_t command = cpu->read_byte(attribute + 3u);

                if (command != 0xc4u && command != 0xc8u) break;
                if (attribute > UINT32_MAX - 4u) return false;
                attribute += 4u;
            }
            if (controls == XG_WORLD_MODELS_MAX_CONTROL_COMMANDS_PER_PRIMITIVE ||
                packet > UINT32_MAX - packet_strides[family] ||
                attribute > UINT32_MAX - attribute_sizes[family] ||
                packet < context.packet_base ||
                packet - context.packet_base > context.packet_capacity ||
                packet_strides[family] > context.packet_capacity -
                    (packet - context.packet_base) ||
                !services->authorize_guest_range(
                    packet, packet_strides[family], 4u, false) ||
                !services->authorize_guest_range(
                    attribute, attribute_sizes[family], 4u, false) ||
                captured_total >= context.receipt_count)
                return false;
            receipt = &initializer_receipts[captured_total];
            if (!receipt->valid ||
                receipt->resource_epoch != context.resource_epoch ||
                receipt->owner_cpu != cpu ||
                receipt->model_address !=
                    normalized_word_address(context.model_address) ||
                receipt->packet_address != normalized_word_address(packet) ||
                receipt->primitive_family != family ||
                !physical_address_equals(
                    receipt->initializer_function, initializer_functions[family]))
                return false;
            captured.packet_address = normalized_word_address(packet);
            captured.resource_epoch = context.resource_epoch;
            captured.table_epoch = template_epoch;
            captured.owner_cpu = cpu;
            captured.model_address = normalized_word_address(context.model_address);
            captured.packet_base = normalized_word_address(context.packet_base);
            captured.primitive_ordinal = captured_total;
            captured.attribute_address = normalized_word_address(attribute);
            captured.primitive_family = (uint8_t)family;
            captured.word_count = packet_strides[family] / 4u;
            const uint32_t expected_tag_payload_word_count =
                captured.word_count - (family == 8u ? 2u : 1u);
            const uint32_t expected_color_mask = expected_color_word_mask(
                family, context.dispatch_mode);
            for (uint32_t word = 0u; word < captured.word_count; ++word) {
                XgRenderWorldModelColorWrite *color = color_write_find(
                    packet + word * 4u, false);
                const bool color_expected =
                    (expected_color_mask & (UINT32_C(1) << word)) != 0u;

                captured.words[word] = cpu->read_word(packet + word * 4u);
                if (color_expected != (color != NULL)) return false;
                if (color_expected) {
                    if (color->resource_epoch != context.resource_epoch ||
                        color->owner_cpu != cpu ||
                        color->model_address != captured.model_address ||
                        color->packet_address != captured.packet_address ||
                        (captured.words[word] & UINT32_C(0x00ffffff)) !=
                            (color->value & UINT32_C(0x00ffffff)))
                        return false;
                    color->used = true;
                }
            }
            if ((captured.words[0] >> 24u) != expected_tag_payload_word_count)
                return false;
            XgRenderWorldModelTemplate *entry = template_find(packet, true);
            if (entry == NULL) return false;
            captured.active = true;
            captured.valid = true;
            *entry = captured;
            templates_populated = true;
            ++captured_total;
            packet += packet_strides[family];
            attribute += attribute_sizes[family];
        }
        topology += 4u + count * 8u;
    }
    if (captured_total != context.receipt_count ||
        cpu->read_word(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL) != packet)
        return false;
    for (uint32_t index = 0u;
         index < XG_RENDER_WORLD_MODEL_COLOR_WRITE_CAPACITY; ++index) {
        if (color_writes[index].valid &&
            color_writes[index].resource_epoch == context.resource_epoch &&
            !color_writes[index].used)
            return false;
    }
    return true;
}

void xg_render_world_model_repository_initializer_finish(
        CPUState *cpu,
        const XgRenderWorldModelRepositoryServices *services) {
    if (!initializer.active) return;
    if (!seed_templates(cpu, services)) {
        xg_render_world_model_repository_invalidate();
        return;
    }
    initializer = (XgRenderWorldModelInitializerContext){0};
    initializer_populated = false;
}

void xg_render_world_model_repository_begin_packet_copy(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderWorldModelRepositoryServices *services) {
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL) return;
    if (cpu != NULL &&
        !physical_address_equals(cpu->gpr[31], UINT32_C(0x80084778)))
        return;
    if (snapshot.packet_copy_begin_count != UINT64_MAX)
        ++snapshot.packet_copy_begin_count;
    snapshot.packet_copy_failure_detail = 1u;
    if (!services_valid(services) || cpu == NULL || cpu->read_word == NULL ||
        initializer.active || packet_copy.active)
        goto fail;
    const uint32_t destination = cpu->gpr[4];
    const uint32_t source = cpu->gpr[5];
    const uint32_t size = cpu->gpr[6];
    snapshot.packet_copy_last_destination = destination;
    snapshot.packet_copy_last_source = source;
    snapshot.packet_copy_last_size = size;
    snapshot.packet_copy_failure_detail = 2u;
    if (size == 0u || destination > UINT32_MAX - size ||
        source > UINT32_MAX - size ||
        !services->authorize_guest_range(source, size, 4u, false) ||
        !services->authorize_guest_range(destination, size, 4u, false))
        goto fail;
    packet_copy = (XgRenderWorldModelPacketCopyContext){
        .owner_cpu = cpu,
        .destination = destination,
        .source = source,
        .size = size,
        .active = true,
    };
    snapshot.packet_copy_failure_detail = 0u;
    return;

fail:
    xg_render_world_model_repository_invalidate();
}

void xg_render_world_model_repository_finish_packet_copy(
        CPUState *cpu, GuestRenderRenderMode render_mode,
        const XgRenderWorldModelRepositoryServices *services) {
    if (!packet_copy.active) return;
    const XgRenderWorldModelPacketCopyContext copy = packet_copy;
    packet_copy = (XgRenderWorldModelPacketCopyContext){0};
    if (render_mode == GUEST_RENDER_RENDER_ORIGINAL) return;
    if (snapshot.packet_copy_finish_count != UINT64_MAX)
        ++snapshot.packet_copy_finish_count;
    snapshot.packet_copy_failure_detail = 3u;
    if (!services_valid(services) || cpu == NULL || cpu != copy.owner_cpu ||
        cpu->read_word == NULL || initializer.active || cpu->gpr[6] != 0u)
        goto fail;
    const uint32_t destination = copy.destination;
    const uint32_t source = copy.source;
    const uint32_t size = copy.size;
    const uint32_t destination_end = destination + size;
    const uint32_t source_end = source + size;
    snapshot.packet_copy_failure_detail = 4u;
    if (cpu->gpr[2] != destination || cpu->gpr[4] != destination_end ||
        cpu->gpr[5] != source_end)
        goto fail;
    snapshot.packet_copy_failure_detail = 5u;
    if (destination != source_end ||
        !services->authorize_guest_range(source, size, 4u, false) ||
        !services->authorize_guest_range(destination, size, 4u, false))
        goto fail;

    uint32_t cursor = source;
    uint32_t primitive_ordinal = 0u;
    while (cursor < source_end) {
        const XgRenderWorldModelTemplate *entry = template_find(cursor, false);
        const uint32_t packet_size = entry != NULL
            ? (uint32_t)entry->word_count * 4u : 0u;

        snapshot.packet_copy_failure_detail = entry == NULL ? 6u : 7u;
        if (entry == NULL || !entry->active || entry->resource_epoch == 0u ||
            entry->owner_cpu != cpu ||
            entry->packet_base != normalized_word_address(source) ||
            entry->packet_address != normalized_word_address(cursor) ||
            entry->primitive_ordinal != primitive_ordinal ||
            packet_size == 0u || packet_size > source_end - cursor)
            goto fail;
        snapshot.packet_copy_failure_detail = 8u;
        for (uint32_t word = 0u; word < entry->word_count; ++word) {
            const uint32_t source_word = cpu->read_word(cursor + word * 4u);

            if (source_word != entry->words[word] ||
                cpu->read_word(destination + (cursor - source) + word * 4u) !=
                    source_word)
                goto fail;
        }
        cursor += packet_size;
        ++primitive_ordinal;
    }

    cursor = source;
    snapshot.packet_copy_failure_detail = 9u;
    while (cursor < source_end) {
        const XgRenderWorldModelTemplate *source_entry =
            template_find(cursor, false);
        XgRenderWorldModelTemplate *destination_entry = template_find(
            destination + (cursor - source), true);

        if (source_entry == NULL || destination_entry == NULL) goto fail;
        XgRenderWorldModelTemplate cloned = *source_entry;
        cloned.packet_base = normalized_word_address(destination);
        cloned.packet_address = normalized_word_address(
            destination + (cursor - source));
        *destination_entry = cloned;
        if (snapshot.packet_copy_template_count != UINT64_MAX)
            ++snapshot.packet_copy_template_count;
        cursor += (uint32_t)cloned.word_count * 4u;
    }
    if (packet_copy_range_count <
            XG_RENDER_WORLD_MODEL_PACKET_COPY_RANGE_CAPACITY) {
        packet_copy_ranges[packet_copy_range_count++] =
            (XgRenderWorldModelPacketCopyRange){
                .destination = destination,
                .source = source,
                .size = size,
            };
        snapshot.packet_copy_range_count = packet_copy_range_count;
    }
    snapshot.packet_copy_failure_detail = 0u;
    return;

fail:
    xg_render_world_model_repository_invalidate();
}

bool xg_render_world_model_repository_read_packet_template(
        void *context, uint32_t model_header_address,
        uint32_t packet_base_address, uint32_t packet_address,
        uint32_t attribute_address, uint8_t primitive_family,
        uint32_t *out_words, uint8_t word_count,
        uint64_t *out_resource_epoch) {
    const XgRenderWorldModelRepositoryReaderContext *reader = context;
    XgRenderWorldModelTemplate *entry;

    if (reader == NULL || out_words == NULL || out_resource_epoch == NULL ||
        primitive_family >= XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT ||
        word_count == 0u ||
        word_count > XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT) {
        template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INVALID_ARGUMENT;
        return false;
    }
    entry = template_find(packet_address, false);
    if (entry == NULL) {
        if (snapshot.first_missing_packet_address == 0u) {
            snapshot.first_missing_model_address = model_header_address;
            snapshot.first_missing_packet_base = packet_base_address;
            snapshot.first_missing_packet_address = packet_address;
            for (uint32_t index = 0u; index < packet_copy_range_count; ++index) {
                const XgRenderWorldModelPacketCopyRange *range =
                    &packet_copy_ranges[index];

                if (packet_address >= range->source &&
                    packet_address - range->source < range->size) {
                    snapshot.first_missing_copy_range_kind = 1u;
                    snapshot.first_missing_copy_range_index = index;
                    break;
                }
                if (packet_address >= range->destination &&
                    packet_address - range->destination < range->size) {
                    snapshot.first_missing_copy_range_kind = 2u;
                    snapshot.first_missing_copy_range_index = index;
                    break;
                }
            }
        }
        template_read_failure = XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MISSING;
        return false;
    }
    if (!entry->active) {
        template_read_failure = XG_RENDER_WORLD_MODEL_TEMPLATE_READ_INACTIVE;
        return false;
    }
    if (entry->resource_epoch == 0u) {
        template_read_failure = XG_RENDER_WORLD_MODEL_TEMPLATE_READ_EPOCH;
        return false;
    }
    if (entry->owner_cpu != reader->cpu) {
        template_read_failure = XG_RENDER_WORLD_MODEL_TEMPLATE_READ_OWNER;
        return false;
    }
    if (entry->model_address != normalized_word_address(model_header_address)) {
        template_read_failure = XG_RENDER_WORLD_MODEL_TEMPLATE_READ_MODEL;
        return false;
    }
    if (entry->packet_base != normalized_word_address(packet_base_address)) {
        template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_PACKET_BASE;
        return false;
    }
    if (entry->primitive_family != primitive_family) {
        template_read_failure = XG_RENDER_WORLD_MODEL_TEMPLATE_READ_FAMILY;
        return false;
    }
    if (entry->word_count != word_count) {
        template_read_failure =
            XG_RENDER_WORLD_MODEL_TEMPLATE_READ_WORD_COUNT;
        return false;
    }
    if (entry->attribute_address != normalized_word_address(attribute_address)) {
        template_read_failure = XG_RENDER_WORLD_MODEL_TEMPLATE_READ_ATTRIBUTE;
        return false;
    }
    memcpy(out_words, entry->words, word_count * sizeof(out_words[0]));
    *out_resource_epoch = entry->resource_epoch;
    return true;
}

uint32_t xg_render_world_model_repository_template_read_failure(void) {
    return template_read_failure;
}

void xg_render_world_model_repository_clear_template_read_failure(void) {
    template_read_failure = 0u;
}

const XgRenderWorldModelTemplate *
xg_render_world_model_repository_find_template(
        uint32_t packet_address) {
    return template_find(packet_address, false);
}

bool xg_render_world_model_repository_update_template_words(
        uint32_t packet_address, CPUState *owner_cpu, uint64_t expected_epoch,
        const uint32_t *words, uint8_t word_count,
        uint32_t word_write_mask) {
    XgRenderWorldModelTemplate *entry;
    uint32_t allowed_mask;

    if (owner_cpu == NULL || words == NULL || expected_epoch == 0u ||
        word_count == 0u ||
        word_count > XG_WORLD_MODELS_MAX_PACKET_WORD_COUNT)
        return false;
    allowed_mask = word_count >= 32u
        ? UINT32_MAX : (UINT32_C(1) << word_count) - 1u;
    if ((word_write_mask & ~allowed_mask) != 0u) return false;
    entry = template_find(packet_address, false);
    if (entry == NULL || entry->table_epoch != template_epoch ||
        !entry->valid || !entry->active || entry->owner_cpu != owner_cpu ||
        entry->resource_epoch != expected_epoch ||
        entry->packet_address != normalized_word_address(packet_address) ||
        entry->word_count != word_count)
        return false;
    for (uint32_t word = 0u; word < word_count; ++word) {
        if ((word_write_mask & (UINT32_C(1) << word)) != 0u)
            entry->words[word] = words[word];
    }
    return true;
}

void xg_render_world_model_repository_snapshot(
        PsxXgRenderWorldNativeSnapshot *out_snapshot) {
    if (out_snapshot != NULL) *out_snapshot = snapshot;
}

void xg_render_world_model_repository_reset(void) {
    snapshot = (PsxXgRenderWorldNativeSnapshot){0};
    xg_render_world_model_repository_invalidate();
}

void xg_render_world_model_repository_handle_invalidation(
        const XgRenderInvalidationEvent *event,
        const XgRenderInvalidationServices *services) {
    const bool model_write = xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_MODEL_FT4) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA);
    const bool semantic_write = event->mutation.semantic_authority_loss ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_SKY) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS) ||
        xg_render_invalidation_has_code_class(
            event, PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);

    (void)services;
    if (event->kind == XG_RENDER_INVALIDATION_CODE_WRITE) {
        if (model_write)
            xg_render_world_model_repository_invalidate();
        if (semantic_write)
            xg_render_world_model_repository_invalidate_initializer();
    } else if (event->kind == XG_RENDER_INVALIDATION_DISABLE) {
        xg_render_world_model_repository_invalidate_initializer();
        xg_render_world_model_repository_invalidate();
    } else if (event->kind == XG_RENDER_INVALIDATION_SCENE_BOUNDARY) {
        xg_render_world_model_repository_invalidate_initializer();
    } else if (event->kind == XG_RENDER_INVALIDATION_RESET) {
        xg_render_world_model_repository_reset();
    }
}
