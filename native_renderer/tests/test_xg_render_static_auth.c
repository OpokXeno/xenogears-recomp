#include "game_identity.h"
#include "guest_render_bridge.h"
#include "guest_render_native_stream.h"
#include "guest_render_transaction.h"
#include "cpu_state.h"
#include "gpu.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_auth.h"
#include "xg_render_auth_runtime.h"
#include "xg_host_3d.h"
#include "xg_model_ft4_raw.h"
#include "xg_sprite_ft4.h"
#include "xg_world_actor_sprites_source_capture.h"
#include "xg_world_decorations_source_capture.h"
#include "xg_world_effects.h"
#include "xg_world_entity_shadows_source_capture.h"
#include "xg_world_horizon.h"
#include "xg_world_minimap.h"
#include "xg_world_models_native.h"
#include "xg_world_terrain_water_source_capture.h"
#include "xg_render_runtime_variants_generated.h"
#include "xg_render_runtime_variant_auth.h"
#include "xg_render_static_auth.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) return 0; } while (0)

#define PRODUCER_ENTRY UINT32_C(0x80075b44)
#define CALLER_SITE UINT32_C(0x800781bc)
#define STATIC_CALLEE UINT32_C(0x8004b54c)
#define RETURN_SITE UINT32_C(0x800781c4)
#define JAL_INSTRUCTION UINT32_C(0x0c012d53)
#define FIELD_RANGE_SIZE UINT32_C(282624)
#define FIELD5_ACTIVATION_SITE UINT32_C(0x80075414)
#define FIELD5_PRODUCER_ENTRY UINT32_C(0x800764b4)
#define FIELD5_CAPTURE_SITE UINT32_C(0x80075694)
#define FIELD5_RETURN_SITE UINT32_C(0x8007569c)
#define FIELD5_ACTIVATION_JAL UINT32_C(0x0c01d92d)
#define FIELD5_ACTIVATION_DELAY UINT32_C(0x248400cc)
#define FIELD5_CAPTURE_DELAY UINT32_C(0x34040001)
#define FIELD5_CAPTURE_NON_JAL UINT32_C(0x08012d53)
#define FIELD5_CAPTURE_WRONG_TARGET_JAL UINT32_C(0x0c0112f4)
#define FIELD5_ACTIVATION_RETURN UINT32_C(0x8007541c)
#define FIELD5_CALLER_RETURN UINT32_C(0x80075614)
#define FIELD5_INTERNAL_CAPTURE UINT32_C(0x800764c0)
#define FIELD5_INTERNAL_ENTRY UINT32_C(0x800764d0)
#define FIELD5_INTERNAL_RETURN UINT32_C(0x800764dc)
#define FIELD5_PARTICLE_INITIALIZER UINT32_C(0x800a8eac)
#define FIELD5_PARTICLE_RENDER UINT32_C(0x800a9b54)
#define FIELD5_ZOOM_RGB UINT32_C(0x800a5600)
#define FIELD5_ZOOM_RENDER UINT32_C(0x800a6408)
#define FIELD5_ZOOM_INITIALIZER UINT32_C(0x800a663c)
#define UI_DRAW_OT_SITE UINT32_C(0x800759cc)
#define UI_DRAW_OT_JAL UINT32_C(0x0c0112f4)
#define KUSEG_ADDRESS(address) ((address) & UINT32_C(0x1fffffff))

static uint32_t test_hook_return_address(uint32_t hook, uint32_t pc) {
    if (hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE) return pc + 8u;
    if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY &&
        (pc & UINT32_C(0x1fffffff)) ==
            (FIELD5_PRODUCER_ENTRY & UINT32_C(0x1fffffff)))
        return FIELD5_ACTIVATION_RETURN;
    return pc;
}

static void test_cold_hook(uint32_t hook, uint32_t pc,
                           uint32_t instruction_word,
                           uint32_t delay_slot_word) {
    CPUState cpu = {0};

    cpu.gpr[31] = test_hook_return_address(hook, pc);
    psx_xg_render_auth_cold_hook(&cpu, hook, pc, instruction_word,
                                 delay_slot_word);
}

static void test_cold_hook_with_cpu(CPUState *cpu, uint32_t hook,
                                    uint32_t pc, uint32_t instruction_word,
                                    uint32_t delay_slot_word) {
    psx_xg_render_auth_cold_hook(cpu, hook, pc, instruction_word,
                                 delay_slot_word);
}

static void test_warm_hook(CPUState *cpu, uint32_t hook, uint32_t pc,
                           uint32_t instruction_word,
                           uint32_t delay_slot_word) {
    CPUState synthetic_cpu = {0};

    if (cpu == NULL) {
        synthetic_cpu.gpr[31] = test_hook_return_address(hook, pc);
        cpu = &synthetic_cpu;
    }
    psx_xg_render_auth_warm_hook(cpu, hook, pc, instruction_word,
                                 delay_slot_word);
}

#define psx_xg_render_auth_cold_hook test_cold_hook
#define psx_xg_render_auth_warm_hook test_warm_hook

static int runtime_has_no_authenticated_observations(void);
static void complete_static_auth_trace(void);
static int reset_source_mode(GuestRenderRenderMode mode);
static void configure_projected_memory(void);

const uint8_t xg_render_game_identity[XG_RENDER_MANIFEST_DIGEST_SIZE] = {
    0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
    0x18u, 0x19u, 0x1au, 0x1bu, 0x1cu, 0x1du, 0x1eu, 0x1fu,
    0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u,
    0x28u, 0x29u, 0x2au, 0x2bu, 0x2cu, 0x2du, 0x2eu, 0x2fu,
};
const uint8_t xg_render_manifest_identity[XG_RENDER_MANIFEST_DIGEST_SIZE] = {
    0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, 0x37u,
    0x38u, 0x39u, 0x3au, 0x3bu, 0x3cu, 0x3du, 0x3eu, 0x3fu,
    0x40u, 0x41u, 0x42u, 0x43u, 0x44u, 0x45u, 0x46u, 0x47u,
    0x48u, 0x49u, 0x4au, 0x4bu, 0x4cu, 0x4du, 0x4eu, 0x4fu,
};
const uint32_t xg_render_namespace_crc32 = 0x25adc86eu;
const XgRenderManifestValidation xg_render_manifest_validation = {
    3u, 4u, 0x11223344u, 0x55667788u, 0x8006f000u, FIELD_RANGE_SIZE,
    PRODUCER_ENTRY, CALLER_SITE, STATIC_CALLEE, RETURN_SITE,
    0x800781b4u, 16u,
    {0x50u, 0x51u, 0x52u, 0x53u, 0x54u, 0x55u, 0x56u, 0x57u,
     0x58u, 0x59u, 0x5au, 0x5bu, 0x5cu, 0x5du, 0x5eu, 0x5fu,
     0x60u, 0x61u, 0x62u, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u,
     0x68u, 0x69u, 0x6au, 0x6bu, 0x6cu, 0x6du, 0x6eu, 0x6fu},
    3u, STATIC_CALLEE, 1u, 1u,
};
const XgRenderManifestRecord xg_render_manifest_records[] = {
    {3u, "producer", 3u, PRODUCER_ENTRY, 0u,
     {0x70u, 0x71u, 0x72u, 0x73u, 0x74u, 0x75u, 0x76u, 0x77u,
      0x78u, 0x79u, 0x7au, 0x7bu, 0x7cu, 0x7du, 0x7eu, 0x7fu,
      0x80u, 0x81u, 0x82u, 0x83u, 0x84u, 0x85u, 0x86u, 0x87u,
      0x88u, 0x89u, 0x8au, 0x8bu, 0x8cu, 0x8du, 0x8eu, 0x8fu},
     {0}, "field-double-buffer", "field-ot"},
    {4u, "site", 4u, CALLER_SITE, STATIC_CALLEE,
     {0},
     {0x50u, 0x51u, 0x52u, 0x53u, 0x54u, 0x55u, 0x56u, 0x57u,
      0x58u, 0x59u, 0x5au, 0x5bu, 0x5cu, 0x5du, 0x5eu, 0x5fu,
      0x60u, 0x61u, 0x62u, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u,
      0x68u, 0x69u, 0x6au, 0x6bu, 0x6cu, 0x6du, 0x6eu, 0x6fu},
     "field-double-buffer", "field-ot"},
};
const uint32_t xg_render_manifest_record_count = 2u;

static PsxGameIdentity runtime_identity;
static int identity_enabled;
static int identity_bound;

static uint32_t producer_family_packet_address;
static uint32_t producer_family_model_address;
static uint32_t producer_family_actor_index;
static uint32_t producer_family_stack_base;
static uint32_t producer_family_packet_tag;
static uint32_t producer_family_ot_bucket_address;
static uint32_t producer_family_ot_word;
static uint32_t producer_family_packet_words[9];
static XgHost3dRotAverage4Input producer_family_host_input;
static XgHost3dRotAverage4Output producer_family_host_output;
static uint32_t producer_family_xy(int16_t x, int16_t y);

enum {
    PRODUCER_FAMILY_ACTOR_BASE = 0x80012000u,
    PRODUCER_FAMILY_SECONDARY = 0x80013000u,
    PRODUCER_FAMILY_STATE = 0x80014000u,
    PRODUCER_FAMILY_OT_BASE = 0x80018000u,
};
static bool presentation_gate_succeeds = true;
static bool presentation_gate_saw_closed_state;

static bool test_presentation_gate(
    GuestRenderRenderMode requested_mode,
    NativeRenderPresentationSnapshot *out_snapshot,
    void *user_data) {
    GuestRenderBridgeSnapshot bridge = {0};

    (void)user_data;
    presentation_gate_saw_closed_state =
        guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK &&
        !bridge.state_open && !bridge.producer_open;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->interpolation_requested = true;
    out_snapshot->smooth_requested = true;
    out_snapshot->quiesced =
        requested_mode != GUEST_RENDER_RENDER_ORIGINAL;
    out_snapshot->interpolation_effective =
        requested_mode == GUEST_RENDER_RENDER_ORIGINAL;
    out_snapshot->smooth_effective =
        requested_mode == GUEST_RENDER_RENDER_ORIGINAL;
    out_snapshot->reason = presentation_gate_succeeds
        ? NATIVE_RENDER_PRESENTATION_GATE_NONE
        : NATIVE_RENDER_PRESENTATION_GATE_OPENGL_REQUIRED;
    return presentation_gate_succeeds;
}

GpuRenderTransactionStatus gr_transaction_begin(
    GpuRenderTransactionId transaction_id,
    uint64_t vram_mutation_serial) {
    (void)transaction_id;
    (void)vram_mutation_serial;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_ordering_barrier(
    GpuRenderTransactionId transaction_id) {
    (void)transaction_id;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_draw_semantic(
    GpuRenderTransactionId transaction_id,
    const GpuRenderSemantic *semantic) {
    (void)transaction_id;
    (void)semantic;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_commit_validate(
    GpuRenderTransactionId transaction_id,
    uint64_t current_vram_mutation_serial,
    const GpuRenderPresent *present) {
    (void)transaction_id;
    (void)current_vram_mutation_serial;
    (void)present;
    return GPU_RENDER_TRANSACTION_READY;
}

GpuRenderTransactionStatus gr_rollback(
        GpuRenderTransactionId transaction_id) {
    (void)transaction_id;
    return GPU_RENDER_TRANSACTION_OK;
}

GpuRenderTransactionStatus gr_deferred_candidate_capture(
        GpuRenderTransactionId transaction_id,
        GpuRenderDeferredCandidateToken *out_candidate_token) {
    (void)transaction_id;
    if (out_candidate_token != NULL)
        *out_candidate_token = GPU_RENDER_DEFERRED_CANDIDATE_NONE;
    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
}

GpuRenderTransactionStatus gr_deferred_candidate_discard(
        GpuRenderDeferredCandidateToken candidate_token) {
    (void)candidate_token;
    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
}

GpuRenderTransactionStatus gr_deferred_transaction_begin(
        GpuRenderTransactionId transaction_id,
        uint64_t vram_mutation_serial,
        GpuRenderDeferredCandidateToken candidate_token) {
    (void)transaction_id;
    (void)vram_mutation_serial;
    (void)candidate_token;
    return GPU_RENDER_TRANSACTION_UNSUPPORTED;
}

static uint16_t producer_family_read_half(uint32_t address) {
    if (address == PRODUCER_FAMILY_SECONDARY + 0x84u) return 0u;
    if (address == PRODUCER_FAMILY_STATE + 0xf4u ||
        address == PRODUCER_FAMILY_STATE + 0xf6u ||
        address == PRODUCER_FAMILY_STATE + 0xf8u)
        return 0x1000u;
    if (address >= 0x800add70u && address < 0x800addb8u) {
        static const uint16_t u[4] = {0u, 16u, 0u, 16u};
        return u[(address & 7u) / 2u];
    }
    if (address >= 0x800addb8u && address < 0x800ade00u) {
        static const uint16_t v[4] = {0u, 0u, 32u, 32u};
        return v[(address & 7u) / 2u];
    }
    if (address >= 0x800ade00u && address < 0x800ade18u) {
        static const uint16_t material[6] = {0u, 0u, 0u, 0u, 32u, 7u};
        return material[((address - 0x800ade00u) % 12u) / 2u];
    }
    return 0xffffu;
}

static uint32_t producer_family_read_word(uint32_t address) {
    const uint32_t stack_base = producer_family_stack_base;
    const uint32_t actor_address = PRODUCER_FAMILY_ACTOR_BASE +
        producer_family_actor_index * XG_FIELD_CHARACTER_SOURCE_ACTOR_STRIDE;

    if (address == producer_family_packet_address)
        return producer_family_packet_tag;
    if (address == producer_family_ot_bucket_address)
        return producer_family_ot_word;
    if (address >= producer_family_model_address &&
        address < producer_family_model_address + 32u) {
        const uint32_t offset = address - producer_family_model_address;
        const XgHost3dVector *vertex =
            &producer_family_host_input.vertices[offset / 8u];

        if ((offset & 7u) == 0u)
            return (uint16_t)vertex->x |
                   ((uint32_t)(uint16_t)vertex->y << 16u);
        return (uint16_t)vertex->z |
               ((uint32_t)vertex->pad << 16u);
    }
    if (address == stack_base + 0xd0u) return PRODUCER_FAMILY_OT_BASE;
    if (address == stack_base + 0xd8u) return 0u;
    if (address == stack_base + 0x20u) return stack_base + 0xc8u;
    if (address == stack_base + 0x24u) return stack_base + 0xccu;
    if (address >= stack_base + 0x10u && address <= stack_base + 0x1cu)
        return producer_family_packet_address + 8u +
               (address - stack_base - 0x10u) * 2u;
    if (address == actor_address + 0x04u) return PRODUCER_FAMILY_SECONDARY;
    if (address == actor_address + 0x08u) return producer_family_model_address;
    if (address == actor_address + 0x20u) return 0u;
    if (address == actor_address + 0x28u) return 1000u;
    if (address == actor_address + 0x4cu) return PRODUCER_FAMILY_STATE;
    if (address == actor_address + 0x58u) return 0x40u;
    switch (address) {
    case 0x8004f37cu: return 0u;
    case 0x80050100u: return 0u;
    case 0x800af9f8u: return 0x80u;
    case 0x800adbfcu: return 2u;
    case 0x800afb10u: return PRODUCER_FAMILY_ACTOR_BASE;
    case 0x800b2268u: return 0u;
    case PRODUCER_FAMILY_STATE + 0x00u: return 0u;
    case PRODUCER_FAMILY_STATE + 0x04u: return 0u;
    case PRODUCER_FAMILY_STATE + 0x14u: return 0u;
    case PRODUCER_FAMILY_STATE + 0x50u: return 0u;
    case PRODUCER_FAMILY_STATE + 0x54u: return 0x1000u;
    case PRODUCER_FAMILY_STATE + 0x58u: return 0u;
    case 0x800afa64u: return 0x1000u;
    case 0x800afa68u: return 0u;
    case 0x800afa6cu: return 0x1000u;
    case 0x800afa70u: return 0u;
    case 0x800afa74u: return 0x1000u;
    case 0x800afa78u: return 0u;
    case 0x800afa7cu: return 0u;
    case 0x800afa80u: return 0u;
    default: break;
    }
    if (address < producer_family_packet_address + 4u ||
        address >= producer_family_packet_address + 40u)
        return 0u;
    return producer_family_packet_words[
        (address - producer_family_packet_address - 4u) / 4u];
}

static void producer_family_write_word(uint32_t address, uint32_t value) {
    if (address == producer_family_packet_address)
        producer_family_packet_tag = value;
    else if (address == producer_family_ot_bucket_address)
        producer_family_ot_word = value;
    else if (address >= producer_family_packet_address + 4u &&
        address < producer_family_packet_address + 40u)
        producer_family_packet_words[
            (address - producer_family_packet_address - 4u) / 4u] = value;
}

enum {
    PARTICLE_BASE = 0x80016000u,
    PARTICLE_MATRIX = 0x80017000u,
    PARTICLE_SCALE = 0x80017100u,
    PARTICLE_STACK = 0x801ff000u,
    PARTICLE_OT_BASE = 0x80018000u,
};

static uint8_t particle_memory[0xc0];
static uint32_t particle_matrix_words[8];
static uint32_t particle_scale_words[3];
static int16_t particle_table[12];
static uint32_t particle_ot_word;
static uint32_t particle_payload_read_count;
static uint32_t particle_buffer_index;
static uint32_t particle_composition_selector;

static uint32_t particle_load_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void particle_store_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8u);
    bytes[2] = (uint8_t)(value >> 16u);
    bytes[3] = (uint8_t)(value >> 24u);
}

static void particle_store_u16(uint32_t offset, uint16_t value) {
    particle_memory[offset] = (uint8_t)value;
    particle_memory[offset + 1u] = (uint8_t)(value >> 8u);
}

static uint32_t particle_read_word(uint32_t address) {
    const uint32_t packet_address = PARTICLE_BASE + 0x50u;
    const uint32_t ot_address = PARTICLE_OT_BASE + 0xccu + 4u;

    if (address >= PARTICLE_BASE && address + 4u <= PARTICLE_BASE +
            sizeof(particle_memory)) {
        if ((address >= packet_address + 4u && address < packet_address + 40u) ||
            (address >= packet_address + 0x2cu &&
             address < packet_address + 0x50u))
            ++particle_payload_read_count;
        return particle_load_u32(&particle_memory[address - PARTICLE_BASE]);
    }
    if (address >= PARTICLE_MATRIX && address < PARTICLE_MATRIX + 32u)
        return particle_matrix_words[(address - PARTICLE_MATRIX) / 4u];
    if (address >= PARTICLE_SCALE && address < PARTICLE_SCALE + 12u)
        return particle_scale_words[(address - PARTICLE_SCALE) / 4u];
    if (address == PARTICLE_STACK + 0x10u) return PARTICLE_SCALE;
    if (address == PARTICLE_STACK + 0x14u)
        return particle_composition_selector;
    if (address == 0x800523f0u) return 0x10000000u;
    if (address == 0x800adb08u) return particle_buffer_index;
    if (address == 0x80050100u) return 0u;
    if (address == 0x800c426cu) return PARTICLE_OT_BASE;
    if (address == ot_address) return particle_ot_word;
    return 0u;
}

static uint16_t particle_read_half(uint32_t address) {
    if (address >= 0x800af27cu && address < 0x800af294u)
        return (uint16_t)particle_table[(address - 0x800af27cu) / 2u];
    if (address >= PARTICLE_BASE && address + 2u <= PARTICLE_BASE +
            sizeof(particle_memory)) {
        const uint32_t offset = address - PARTICLE_BASE;

        return (uint16_t)particle_memory[offset] |
               ((uint16_t)particle_memory[offset + 1u] << 8u);
    }
    return 0u;
}

static uint8_t particle_read_byte(uint32_t address) {
    if (address >= PARTICLE_BASE &&
        address < PARTICLE_BASE + sizeof(particle_memory))
        return particle_memory[address - PARTICLE_BASE];
    return 0u;
}

static void particle_write_word(uint32_t address, uint32_t value) {
    const uint32_t ot_address = PARTICLE_OT_BASE + 0xccu + 4u;

    if (address >= PARTICLE_BASE && address + 4u <= PARTICLE_BASE +
            sizeof(particle_memory))
        particle_store_u32(&particle_memory[address - PARTICLE_BASE], value);
    else if (address == ot_address)
        particle_ot_word = value;
}

static void particle_write_byte(uint32_t address, uint8_t value) {
    if (address >= PARTICLE_BASE &&
        address < PARTICLE_BASE + sizeof(particle_memory))
        particle_memory[address - PARTICLE_BASE] = value;
}

enum {
    ZOOM_MEMORY_BASE = 0x800b1100u,
    ZOOM_MEMORY_SIZE = 0x400u,
    ZOOM_STACK_BASE = 0x801fc000u,
    ZOOM_STACK_SIZE = 0x3000u,
    ZOOM_INIT_ENTRY_SP = 0x801fd000u,
    ZOOM_ENTRY_SP = 0x801fe000u,
};

static uint8_t zoom_memory[ZOOM_MEMORY_SIZE];
static uint8_t zoom_stack[ZOOM_STACK_SIZE];
static uint32_t zoom_scale;
static uint32_t zoom_buffer_index;
static uint32_t zoom_context_address;
static uint32_t zoom_ot_word;
static uint32_t zoom_payload_read_count;
static uint32_t zoom_xy_write_count;
static bool zoom_field_identity;

static uint32_t zoom_ft4_address(uint32_t quad, uint32_t buffer) {
    return UINT32_C(0x800b1274) + quad * 0x50u + buffer * 0x28u;
}

static uint32_t zoom_draw_mode_address(uint32_t quad, uint32_t buffer) {
    return UINT32_C(0x800b11ac) + quad * 0x18u + buffer * 0x0cu;
}

static bool zoom_packet_payload_address(uint32_t address) {
    uint32_t buffer;
    uint32_t quad;

    for (buffer = 0u; buffer < 2u; ++buffer) {
        for (quad = 0u; quad < 5u; ++quad) {
            const uint32_t ft4 = zoom_ft4_address(quad, buffer);
            const uint32_t draw_mode = zoom_draw_mode_address(quad, buffer);

            const uint32_t projection = UINT32_C(0x800b1404) + quad * 0x20u;

            if ((address >= ft4 && address <= ft4 + 0x24u) ||
                (address >= draw_mode && address <= draw_mode + 8u) ||
                (address >= projection && address <= projection + 0x1cu))
                return true;
        }
    }
    return false;
}

static bool zoom_xy_address(uint32_t address) {
    uint32_t buffer;
    uint32_t quad;
    uint32_t vertex;

    for (buffer = 0u; buffer < 2u; ++buffer) {
        for (quad = 0u; quad < 5u; ++quad) {
            const uint32_t ft4 = zoom_ft4_address(quad, buffer);

            for (vertex = 0u; vertex < 4u; ++vertex) {
                if (address == ft4 + 8u + vertex * 8u) return true;
            }
        }
    }
    return false;
}

static uint32_t zoom_read_word(uint32_t address) {
    if (zoom_field_identity && address == PRODUCER_ENTRY)
        return UINT32_C(0x27bdff18);
    if (zoom_field_identity && address == CALLER_SITE)
        return JAL_INSTRUCTION;
    if (zoom_field_identity && address == RETURN_SITE)
        return UINT32_C(0x0c01d1c0);
    if (address >= ZOOM_MEMORY_BASE &&
        address + 4u <= ZOOM_MEMORY_BASE + ZOOM_MEMORY_SIZE) {
        if (zoom_packet_payload_address(address)) ++zoom_payload_read_count;
        return particle_load_u32(&zoom_memory[address - ZOOM_MEMORY_BASE]);
    }
    if (address >= ZOOM_STACK_BASE &&
        address + 4u <= ZOOM_STACK_BASE + ZOOM_STACK_SIZE)
        return particle_load_u32(&zoom_stack[address - ZOOM_STACK_BASE]);
    if (address == UINT32_C(0x800c2684)) return zoom_scale;
    if (address == UINT32_C(0x800adb08)) return zoom_buffer_index;
    if (address == UINT32_C(0x800c426c)) return zoom_context_address;
    if (address == zoom_context_address + 0x80d4u) return zoom_ot_word;
    return 0u;
}

static void zoom_write_word(uint32_t address, uint32_t value) {
    if (address >= ZOOM_MEMORY_BASE &&
        address + 4u <= ZOOM_MEMORY_BASE + ZOOM_MEMORY_SIZE) {
        if (zoom_xy_address(address)) ++zoom_xy_write_count;
        particle_store_u32(&zoom_memory[address - ZOOM_MEMORY_BASE], value);
    } else if (address == zoom_context_address + 0x80d4u) {
        zoom_ot_word = value;
    }
}

static void zoom_store_word(uint32_t address, uint32_t value) {
    if (address >= ZOOM_MEMORY_BASE &&
        address + 4u <= ZOOM_MEMORY_BASE + ZOOM_MEMORY_SIZE)
        particle_store_u32(&zoom_memory[address - ZOOM_MEMORY_BASE], value);
    else if (address >= ZOOM_STACK_BASE &&
             address + 4u <= ZOOM_STACK_BASE + ZOOM_STACK_SIZE)
        particle_store_u32(&zoom_stack[address - ZOOM_STACK_BASE], value);
}

enum {
    PROJECTED_MEMORY_BASE = 0x800b4000u,
    PROJECTED_MEMORY_SIZE = 0x1000u,
    PROJECTED_OBJECT = PROJECTED_MEMORY_BASE,
    PROJECTED_SECOND_OBJECT = PROJECTED_MEMORY_BASE + 0x800u,
    PROJECTED_EYE = PROJECTED_MEMORY_BASE + 0x400u,
    PROJECTED_AT = PROJECTED_MEMORY_BASE + 0x408u,
    PROJECTED_MATRIX = PROJECTED_MEMORY_BASE + 0x420u,
    PROJECTED_COLORS = PROJECTED_MEMORY_BASE + 0x460u,
    PROJECTED_OT = PROJECTED_MEMORY_BASE + 0x500u,
    PROJECTED_STACK_BASE = 0x801f8000u,
    PROJECTED_STACK_SIZE = 0x4000u,
    PROJECTED_INIT_SP = 0x801fa000u,
    PROJECTED_CALL_SP = 0x801fb000u,
};

static uint8_t projected_memory[PROJECTED_MEMORY_SIZE];
static uint8_t projected_stack[PROJECTED_STACK_SIZE];
static uint32_t projected_payload_read_count;
static uint32_t projected_packet_write_count;
static uint32_t projected_ot_write_count;
static bool projected_use_field_ot;
static bool projected_use_overlay_2e;
static bool projected_watch_registered;
static bool projected_table_watch_registered;
static bool world_sky_watch_registered;
static bool world_sky_table_watch_registered;
static bool world_horizon_watch_registered;
static bool world_horizon_table_watch_registered;
static bool world_effects_watch_registered;
static bool model_ft4_watch_registered;
static bool sprite_ft4_watch_registered;
static bool sidecar_packet_watch_registered;
static bool sidecar_source_watch_registered;

static void record_render_code_watch(uint32_t address, uint32_t size) {
    if (address <= UINT32_C(0x000273c4) &&
        UINT32_C(0x000273c8) <= address + size)
        projected_watch_registered = true;
    if (address <= UINT32_C(0x00056b94) &&
        UINT32_C(0x00056b98) <= address + size)
        projected_table_watch_registered = true;
    if (address <= UINT32_C(0x000737ec) &&
        UINT32_C(0x000737f0) <= address + size)
        world_sky_watch_registered = true;
    if (address <= UINT32_C(0x0009a280) &&
        UINT32_C(0x0009a284) <= address + size)
        world_sky_table_watch_registered = true;
    if (address <= UINT32_C(0x00073b04) &&
        UINT32_C(0x00073e30) <= address + size)
        world_horizon_watch_registered = true;
    if (address <= UINT32_C(0x0009a300) &&
        UINT32_C(0x0009a340) <= address + size)
        world_horizon_table_watch_registered = true;
    if (address <= UINT32_C(0x00089c78) &&
        UINT32_C(0x0008a2c8) <= address + size)
        world_effects_watch_registered = true;
    if (address <= UINT32_C(0x0002e268) &&
        UINT32_C(0x0002e26c) <= address + size)
        model_ft4_watch_registered = true;
    if (address <= UINT32_C(0x0001e86c) &&
        UINT32_C(0x0001e870) <= address + size)
        sprite_ft4_watch_registered = true;
    if (address <= UINT32_C(0x0009e200) &&
        UINT32_C(0x0009e228) <= address + size)
        sidecar_packet_watch_registered = true;
    if (address <= UINT32_C(0x00062104) &&
        UINT32_C(0x00062120) <= address + size)
        sidecar_source_watch_registered = true;
}

static uint32_t projected_packet_size(uint32_t address) {
    uint32_t buffer;
    uint32_t strip;

    for (buffer = 0u; buffer < 2u; ++buffer) {
        for (strip = 0u; strip < 8u; ++strip) {
            if (address == PROJECTED_OBJECT + buffer * 0x140u + strip * 0x28u)
                return 0x28u;
        }
        if (address == PROJECTED_OBJECT + 0x280u + buffer * 0x18u ||
            address == PROJECTED_OBJECT + 0x2b0u + buffer * 0x18u)
            return 0x18u;
        if (address == PROJECTED_OBJECT + 0x2e0u + buffer * 0x24u)
            return 0x24u;
    }
    return 0u;
}

static bool projected_packet_payload_address(uint32_t address,
                                              uint32_t width) {
    uint32_t buffer;
    uint32_t strip;

    if (projected_use_overlay_2e && width == 4u &&
        (address == PROJECTED_OBJECT + 0x308u ||
         address == PROJECTED_OBJECT + 0x364u ||
         (address >= PROJECTED_OBJECT + 0x4b8u &&
          address <= PROJECTED_OBJECT + 0x4c8u)))
        return false;
    for (buffer = 0u; buffer < 2u; ++buffer) {
        for (strip = 0u; strip < 8u; ++strip) {
            const uint32_t packet =
                PROJECTED_OBJECT + buffer * 0x140u + strip * 0x28u;

            if (address < packet + 0x28u && address + width > packet + 4u)
                return true;
        }
        {
            const uint32_t packets[3] = {
                PROJECTED_OBJECT + 0x280u + buffer * 0x18u,
                PROJECTED_OBJECT + 0x2b0u + buffer * 0x18u,
                PROJECTED_OBJECT + 0x2e0u + buffer * 0x24u,
            };
            const uint32_t sizes[3] = { 0x18u, 0x18u, 0x24u };
            uint32_t packet_index;

            for (packet_index = 0u; packet_index < 3u; ++packet_index) {
                if (address < packets[packet_index] + sizes[packet_index] &&
                    address + width > packets[packet_index] + 4u)
                    return true;
            }
        }
    }
    return false;
}

static uint32_t projected_read_word(uint32_t address) {
    if (address == UINT32_C(0x800625a0) && projected_use_overlay_2e)
        return PROJECTED_OBJECT;
    if (address == UINT32_C(0x800c426c) && projected_use_field_ot)
        return PROJECTED_OT - 0x40ccu - 7u * 4u;
    if (address == UINT32_C(0x800ccb00) && !projected_use_field_ot)
        return PROJECTED_OT - 0x3ffcu - 0x70u;
    if (address == UINT32_C(0x800ccb04) && !projected_use_field_ot)
        return PROJECTED_OT - 0x3ffcu;
    if (address >= PROJECTED_MEMORY_BASE &&
        address + 4u <= PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE) {
        if (projected_packet_payload_address(address, 4u))
            ++projected_payload_read_count;
        return particle_load_u32(
            &projected_memory[address - PROJECTED_MEMORY_BASE]);
    }
    if (address >= PROJECTED_STACK_BASE &&
        address + 4u <= PROJECTED_STACK_BASE + PROJECTED_STACK_SIZE)
        return particle_load_u32(&projected_stack[address - PROJECTED_STACK_BASE]);
    return 0u;
}

static uint16_t projected_read_half(uint32_t address) {
    if (address >= PROJECTED_MEMORY_BASE &&
        address + 2u <= PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE) {
        const uint32_t offset = address - PROJECTED_MEMORY_BASE;

        if (projected_packet_payload_address(address, 2u))
            ++projected_payload_read_count;
        return (uint16_t)projected_memory[offset] |
            ((uint16_t)projected_memory[offset + 1u] << 8u);
    }
    if (address >= PROJECTED_STACK_BASE &&
        address + 2u <= PROJECTED_STACK_BASE + PROJECTED_STACK_SIZE) {
        const uint32_t offset = address - PROJECTED_STACK_BASE;

        return (uint16_t)projected_stack[offset] |
            ((uint16_t)projected_stack[offset + 1u] << 8u);
    }
    if (address >= UINT32_C(0x80057030) &&
        address <= UINT32_C(0x80057830))
        return 0u;
    return 0u;
}

static uint8_t projected_read_byte(uint32_t address) {
    if (address >= PROJECTED_MEMORY_BASE &&
        address < PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE) {
        if (projected_packet_payload_address(address, 1u))
            ++projected_payload_read_count;
        return projected_memory[address - PROJECTED_MEMORY_BASE];
    }
    if (address >= PROJECTED_STACK_BASE &&
        address < PROJECTED_STACK_BASE + PROJECTED_STACK_SIZE)
        return projected_stack[address - PROJECTED_STACK_BASE];
    return 0u;
}

static void projected_write_word(uint32_t address, uint32_t value) {
    if (address >= PROJECTED_MEMORY_BASE &&
        address + 4u <= PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE) {
        if (projected_packet_size(address) != 0u) ++projected_packet_write_count;
        if (address == PROJECTED_OT) ++projected_ot_write_count;
        particle_store_u32(&projected_memory[address - PROJECTED_MEMORY_BASE],
                           value);
    } else if (address >= PROJECTED_STACK_BASE &&
               address + 4u <= PROJECTED_STACK_BASE + PROJECTED_STACK_SIZE) {
        particle_store_u32(&projected_stack[address - PROJECTED_STACK_BASE],
                           value);
    }
}

static void projected_write_half(uint32_t address, uint16_t value) {
    if (address >= PROJECTED_MEMORY_BASE &&
        address + 2u <= PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE) {
        const uint32_t offset = address - PROJECTED_MEMORY_BASE;

        if (projected_packet_payload_address(address, 2u))
            ++projected_packet_write_count;
        projected_memory[offset] = (uint8_t)value;
        projected_memory[offset + 1u] = (uint8_t)(value >> 8u);
    }
}

static void projected_write_byte(uint32_t address, uint8_t value) {
    if (address >= PROJECTED_MEMORY_BASE &&
        address < PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE) {
        if (projected_packet_payload_address(address, 1u))
            ++projected_packet_write_count;
        projected_memory[address - PROJECTED_MEMORY_BASE] = value;
    }
}

static void projected_store_word(uint32_t address, uint32_t value) {
    if (address >= PROJECTED_MEMORY_BASE &&
        address + 4u <= PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE)
        particle_store_u32(&projected_memory[address - PROJECTED_MEMORY_BASE],
                           value);
    else if (address >= PROJECTED_STACK_BASE &&
             address + 4u <= PROJECTED_STACK_BASE + PROJECTED_STACK_SIZE)
        particle_store_u32(&projected_stack[address - PROJECTED_STACK_BASE],
                           value);
}

static void projected_store_half(uint32_t address, uint16_t value) {
    uint8_t *memory;
    uint32_t offset;

    if (address >= PROJECTED_MEMORY_BASE &&
        address + 2u <= PROJECTED_MEMORY_BASE + PROJECTED_MEMORY_SIZE) {
        memory = projected_memory;
        offset = address - PROJECTED_MEMORY_BASE;
    } else {
        memory = projected_stack;
        offset = address - PROJECTED_STACK_BASE;
    }
    memory[offset] = (uint8_t)value;
    memory[offset + 1u] = (uint8_t)(value >> 8u);
}

static uint32_t pack_s16(int16_t low, int16_t high) {
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16u);
}

enum {
    WORLD_NATIVE_RAM_SIZE = 0x200000u,
    WORLD_NATIVE_SCRATCH_SIZE = 0x400u,
    WORLD_NATIVE_CONTEXT = 0x80010000u,
    WORLD_NATIVE_OT = 0x80011000u,
    WORLD_NATIVE_ACTOR = 0x80012000u,
    WORLD_NATIVE_ACTOR_DATA = 0x80012100u,
    WORLD_NATIVE_ACTOR_DESCRIPTOR = 0x80012200u,
    WORLD_NATIVE_DECORATION_PACKETS = 0x80016000u,
    WORLD_NATIVE_PACKETS = 0x80018000u,
    WORLD_NATIVE_STACK = 0x801ff000u,
    WORLD_NATIVE_SCRATCH_STACK = 0x1f800310u,
};

static uint8_t world_native_ram[WORLD_NATIVE_RAM_SIZE];
static uint8_t world_native_scratch[WORLD_NATIVE_SCRATCH_SIZE];
static uint32_t world_native_write_count;

static uint8_t *world_native_pointer(uint32_t address, uint32_t size) {
    const uint32_t physical = address & UINT32_C(0x1fffffff);

    if (size == 0u) return NULL;
    if (physical >= UINT32_C(0x1f800000) &&
        physical - UINT32_C(0x1f800000) <=
            WORLD_NATIVE_SCRATCH_SIZE - size)
        return &world_native_scratch[physical - UINT32_C(0x1f800000)];
    if (physical <= WORLD_NATIVE_RAM_SIZE - size)
        return &world_native_ram[physical];
    return NULL;
}

static void world_native_put_u8(uint32_t address, uint8_t value) {
    uint8_t *destination = world_native_pointer(address, 1u);

    if (destination != NULL) *destination = value;
}

static void world_native_put_u16(uint32_t address, uint16_t value) {
    uint8_t *destination = world_native_pointer(address, 2u);

    if (destination == NULL) return;
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void world_native_put_u32(uint32_t address, uint32_t value) {
    uint8_t *destination = world_native_pointer(address, 4u);

    if (destination == NULL) return;
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static uint8_t world_native_read_byte(uint32_t address) {
    uint8_t *source = world_native_pointer(address, 1u);

    return source == NULL ? 0u : source[0];
}

static uint16_t world_native_read_half(uint32_t address) {
    uint8_t *source = world_native_pointer(address, 2u);

    return source == NULL ? 0u :
        (uint16_t)source[0] | ((uint16_t)source[1] << 8u);
}

static uint32_t world_native_read_word(uint32_t address) {
    uint8_t *source = world_native_pointer(address, 4u);

    return source == NULL ? 0u :
        (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
        ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static void world_native_write_half(uint32_t address, uint16_t value) {
    if (world_native_pointer(address, 2u) == NULL) return;
    ++world_native_write_count;
    world_native_put_u16(address, value);
}

static void world_native_write_word(uint32_t address, uint32_t value) {
    if (world_native_pointer(address, 4u) == NULL) return;
    ++world_native_write_count;
    world_native_put_u32(address, value);
}

static void world_native_put_identity_matrix(uint32_t address,
                                             int32_t translation_z) {
    world_native_put_u32(address, pack_s16(4096, 0));
    world_native_put_u32(address + 4u, 0u);
    world_native_put_u32(address + 8u, pack_s16(4096, 0));
    world_native_put_u32(address + 12u, 0u);
    world_native_put_u32(address + 16u, pack_s16(4096, 0));
    world_native_put_u32(address + 20u, 0u);
    world_native_put_u32(address + 24u, 0u);
    world_native_put_u32(address + 28u, (uint32_t)translation_z);
}

static void configure_empty_world_native_cpu(CPUState *cpu) {
    memset(world_native_ram, 0, sizeof(world_native_ram));
    memset(world_native_scratch, 0, sizeof(world_native_scratch));
    world_native_write_count = 0u;
    memset(cpu, 0, sizeof(*cpu));
    cpu->read_byte = world_native_read_byte;
    cpu->read_half = world_native_read_half;
    cpu->read_word = world_native_read_word;
    cpu->write_half = world_native_write_half;
    cpu->write_word = world_native_write_word;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 256u;
    cpu->gte_ctrl[30] = 1024u;
}

enum {
    RESIDENT_LINE_F2_HEAP = 0x800d3a2cu,
    RESIDENT_LINE_F2_STATE = 0x800d3000u,
    RESIDENT_LINE_F2_LEVEL = 4u,
    RESIDENT_LINE_F2_COUNT = 3u,
    RESIDENT_LINE_F2_BUFFER = 1u,
};

static uint32_t resident_line_f2_packet(uint32_t index) {
    return RESIDENT_LINE_F2_HEAP + 0x908u +
        (RESIDENT_LINE_F2_BUFFER + index * 2u) * 0x10u;
}

static void configure_resident_line_f2_cpu(CPUState *cpu) {
    static const uint8_t source_y[RESIDENT_LINE_F2_COUNT] = {2u, 5u, 9u};

    configure_empty_world_native_cpu(cpu);
    world_native_put_u32(UINT32_C(0x800c3ea4), RESIDENT_LINE_F2_HEAP);
    world_native_put_u32(UINT32_C(0x800d2d28), RESIDENT_LINE_F2_STATE);
    world_native_put_u8(UINT32_C(0x800ccb34), RESIDENT_LINE_F2_BUFFER);
    for (uint32_t index = 0u; index < RESIDENT_LINE_F2_COUNT; ++index) {
        const uint32_t packet = resident_line_f2_packet(index);

        world_native_put_u8(
            UINT32_C(0x800c3202) + RESIDENT_LINE_F2_LEVEL * 6u + index,
            source_y[index]);
        world_native_put_u32(packet, UINT32_C(0x03000000));
        world_native_put_u32(packet + 4u, UINT32_C(0x40ffffff));
    }
    cpu->gpr[4] = RESIDENT_LINE_F2_LEVEL;
    cpu->gpr[5] = RESIDENT_LINE_F2_COUNT;
}

static void materialize_resident_line_f2_output(void) {
    static const uint8_t source_y[RESIDENT_LINE_F2_COUNT] = {2u, 5u, 9u};

    for (uint32_t index = 0u; index < RESIDENT_LINE_F2_COUNT; ++index) {
        const uint32_t packet = resident_line_f2_packet(index);
        const uint16_t y = (uint16_t)(source_y[index] + 0x5eu);

        world_native_put_u32(packet + 8u, 0x0cu | ((uint32_t)y << 16u));
        world_native_put_u32(packet + 12u, 0x12u | ((uint32_t)y << 16u));
    }
    world_native_put_u8(
        RESIDENT_LINE_F2_STATE + 0x97u, RESIDENT_LINE_F2_COUNT);
    world_native_put_u8(
        RESIDENT_LINE_F2_STATE + 0x98u, RESIDENT_LINE_F2_BUFFER);
}

enum {
    WORLD_MEMORY_BASE = 0x80050000u,
    WORLD_MEMORY_SIZE = 0x50000u,
    WORLD_VERTEX_BASE = 0x8009a280u,
    WORLD_CAMERA_MATRIX = 0x8009c808u,
    WORLD_CONTEXT_OBJECT = 0x8009bf00u,
    WORLD_OT_BASE = 0x800b0000u,
    WORLD_OT_SIZE = 0x4000u,
    WORLD_SCRATCH_BASE = 0x1f800000u,
    WORLD_SCRATCH_SIZE = 0x110u,
    WORLD_STACK_BASE = 0x801fe000u,
    WORLD_STACK_SIZE = 0x2000u,
};

static uint8_t world_memory[WORLD_MEMORY_SIZE];
static uint8_t world_ot[WORLD_OT_SIZE];
static uint8_t world_scratch[WORLD_SCRATCH_SIZE];
static uint8_t world_stack[WORLD_STACK_SIZE];
static uint32_t world_payload_read_count;
static uint32_t world_tag_write_count;
static uint32_t world_xy_write_count;
static uint32_t world_ot_write_count;
static uint32_t world_scratch_write_count;
static uint32_t world_effects_packet_write_count;
static uint32_t world_horizon_packet_write_count;
static uint32_t world_horizon_window_write_count;
static uint32_t world_minimap_tag_write_count;
static uint32_t world_minimap_xy_write_count;

static uint32_t world_packet_address(uint32_t quad) {
    return UINT32_C(0x8009d1b8) + quad * 0x48u;
}

static uint32_t world_horizon_packet_address(uint32_t quad) {
    return UINT32_C(0x8009c76c) + quad * 0x50u;
}

static uint32_t world_effects_packet_address(uint32_t quad) {
    return UINT32_C(0x8009d800) + quad * 0x28u;
}

static uint32_t world_minimap_packet_address(uint32_t triangle) {
    return UINT32_C(0x8009c6d4) + triangle * 0x1cu;
}

static bool world_packet_payload_address(uint32_t address) {
    uint32_t quad;

    for (quad = 0u; quad < 4u; ++quad) {
        const uint32_t packet = world_packet_address(quad);

        if (address >= packet + 4u && address < packet + 0x24u) return true;
        if (address >= world_minimap_packet_address(quad) + 4u &&
            address < world_minimap_packet_address(quad) + 0x1cu)
            return true;
    }
    return false;
}

static uint32_t world_read_word(uint32_t address) {
    if (address >= WORLD_MEMORY_BASE &&
        address + 4u <= WORLD_MEMORY_BASE + WORLD_MEMORY_SIZE) {
        if (world_packet_payload_address(address)) ++world_payload_read_count;
        return particle_load_u32(&world_memory[address - WORLD_MEMORY_BASE]);
    }
    if (address >= WORLD_OT_BASE &&
        address + 4u <= WORLD_OT_BASE + WORLD_OT_SIZE)
        return particle_load_u32(&world_ot[address - WORLD_OT_BASE]);
    if (address >= WORLD_SCRATCH_BASE &&
        address + 4u <= WORLD_SCRATCH_BASE + WORLD_SCRATCH_SIZE)
        return particle_load_u32(&world_scratch[address - WORLD_SCRATCH_BASE]);
    if (address >= WORLD_STACK_BASE &&
        address + 4u <= WORLD_STACK_BASE + WORLD_STACK_SIZE)
        return particle_load_u32(&world_stack[address - WORLD_STACK_BASE]);
    return 0u;
}

static uint16_t world_read_half(uint32_t address) {
    const uint32_t aligned = address & ~3u;
    const uint32_t word = world_read_word(aligned);

    return (uint16_t)(word >> ((address & 2u) * 8u));
}

static void world_write_word(uint32_t address, uint32_t value) {
    uint32_t quad;

    if (address >= WORLD_MEMORY_BASE &&
        address + 4u <= WORLD_MEMORY_BASE + WORLD_MEMORY_SIZE) {
        if (address >= world_effects_packet_address(0u) &&
            address < world_effects_packet_address(0x100u))
            ++world_effects_packet_write_count;
        for (quad = 0u; quad < 2u; ++quad) {
            const uint32_t packet = world_horizon_packet_address(quad);

            if (address >= packet && address < packet + 0x28u)
                ++world_horizon_packet_write_count;
        }
        if (address == UINT32_C(0x8009d3d8) ||
            address == UINT32_C(0x8009d3e4))
            ++world_horizon_window_write_count;
        for (quad = 0u; quad < 4u; ++quad) {
            const uint32_t packet = world_packet_address(quad);
            const uint32_t minimap_packet =
                world_minimap_packet_address(quad);

            if (address == packet) ++world_tag_write_count;
            if (address == packet + 8u || address == packet + 16u ||
                address == packet + 24u || address == packet + 32u)
                ++world_xy_write_count;
            if (address == minimap_packet) ++world_minimap_tag_write_count;
            if (address == minimap_packet + 8u ||
                address == minimap_packet + 16u ||
                address == minimap_packet + 24u)
                ++world_minimap_xy_write_count;
        }
        particle_store_u32(&world_memory[address - WORLD_MEMORY_BASE], value);
    } else if (address >= WORLD_OT_BASE &&
               address + 4u <= WORLD_OT_BASE + WORLD_OT_SIZE) {
        ++world_ot_write_count;
        particle_store_u32(&world_ot[address - WORLD_OT_BASE], value);
    } else if (address >= WORLD_SCRATCH_BASE &&
               address + 4u <= WORLD_SCRATCH_BASE + WORLD_SCRATCH_SIZE) {
        ++world_scratch_write_count;
        particle_store_u32(&world_scratch[address - WORLD_SCRATCH_BASE], value);
    } else if (address >= WORLD_STACK_BASE &&
               address + 4u <= WORLD_STACK_BASE + WORLD_STACK_SIZE) {
        particle_store_u32(&world_stack[address - WORLD_STACK_BASE], value);
    }
}

static void world_store_word(uint32_t address, uint32_t value) {
    if (address >= WORLD_MEMORY_BASE &&
        address + 4u <= WORLD_MEMORY_BASE + WORLD_MEMORY_SIZE)
        particle_store_u32(&world_memory[address - WORLD_MEMORY_BASE], value);
    else if (address >= WORLD_OT_BASE &&
             address + 4u <= WORLD_OT_BASE + WORLD_OT_SIZE)
        particle_store_u32(&world_ot[address - WORLD_OT_BASE], value);
    else if (address >= WORLD_SCRATCH_BASE &&
             address + 4u <= WORLD_SCRATCH_BASE + WORLD_SCRATCH_SIZE)
        particle_store_u32(&world_scratch[address - WORLD_SCRATCH_BASE], value);
    else if (address >= WORLD_STACK_BASE &&
             address + 4u <= WORLD_STACK_BASE + WORLD_STACK_SIZE)
        particle_store_u32(&world_stack[address - WORLD_STACK_BASE], value);
}

static void world_store_half(uint32_t address, uint16_t value) {
    const uint32_t aligned = address & ~3u;
    uint32_t word = world_read_word(aligned);
    const uint32_t shift = (address & 2u) * 8u;

    word = (word & ~(UINT32_C(0xffff) << shift)) |
        ((uint32_t)value << shift);
    world_store_word(aligned, word);
}

static void configure_world_sky_cpu(CPUState *cpu) {
    static const XgHost3dVector vertices[4][4] = {
        { { -4096, -768, 4096, 0u }, { 4096, -768, 4096, 0u },
          { -4096, 1024, 4096, 0u }, { 4096, 1024, 4096, 0u } },
        { { -4096, -1152, 4096, 0u }, { 4096, -1152, 4096, 0u },
          { -4096, -768, 4096, 0u }, { 4096, -768, 4096, 0u } },
        { { -4096, -3200, 3072, 0u }, { 4096, -3200, 3072, 0u },
          { -4096, -1152, 4096, 0u }, { 4096, -1152, 4096, 0u } },
        { { -4096, -4096, 1024, 0u }, { 4096, -4096, 1024, 0u },
          { -4096, -3200, 3072, 0u }, { 4096, -3200, 3072, 0u } },
    };
    const uint32_t matrix_words[8] = {
        pack_s16(4096, 0), pack_s16(0, 0), pack_s16(3548, -2046),
        pack_s16(0, 2046), pack_s16(3548, 0), 0u, 273u, 798u,
    };
    uint32_t quad;
    uint32_t vertex;

    memset(world_memory, 0, sizeof(world_memory));
    memset(world_ot, 0, sizeof(world_ot));
    memset(world_scratch, 0, sizeof(world_scratch));
    memset(world_stack, 0, sizeof(world_stack));
    world_payload_read_count = 0u;
    world_tag_write_count = 0u;
    world_xy_write_count = 0u;
    world_ot_write_count = 0u;
    world_scratch_write_count = 0u;
    world_effects_packet_write_count = 0u;
    world_horizon_packet_write_count = 0u;
    world_horizon_window_write_count = 0u;
    world_minimap_tag_write_count = 0u;
    world_minimap_xy_write_count = 0u;
    world_store_word(UINT32_C(0x800523f0), UINT32_C(0x10000000));
    for (quad = 0u; quad < 8u; ++quad)
        world_store_word(WORLD_CAMERA_MATRIX + quad * 4u, matrix_words[quad]);
    for (quad = 0u; quad < 4u; ++quad) {
        for (vertex = 0u; vertex < 4u; ++vertex) {
            const uint32_t address =
                WORLD_VERTEX_BASE + (quad * 4u + vertex) * 8u;

            world_store_word(address,
                pack_s16(vertices[quad][vertex].x, vertices[quad][vertex].y));
            world_store_word(address + 4u,
                pack_s16(vertices[quad][vertex].z,
                         (int16_t)vertices[quad][vertex].pad));
        }
        world_store_word(world_packet_address(quad), UINT32_C(0x08000000));
    }
    world_store_word(UINT32_C(0x8009bcdc), 0x100u);
    world_store_word(UINT32_C(0x8009bd38), 0u);
    world_store_word(UINT32_C(0x8009be0c), 140u);
    world_store_word(UINT32_C(0x8009be3c), WORLD_CONTEXT_OBJECT);
    world_store_word(WORLD_CONTEXT_OBJECT + 0x70u, WORLD_OT_BASE);
    world_store_word(UINT32_C(0x8009d7cc), 0u);
    world_store_word(UINT32_C(0x8009d7f0), 1u);
    world_store_word(UINT32_C(0x80050100), 2u);

    memset(cpu, 0, sizeof(*cpu));
    cpu->gpr[31] = UINT32_C(0x80071b60);
    cpu->read_word = world_read_word;
    cpu->read_half = world_read_half;
    cpu->write_word = world_write_word;
}

static void configure_world_horizon_cpu(CPUState *cpu) {
    static const XgHost3dVector vertices[2][4] = {
        { { -4096, -896, 4032, 0u }, { 0, -896, 4032, 0u },
          { -4096, -640, 4032, 0u }, { 0, -640, 4032, 0u } },
        { { 0, -896, 4032, 0u }, { 4096, -896, 4032, 0u },
          { 0, -640, 4032, 0u }, { 4096, -640, 4032, 0u } },
    };
    uint32_t quad;
    uint32_t vertex;

    configure_world_sky_cpu(cpu);
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 140u << 16u;
    cpu->gte_ctrl[26] = 0x100u;
    for (quad = 0u; quad < 2u; ++quad) {
        const uint32_t packet = world_horizon_packet_address(quad);

        for (vertex = 0u; vertex < 4u; ++vertex) {
            const uint32_t address = UINT32_C(0x8009a300) +
                (quad * 4u + vertex) * 8u;

            world_store_word(address,
                pack_s16(vertices[quad][vertex].x, vertices[quad][vertex].y));
            world_store_word(address + 4u,
                pack_s16(vertices[quad][vertex].z,
                         (int16_t)vertices[quad][vertex].pad));
        }
        world_store_word(packet, UINT32_C(0x09000000));
        world_store_word(packet + 4u, UINT32_C(0x2e303030));
        world_store_word(packet + 12u, UINT32_C(0x7f910000));
        world_store_word(packet + 20u, UINT32_C(0x003e0080));
        world_store_word(packet + 28u, UINT32_C(0x00003f00));
        world_store_word(packet + 36u, UINT32_C(0x00003f80));
    }
    world_store_word(UINT32_C(0x8009d3d8), UINT32_C(0x02000000));
    world_store_word(UINT32_C(0x8009d3dc), UINT32_C(0xe2000010));
    world_store_word(UINT32_C(0x8009d3e0), 0u);
    world_store_word(UINT32_C(0x8009d3e4), UINT32_C(0x02000000));
    world_store_word(UINT32_C(0x8009d3e8), UINT32_C(0xe2000000));
    world_store_word(UINT32_C(0x8009d3ec), 0u);
    world_store_word(WORLD_OT_BASE + 248u * 4u, UINT32_C(0xab123456));
    cpu->gpr[29] = UINT32_C(0x801ff000);
    cpu->gpr[31] = UINT32_C(0x80071b58);
}

static void materialize_world_horizon_output(CPUState *cpu) {
    static const int16_t xy[2][4][2] = {
        { { -113, -28 }, { 160, -28 }, { -105, -9 }, { 160, -9 } },
        { { 160, -28 }, { 432, -28 }, { 160, -9 }, { 424, -9 } },
    };
    uint32_t quad;
    uint32_t vertex;

    for (quad = 0u; quad < 2u; ++quad) {
        const uint32_t packet = world_horizon_packet_address(quad);

        for (vertex = 0u; vertex < 4u; ++vertex)
            world_store_word(packet + 8u + vertex * 8u,
                pack_s16(xy[quad][vertex][0], xy[quad][vertex][1]));
    }
    world_store_word(UINT32_C(0x8009d3e4), UINT32_C(0x02123456));
    world_store_word(world_horizon_packet_address(0u),
                     UINT32_C(0x0909d3e4));
    world_store_word(world_horizon_packet_address(1u),
                     UINT32_C(0x0909c76c));
    world_store_word(UINT32_C(0x8009d3d8), UINT32_C(0x0209c7bc));
    world_store_word(WORLD_OT_BASE + 248u * 4u, UINT32_C(0xab09d3d8));
    cpu->gpr[29] -= 0x40u;
}

static void configure_world_effects_cpu(CPUState *cpu) {
    const uint32_t packet_base = UINT32_C(0x8009d800);
    const uint32_t particles = UINT32_C(0x80060000);
    static const XgHost3dVector vertices[4] = {
        { -32, -32, 0, 0u }, { 32, -32, 0, 0u },
        { -32, 32, 0, 0u }, { 32, 32, 0, 0u },
    };
    static const uint16_t uv[4] = {
        0x0000u, 0x003fu, 0x3f00u, 0x3f3fu,
    };
    uint32_t matrix;
    uint32_t vertex;

    configure_world_sky_cpu(cpu);
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 0x100u;
    for (matrix = 0u; matrix < 2u; ++matrix) {
        const uint32_t address = matrix == 0u
            ? UINT32_C(0x8009c808) : UINT32_C(0x8009a180);

        world_store_word(address, pack_s16(4096, 0));
        world_store_word(address + 4u, pack_s16(0, 0));
        world_store_word(address + 8u, pack_s16(4096, 0));
        world_store_word(address + 12u, pack_s16(0, 0));
        world_store_word(address + 16u, pack_s16(4096, 0));
        world_store_word(address + 20u, 0u);
        world_store_word(address + 24u, 0u);
        world_store_word(address + 28u, matrix == 0u ? 512u : 0u);
    }
    for (vertex = 0u; vertex < 4u; ++vertex) {
        const uint32_t address = UINT32_C(0x8009b040) + 0x20u + vertex * 8u;

        world_store_word(address,
                         pack_s16(vertices[vertex].x, vertices[vertex].y));
        world_store_word(address + 4u,
                         pack_s16(vertices[vertex].z,
                                  (int16_t)vertices[vertex].pad));
        world_store_half(UINT32_C(0x8009aff0) + 8u + vertex * 2u, uv[vertex]);
    }
    world_store_word(UINT32_C(0x8009bdf4), particles);
    world_store_half(particles + 6u, 1u);
    world_store_word(particles + 8u, 0u);
    world_store_word(particles + 0x0cu, 0u);
    world_store_word(particles + 0x10u, 0u);
    world_store_word(particles + 0x38u,
                     (uint32_t)4096u | ((uint32_t)4096u << 16u));
    world_store_word(particles + 0x40u, UINT32_C(0x00302010));
    world_store_word(particles + 0x44u, 0u);
    world_store_half(particles + 0x48u, 0x0123u);
    world_store_word(UINT32_C(0x8009d7f0), 1u);
    world_store_word(UINT32_C(0x8009be20), packet_base);
    world_store_word(packet_base, UINT32_C(0x09000000));
    world_store_word(packet_base + 4u, UINT32_C(0x2e000000));
    world_store_half(packet_base + 14u, 0x7fd0u);
    world_store_word(WORLD_OT_BASE + 32u * 4u, UINT32_C(0xab123456));
    cpu->gpr[29] = UINT32_C(0x801ff000);
    cpu->gpr[31] = UINT32_C(0x80071aa8);
}

static void complete_world_effects_cpu(CPUState *cpu, uint32_t count) {
    const uint32_t packet_cursor = UINT32_C(0x8009d800) + count * 0x28u;

    cpu->gpr[18] = packet_cursor + 0x24u;
    cpu->gpr[19] = packet_cursor;
    cpu->gpr[21] = 0x100u;
    cpu->gpr[29] -= 0x50u;
}

static void materialize_world_effects_output(void) {
    const uint32_t packet = UINT32_C(0x8009d800);

    world_store_word(packet, UINT32_C(0x09123456));
    world_store_word(packet + 4u, UINT32_C(0x2e302010));
    world_store_word(packet + 8u, pack_s16(144, 104));
    world_store_word(packet + 12u, UINT32_C(0x7fd00000));
    world_store_word(packet + 16u, pack_s16(176, 104));
    world_store_word(packet + 20u, UINT32_C(0x0123003f));
    world_store_word(packet + 24u, pack_s16(144, 136));
    world_store_word(packet + 28u, UINT32_C(0x00003f00));
    world_store_word(packet + 32u, pack_s16(176, 136));
    world_store_word(packet + 36u, UINT32_C(0x00003f3f));
    world_store_word(WORLD_OT_BASE + 32u * 4u, UINT32_C(0xab09d800));
}

static void configure_world_minimap_cpu(CPUState *cpu) {
    static const XgHost3dVector triangles[4][3] = {
        { { 0, 0, 0, 0u }, { -8, -8, 0, 0u }, { -4, -11, 0, 0u } },
        { { 0, 0, 0, 0u }, { -4, -11, 0, 0u }, { 0, -12, 0, 0u } },
        { { 0, 0, 0, 0u }, { 0, -12, 0, 0u }, { 4, -11, 0, 0u } },
        { { 0, 0, 0, 0u }, { 4, -11, 0, 0u }, { 8, -8, 0, 0u } },
    };
    const uint32_t stack_pointer = UINT32_C(0x801fefc8);
    uint32_t triangle;
    uint32_t vertex;
    uint32_t word;

    configure_world_sky_cpu(cpu);
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 256u;
    world_store_word(UINT32_C(0x8009d55c), 0u);
    world_store_word(UINT32_C(0x8009d564), 0u);
    world_store_word(UINT32_C(0x8009bcdc), 256u);
    world_store_word(UINT32_C(0x8009be0c), 120u);
    world_store_word(UINT32_C(0x8006f160), 0u);
    world_store_word(WORLD_OT_BASE, UINT32_C(0xab123456));
    world_store_word(UINT32_C(0x1f8000bc), UINT32_C(0xcafe2222));
    world_store_word(UINT32_C(0x1f800100), UINT32_C(0xbeef3333));
    for (triangle = 0u; triangle < 4u; ++triangle) {
        const uint32_t packet = world_minimap_packet_address(triangle);

        for (vertex = 0u; vertex < 3u; ++vertex) {
            const uint32_t source = UINT32_C(0x8009a340) +
                (triangle * 3u + vertex) * 8u;

            world_store_word(source,
                pack_s16(triangles[triangle][vertex].x,
                         triangles[triangle][vertex].y));
            world_store_word(source + 4u,
                pack_s16(triangles[triangle][vertex].z,
                         (int16_t)triangles[triangle][vertex].pad));
        }
        world_store_word(packet, UINT32_C(0x06111111) + triangle);
        for (word = 1u; word < 7u; ++word)
            world_store_word(packet + word * 4u,
                UINT32_C(0xa1000000) + triangle * 0x100u + word);
    }
    world_store_word(UINT32_C(0x8009c5a0), UINT32_C(0x01123456));
    world_store_word(UINT32_C(0x8009c5a4), UINT32_C(0xe100043e));
    world_store_word(UINT32_C(0x8009c5e8), UINT32_C(0x09123456));

    cpu->gpr[2] = UINT32_C(0x8009c664);
    cpu->gpr[3] = 0x70u;
    cpu->gpr[16] = world_minimap_packet_address(0u);
    cpu->gpr[17] = 0u;
    cpu->gpr[18] = UINT32_C(0x8009a340);
    cpu->gpr[19] = UINT32_C(0x1f800000);
    cpu->gpr[20] = UINT32_C(0x00ffffff);
    cpu->gpr[21] = UINT32_C(0x1f8000f0);
    cpu->gpr[22] = UINT32_C(0xff000000);
    cpu->gpr[23] = UINT32_C(0x8009d55c);
    cpu->gpr[29] = stack_pointer;
    cpu->gpr[31] = UINT32_C(0x80071b84);
    world_store_word(stack_pointer + 0x30u, cpu->gpr[31]);
}

enum {
    MODEL_SHADOW_MEMORY_BASE = 0x8004f000u,
    MODEL_SHADOW_MEMORY_SIZE = 0x71000u,
    MODEL_SHADOW_STACK_BASE = 0x801f0000u,
    MODEL_SHADOW_STACK_SIZE = 0x10000u,
    MODEL_SHADOW_MODEL = 0x80060000u,
    MODEL_SHADOW_VERTICES = 0x80060100u,
    MODEL_SHADOW_TOPOLOGY = 0x80060200u,
    MODEL_SHADOW_MATERIAL = 0x80060300u,
    MODEL_SHADOW_PACKET = 0x8009e000u,
    MODEL_SHADOW_OT = 0x800b0000u,
    MODEL_SHADOW_SP = 0x801f8000u,
};

static uint8_t model_shadow_memory[MODEL_SHADOW_MEMORY_SIZE];
static uint8_t model_shadow_stack[MODEL_SHADOW_STACK_SIZE];
static uint32_t model_shadow_tracked_payload;
static uint32_t model_shadow_payload_read_count;

static uint32_t model_shadow_read_word(uint32_t address) {
    if (model_shadow_tracked_payload != 0u &&
        address >= model_shadow_tracked_payload &&
        address < model_shadow_tracked_payload + 40u)
        ++model_shadow_payload_read_count;
    if (address >= MODEL_SHADOW_MEMORY_BASE &&
        address + 4u <= MODEL_SHADOW_MEMORY_BASE + MODEL_SHADOW_MEMORY_SIZE)
        return particle_load_u32(
            &model_shadow_memory[address - MODEL_SHADOW_MEMORY_BASE]);
    if (address >= MODEL_SHADOW_STACK_BASE &&
        address + 4u <= MODEL_SHADOW_STACK_BASE + MODEL_SHADOW_STACK_SIZE)
        return particle_load_u32(
            &model_shadow_stack[address - MODEL_SHADOW_STACK_BASE]);
    return 0u;
}

static uint16_t model_shadow_read_half(uint32_t address) {
    const uint32_t word = model_shadow_read_word(address & ~3u);

    return (uint16_t)(word >> ((address & 2u) * 8u));
}

static uint8_t model_shadow_read_byte(uint32_t address) {
    const uint32_t word = model_shadow_read_word(address & ~3u);

    return (uint8_t)(word >> ((address & 3u) * 8u));
}

static void model_shadow_store_word(uint32_t address, uint32_t value) {
    if (address >= MODEL_SHADOW_MEMORY_BASE &&
        address + 4u <= MODEL_SHADOW_MEMORY_BASE + MODEL_SHADOW_MEMORY_SIZE)
        particle_store_u32(
            &model_shadow_memory[address - MODEL_SHADOW_MEMORY_BASE], value);
    else if (address >= MODEL_SHADOW_STACK_BASE &&
             address + 4u <= MODEL_SHADOW_STACK_BASE + MODEL_SHADOW_STACK_SIZE)
        particle_store_u32(
            &model_shadow_stack[address - MODEL_SHADOW_STACK_BASE], value);
}

static void configure_model_ft4_shadow_cpu(CPUState *cpu) {
    static const XgHost3dVector vertices[4] = {
        { -64, -64, 0, 0u }, { 64, -64, 0, 0u },
        { -64, 64, 0, 0u }, { 64, 64, 0, 0u },
    };
    const uint32_t matrix_words[8] = {
        pack_s16(4096, 0), pack_s16(0, 0), pack_s16(4096, 0),
        pack_s16(0, 0), pack_s16(4096, 0), 0u, 0u, 512u,
    };
    uint32_t index;

    memset(model_shadow_memory, 0, sizeof(model_shadow_memory));
    memset(model_shadow_stack, 0, sizeof(model_shadow_stack));
    model_shadow_store_word(MODEL_SHADOW_MODEL + 4u, pack_s16(1, 1));
    model_shadow_store_word(MODEL_SHADOW_MODEL + 8u, MODEL_SHADOW_VERTICES);
    model_shadow_store_word(MODEL_SHADOW_MODEL + 0x10u,
                            MODEL_SHADOW_TOPOLOGY);
    model_shadow_store_word(MODEL_SHADOW_MODEL + 0x14u,
                             MODEL_SHADOW_MATERIAL);
    model_shadow_store_word(MODEL_SHADOW_TOPOLOGY,
                            UINT32_C(0x0001000d));
    model_shadow_store_word(MODEL_SHADOW_TOPOLOGY + 4u, pack_s16(0, 1));
    model_shadow_store_word(MODEL_SHADOW_TOPOLOGY + 8u, pack_s16(2, 3));
    model_shadow_store_word(MODEL_SHADOW_MATERIAL,
                            UINT32_C(0xc4000123));
    model_shadow_store_word(MODEL_SHADOW_MATERIAL + 4u,
                            UINT32_C(0xc8000042));
    model_shadow_store_word(MODEL_SHADOW_MATERIAL + 8u,
                            UINT32_C(0x2f808080));
    model_shadow_store_word(MODEL_SHADOW_MATERIAL + 12u,
                            UINT32_C(0x20100000));
    model_shadow_store_word(MODEL_SHADOW_MATERIAL + 16u,
                            UINT32_C(0x40300020));
    for (index = 0u; index < 4u; ++index) {
        model_shadow_store_word(MODEL_SHADOW_VERTICES + index * 8u,
            pack_s16(vertices[index].x, vertices[index].y));
        model_shadow_store_word(MODEL_SHADOW_VERTICES + index * 8u + 4u,
            pack_s16(vertices[index].z, (int16_t)vertices[index].pad));
    }
    for (index = 0u; index < 8u; ++index)
        model_shadow_store_word(
            MODEL_SHADOW_SP + 0x10u + index * 4u, matrix_words[index]);
    model_shadow_store_word(UINT32_C(0x800500f8), 319u);
    model_shadow_store_word(UINT32_C(0x800500fc), UINT32_C(0x00ee0000));
    model_shadow_store_word(UINT32_C(0x80050100), 4u);
    model_shadow_store_word(UINT32_C(0x80050108), 0u);
    model_shadow_store_word(UINT32_C(0x8005010c), 1u);
    model_shadow_store_word(UINT32_C(0x80059308), 0u);
    model_shadow_store_word(UINT32_C(0x8005930c), 0u);
    model_shadow_store_word(UINT32_C(0x80059424), MODEL_SHADOW_PACKET);
    model_shadow_store_word(UINT32_C(0x8005953c), MODEL_SHADOW_VERTICES);
    model_shadow_store_word(UINT32_C(0x80059568), MODEL_SHADOW_OT);
    model_shadow_store_word(UINT32_C(0x80059578), 7u);
    model_shadow_store_word(MODEL_SHADOW_OT + 32u * 4u,
                            UINT32_C(0x00123456));

    memset(cpu, 0, sizeof(*cpu));
    cpu->gpr[4] = MODEL_SHADOW_MODEL;
    cpu->gpr[5] = MODEL_SHADOW_PACKET;
    cpu->gpr[6] = MODEL_SHADOW_OT;
    cpu->gpr[7] = 0u;
    cpu->gpr[29] = MODEL_SHADOW_SP;
    cpu->gpr[31] = UINT32_C(0x800257dc);
    cpu->read_word = model_shadow_read_word;
    cpu->read_half = model_shadow_read_half;
    cpu->read_byte = model_shadow_read_byte;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 256u;
    cpu->gte_ctrl[30] = 1024u;
}

enum {
    SPRITE_SHADOW_INSTANCE = 0x80061000u,
    SPRITE_SHADOW_DATA = 0x80061100u,
    SPRITE_SHADOW_DESCRIPTOR = 0x80061200u,
    SPRITE_SHADOW_VERTICES = 0x8004fb98u,
    SPRITE_SHADOW_PACKET = 0x8009e100u,
};

static void configure_sprite_ft4_shadow_cpu(CPUState *cpu) {
    static const XgHost3dVector vertices[4] = {
        { -32, -48, 0, 0u }, { 32, -48, 0, 0u },
        { -32, 48, 0, 0u }, { 32, 48, 0, 0u },
    };
    uint32_t vertex;

    memset(model_shadow_memory, 0, sizeof(model_shadow_memory));
    memset(model_shadow_stack, 0, sizeof(model_shadow_stack));
    model_shadow_store_word(SPRITE_SHADOW_INSTANCE + 0x20u,
                            SPRITE_SHADOW_DATA);
    model_shadow_store_word(SPRITE_SHADOW_INSTANCE + 0x40u, 4u);
    model_shadow_store_word(SPRITE_SHADOW_DATA + 0x30u,
                            SPRITE_SHADOW_DESCRIPTOR);
    model_shadow_store_word(SPRITE_SHADOW_DESCRIPTOR + 4u,
                            UINT32_C(0x30200000));
    model_shadow_store_word(SPRITE_SHADOW_DESCRIPTOR + 8u,
                            UINT32_C(0x00230000));
    model_shadow_store_word(SPRITE_SHADOW_DESCRIPTOR + 12u,
                            UINT32_C(0x00000042));
    model_shadow_store_word(SPRITE_SHADOW_DESCRIPTOR + 16u,
                            UINT32_C(0x2f808080));
    model_shadow_store_word(SPRITE_SHADOW_DESCRIPTOR + 20u, 1u);
    for (vertex = 0u; vertex < 4u; ++vertex) {
        model_shadow_store_word(SPRITE_SHADOW_VERTICES + vertex * 8u,
            pack_s16(vertices[vertex].x, vertices[vertex].y));
        model_shadow_store_word(SPRITE_SHADOW_VERTICES + vertex * 8u + 4u,
            pack_s16(vertices[vertex].z, (int16_t)vertices[vertex].pad));
    }

    memset(cpu, 0, sizeof(*cpu));
    cpu->gpr[4] = SPRITE_SHADOW_INSTANCE;
    cpu->gpr[31] = UINT32_C(0x800babc4);
    cpu->read_word = model_shadow_read_word;
    cpu->read_half = model_shadow_read_half;
    cpu->read_byte = model_shadow_read_byte;
    cpu->gte_ctrl[0] = pack_s16(4096, 0);
    cpu->gte_ctrl[1] = pack_s16(0, 0);
    cpu->gte_ctrl[2] = pack_s16(4096, 0);
    cpu->gte_ctrl[3] = pack_s16(0, 0);
    cpu->gte_ctrl[4] = 4096u;
    cpu->gte_ctrl[7] = 512u;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 256u;
}

static int configure_host_geometry_cpu_for_actor(CPUState *cpu,
                                                 uint32_t packet_address,
                                                 uint32_t actor_index) {
    static const XgHost3dVector vertices[4] = {
        {24, 0, 24, 0u}, {-24, 0, 24, 0u},
        {24, 0, -24, 0u}, {-24, 0, -24, 0u},
    };
    size_t index;

    memset(cpu, 0, sizeof(*cpu));
    memset(&producer_family_host_input, 0,
           sizeof(producer_family_host_input));
    producer_family_packet_address =
        (packet_address & UINT32_C(0x1fffffff)) | UINT32_C(0x80000000);
    producer_family_model_address = producer_family_packet_address - 0x20u;
    producer_family_actor_index = actor_index;
    producer_family_stack_base = UINT32_C(0x801ff000);
    for (index = 0u; index < 4u; ++index)
        producer_family_host_input.vertices[index] = vertices[index];
    producer_family_host_input.projection.rotation[0][0] = -0xc00;
    producer_family_host_input.projection.rotation[1][1] = 0xc00;
    producer_family_host_input.projection.rotation[2][2] = -0xc00;
    producer_family_host_input.projection.translation[2] = 1000;
    producer_family_host_input.projection.screen_offset_x = 0x00a00000;
    producer_family_host_input.projection.screen_offset_y = 0x00700000;
    producer_family_host_input.projection.projection_distance = 0x80u;
    producer_family_host_input.projection.average_z_scale4 = 0x100;
    if (!xg_host_3d_rot_average4(&producer_family_host_input,
                                  &producer_family_host_output))
        return 0;
    producer_family_packet_tag = UINT32_C(0x09000000);
    producer_family_ot_bucket_address = PRODUCER_FAMILY_OT_BASE +
        producer_family_host_output.ordering_depth * 4u;
    producer_family_ot_word = UINT32_C(0xab123456);
    cpu->gpr[4] = producer_family_model_address;
    cpu->gpr[5] = producer_family_model_address + 8u;
    cpu->gpr[6] = producer_family_model_address + 16u;
    cpu->gpr[7] = producer_family_model_address;
    cpu->gpr[18] = PRODUCER_FAMILY_ACTOR_BASE +
        actor_index * XG_FIELD_CHARACTER_SOURCE_ACTOR_STRIDE;
    cpu->gpr[21] = actor_index;
    cpu->gpr[29] = producer_family_stack_base;
    cpu->read_word = producer_family_read_word;
    cpu->write_word = producer_family_write_word;
    cpu->read_half = producer_family_read_half;
    cpu->gte_ctrl[0] = pack_s16(-0xc00, 0);
    cpu->gte_ctrl[1] = pack_s16(0, 0);
    cpu->gte_ctrl[2] = pack_s16(0xc00, 0);
    cpu->gte_ctrl[3] = pack_s16(0, 0);
    cpu->gte_ctrl[4] = (uint16_t)-0xc00;
    cpu->gte_ctrl[7] = 1000u;
    cpu->gte_ctrl[24] = 0x00a00000u;
    cpu->gte_ctrl[25] = 0x00700000u;
    cpu->gte_ctrl[26] = 0x80u;
    cpu->gte_ctrl[30] = 0x100u;
    return 1;
}

static int configure_host_geometry_cpu(CPUState *cpu,
                                       uint32_t packet_address) {
    return configure_host_geometry_cpu_for_actor(cpu, packet_address, 0u);
}

void gpu_get_draw_state(GpuDrawState *out) {
    memset(out, 0, sizeof(*out));
    out->right = 319u;
    out->bottom = 239u;
}

void gpu_native_environment_get(GpuNativeDrawEnvironment *out) {
    memset(out, 0, sizeof(*out));
    gpu_get_draw_state(&out->draw);
}

int psx_ws_x_margin(void) { return 0; }

uint32_t psx_ws_xclip_bound(uint32_t vanilla) { return vanilla; }

uint64_t gpu_render_vram_mutation_serial(void) {
    return 17u;
}

bool gpu_render_vram_mutation_overflowed(void) {
    return false;
}

int psx_game_identity_equal(const PsxGameIdentity *left,
                            const PsxGameIdentity *right) {
    return left && right && memcmp(left, right, sizeof(*left)) == 0;
}

const PsxGameIdentity *psx_game_identity_runtime(void) {
    return identity_enabled ? &runtime_identity : NULL;
}

int psx_game_identity_bind_static(const PsxGameIdentity *identity) {
    if (!identity || !identity_enabled ||
        memcmp(identity, &runtime_identity, sizeof(*identity)) != 0)
        return 0;
    identity_bound = 1;
    return 1;
}

int psx_game_identity_gate(const PsxGameIdentity *identity) {
    return identity && identity_bound && identity_enabled &&
           memcmp(identity, &runtime_identity, sizeof(*identity)) == 0;
}

static void set_matching_runtime_identity(void) {
    memcpy(runtime_identity.game_sha256, xg_render_game_identity,
           sizeof(runtime_identity.game_sha256));
    memcpy(runtime_identity.manifest_sha256, xg_render_manifest_identity,
           sizeof(runtime_identity.manifest_sha256));
    identity_enabled = 1;
}

static void set_candidate_provenance(PsxXgRenderAuthCandidate *candidate,
                                     uint32_t artifact_base,
                                     uint32_t artifact_size,
                                     uint32_t artifact_crc32) {
    memcpy(&candidate->identity, &runtime_identity, sizeof(candidate->identity));
    candidate->pair_id = UINT64_C(0x1020304050607080);
    candidate->artifact_base = artifact_base;
    candidate->artifact_size = artifact_size;
    candidate->artifact_crc32 = artifact_crc32;
    candidate->authority_provenance = true;
    candidate->pair_bound = true;
}

static void note_matching_candidate(void) {
    PsxXgRenderAuthCandidate candidate = {
        PRODUCER_ENTRY, PRODUCER_ENTRY, RETURN_SITE - PRODUCER_ENTRY,
        PRODUCER_ENTRY,
    };

    set_candidate_provenance(&candidate,
                             xg_render_manifest_validation.field_range_start,
                             xg_render_manifest_validation.field_range_size,
                             xg_render_manifest_validation.field_base_crc32);
    psx_xg_render_auth_note_candidate_dispatch(&candidate);
}

static PsxXgRenderAuthCandidate matching_field5_candidate(void) {
    PsxXgRenderAuthCandidate candidate = {
        FIELD5_PRODUCER_ENTRY & 0x1fffffffu,
        FIELD5_PRODUCER_ENTRY & 0x1fffffffu, 16u,
        FIELD5_PRODUCER_ENTRY & 0x1fffffffu,
    };

    set_candidate_provenance(&candidate, 0x0006f000u, 282628u,
                             UINT32_C(0xb7ce1120));
    return candidate;
}

static void note_matching_field5_candidate(void) {
    const PsxXgRenderAuthCandidate candidate = matching_field5_candidate();

    psx_xg_render_auth_note_candidate_dispatch(&candidate);
}

static int snapshot_is(XgRenderStaticAuthStatus status,
                       GuestRenderRenderMode mode,
                       int selected) {
    XgRenderStaticAuthSnapshot snapshot = {0};

    CHECK(xg_render_static_auth_snapshot(&snapshot) == XG_RENDER_STATIC_AUTH_OK);
    CHECK(snapshot.status == status);
    CHECK(snapshot.effective_render_mode == mode);
    CHECK(snapshot.auth_scene_selected == selected);
    return 1;
}

static int test_identity_mismatch_forces_original(void) {
    identity_enabled = 1;
    runtime_identity.game_sha256[0] = 0u;
    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    return snapshot_is(XG_RENDER_STATIC_AUTH_IDENTITY_MISMATCH,
                       GUEST_RENDER_RENDER_ORIGINAL, 0);
}

static int test_site_and_delay_mismatch_block_proof(void) {
    set_matching_runtime_identity();
    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    CHECK(snapshot_is(XG_RENDER_STATIC_AUTH_TRACE_UNPROVEN,
                      GUEST_RENDER_RENDER_ORIGINAL, 0));
    psx_xg_render_static_auth_capture(CALLER_SITE + 4u, STATIC_CALLEE,
                                      RETURN_SITE, JAL_INSTRUCTION, 0u);
    CHECK(snapshot_is(XG_RENDER_STATIC_AUTH_SITE_MISMATCH,
                      GUEST_RENDER_RENDER_ORIGINAL, 0));
    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    psx_xg_render_static_auth_capture(CALLER_SITE, STATIC_CALLEE,
                                      RETURN_SITE, JAL_INSTRUCTION, 0x08000000u);
    return snapshot_is(XG_RENDER_STATIC_AUTH_DELAY_SLOT_MISMATCH,
                       GUEST_RENDER_RENDER_ORIGINAL, 0);
}

static int test_synthetic_trace_selects_next_scene_only(void) {
    XgRenderStaticAuthTraceSnapshot trace = {0};

    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    CHECK(snapshot_is(XG_RENDER_STATIC_AUTH_TRACE_UNPROVEN,
                      GUEST_RENDER_RENDER_ORIGINAL, 0));
    psx_xg_render_static_auth_capture(CALLER_SITE, STATIC_CALLEE,
                                      RETURN_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_static_auth_return(RETURN_SITE, RETURN_SITE);
    CHECK(snapshot_is(XG_RENDER_STATIC_AUTH_TRACE_UNPROVEN,
                      GUEST_RENDER_RENDER_ORIGINAL, 0));
    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    CHECK(snapshot_is(XG_RENDER_STATIC_AUTH_SELECTED,
                      GUEST_RENDER_RENDER_ORIGINAL, 1));
    psx_xg_render_static_auth_capture(CALLER_SITE, STATIC_CALLEE,
                                      RETURN_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_static_auth_return(RETURN_SITE, RETURN_SITE);
    CHECK(snapshot_is(XG_RENDER_STATIC_AUTH_SELECTED,
                       GUEST_RENDER_RENDER_NATIVE, 1));
    CHECK(xg_render_static_auth_trace_snapshot(&trace) == XG_RENDER_STATIC_AUTH_OK);
    CHECK(trace.count >= 3u);
    CHECK(trace.events[trace.count - 3u].hook == XG_RENDER_STATIC_AUTH_HOOK_ENTRY);
    CHECK(trace.events[trace.count - 2u].caller_site == CALLER_SITE);
    CHECK(trace.events[trace.count - 2u].callee_entry == STATIC_CALLEE);
    CHECK(trace.events[trace.count - 2u].return_site == RETURN_SITE);
    CHECK(trace.events[trace.count - 1u].hook == XG_RENDER_STATIC_AUTH_HOOK_RETURN);
    return 1;
}

static int test_static_auth_rejects_wrong_return_address_before_observation(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot auth_snapshot = {0};
    XgRenderStaticAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    complete_static_auth_trace();
    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    psx_xg_render_static_auth_capture(CALLER_SITE, STATIC_CALLEE,
                                      RETURN_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_static_auth_return(RETURN_SITE, RETURN_SITE + 4u);
    CHECK(xg_render_static_auth_snapshot(&snapshot) == XG_RENDER_STATIC_AUTH_OK);
    CHECK(!snapshot.auth_scene_selected);
    CHECK(!snapshot.trace_proven);
    CHECK(snapshot.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(auth_snapshot.hook_count == 0u);
    CHECK(auth_snapshot.producer_begin_count == 0u);
    return 1;
}

static int test_trace_schema_is_metadata_only(void) {
    XgRenderStaticAuthTraceSnapshot trace = {0};

    CHECK(sizeof(XgRenderStaticAuthTraceEvent) == 32u);
    CHECK(offsetof(XgRenderStaticAuthTraceEvent, sequence) == 0u);
    CHECK(offsetof(XgRenderStaticAuthTraceEvent, hook) == 8u);
    CHECK(offsetof(XgRenderStaticAuthTraceEvent, producer_entry) == 12u);
    CHECK(offsetof(XgRenderStaticAuthTraceEvent, caller_site) == 16u);
    CHECK(offsetof(XgRenderStaticAuthTraceEvent, callee_entry) == 20u);
    CHECK(offsetof(XgRenderStaticAuthTraceEvent, return_site) == 24u);
    CHECK(offsetof(XgRenderStaticAuthTraceEvent, accepted) == 28u);

    set_matching_runtime_identity();
    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    psx_xg_render_static_auth_capture(CALLER_SITE, STATIC_CALLEE,
                                      RETURN_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_static_auth_return(RETURN_SITE, RETURN_SITE);
    CHECK(xg_render_static_auth_trace_snapshot(&trace) == XG_RENDER_STATIC_AUTH_OK);
    CHECK(trace.count >= 3u);
    CHECK(trace.events[trace.count - 2u].accepted);
    CHECK(trace.events[trace.count - 2u].caller_site == CALLER_SITE);
    CHECK(trace.events[trace.count - 2u].callee_entry == STATIC_CALLEE);
    CHECK(trace.events[trace.count - 2u].return_site == RETURN_SITE);
    CHECK(trace.events[trace.count - 1u].accepted);
    return 1;
}

static int runtime_snapshot_is(XgRenderAuthRejectReason reason,
                               size_t producer_begins) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    GuestRenderBridgeSnapshot bridge = {0};

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == reason);
    CHECK(snapshot.producer_begin_count == producer_begins);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(!bridge.producer_open);
    return 1;
}

static int rejection_receipt_is(PsxXgRenderAuthRejectionSource source,
                                int has_hook, PsxXgRenderAuthHook hook,
                                uint32_t guest_pc) {
    PsxXgRenderAuthRejectionReceipt receipt = {0};

    psx_xg_render_auth_rejection_snapshot(&receipt);
    CHECK(receipt.source == source);
    CHECK(receipt.has_hook == has_hook);
    CHECK(!has_hook || receipt.hook == hook);
    CHECK(receipt.guest_pc == guest_pc);
    return 1;
}

static int completed_proof_receipts_equal(
    const PsxXgRenderAuthCompletedProofReceipt *left,
    const PsxXgRenderAuthCompletedProofReceipt *right) {
    CHECK(left->available == right->available);
    CHECK(left->blocked == right->blocked);
    CHECK(left->producer_record_id == right->producer_record_id);
    CHECK(left->site_record_id == right->site_record_id);
    CHECK(left->tuple.producer_entry == right->tuple.producer_entry);
    CHECK(left->tuple.capture_site == right->tuple.capture_site);
    CHECK(left->tuple.static_callee == right->tuple.static_callee);
    CHECK(left->tuple.return_site == right->tuple.return_site);
    CHECK(left->tier == right->tier);
    CHECK(left->state_id.scene_epoch == right->state_id.scene_epoch);
    CHECK(left->state_id.state_sequence == right->state_id.state_sequence);
    CHECK(left->entry_event_sequence == right->entry_event_sequence);
    CHECK(left->capture_event_sequence == right->capture_event_sequence);
    CHECK(left->return_event_sequence == right->return_event_sequence);
    CHECK(left->candidate_matched == right->candidate_matched);
    CHECK(left->candidate_dispatched == right->candidate_dispatched);
    CHECK(left->blocker_reason == right->blocker_reason);
    CHECK(left->blocker_rejection.source == right->blocker_rejection.source);
    CHECK(left->blocker_rejection.hook == right->blocker_rejection.hook);
    CHECK(left->blocker_rejection.guest_pc ==
          right->blocker_rejection.guest_pc);
    CHECK(left->blocker_rejection.has_hook ==
          right->blocker_rejection.has_hook);
    return 1;
}

static int completed_proof_is(
    const PsxXgRenderAuthCompletedProofReceipt *receipt,
    XgRenderAuthTier tier, int candidate_matched, int candidate_dispatched) {
    CHECK(receipt->available);
    CHECK(!receipt->blocked);
    CHECK(receipt->producer_record_id == XG_RENDER_AUTH_PRODUCER_RECORD_ID);
    CHECK(receipt->site_record_id == XG_RENDER_AUTH_SITE_RECORD_ID);
    CHECK(receipt->tuple.producer_entry == PRODUCER_ENTRY);
    CHECK(receipt->tuple.capture_site == CALLER_SITE);
    CHECK(receipt->tuple.static_callee == STATIC_CALLEE);
    CHECK(receipt->tuple.return_site == RETURN_SITE);
    CHECK(receipt->tier == tier);
    CHECK(receipt->state_id.scene_epoch != 0u);
    CHECK(receipt->entry_event_sequence != 0u);
    CHECK(receipt->capture_event_sequence ==
          receipt->entry_event_sequence + 1u);
    CHECK(receipt->return_event_sequence ==
          receipt->capture_event_sequence + 1u);
    CHECK(receipt->candidate_matched == candidate_matched);
    CHECK(receipt->candidate_dispatched == candidate_dispatched);
    CHECK(receipt->blocker_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(receipt->blocker_rejection.source ==
          PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE);
    CHECK(!receipt->blocker_rejection.has_hook);
    CHECK(receipt->blocker_rejection.guest_pc == 0u);
    return 1;
}

static void complete_static_auth_trace(void) {
    psx_xg_render_static_auth_entry(PRODUCER_ENTRY);
    psx_xg_render_static_auth_capture(CALLER_SITE, STATIC_CALLEE,
                                      RETURN_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_static_auth_return(RETURN_SITE, RETURN_SITE);
}

static int test_runtime_completed_proof_receipt_lifecycle(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot before_later_events = {0};
    XgRenderAuthSnapshot after_later_events = {0};
    PsxXgRenderAuthCompletedProofReceipt receipt = {0};
    PsxXgRenderAuthCompletedProofReceipt warm = {0};
    PsxXgRenderAuthCompletedProofReceipt cold = {0};
    const size_t later_proof_count = xg_render_auth_trace_capacity() / 3u + 3u;

    psx_xg_render_auth_completed_proof_snapshot(&receipt);
    CHECK(!receipt.available);
    CHECK(!receipt.blocked);
    CHECK(receipt.blocker_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(receipt.blocker_rejection.source ==
          PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE);

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    psx_xg_render_auth_scene_boundary();
    note_matching_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_completed_proof_snapshot(&receipt);
    CHECK(!receipt.available);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_completed_proof_snapshot(&receipt);
    CHECK(!receipt.available);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    psx_xg_render_auth_completed_proof_snapshot(&warm);
    CHECK(completed_proof_is(&warm, XG_RENDER_AUTH_TIER_WARM_NATIVE, 1, 1));
    CHECK(xg_render_auth_snapshot(auth, &before_later_events) ==
          XG_RENDER_AUTH_OK);
    CHECK(!before_later_events.native_use_permitted);

    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY + 4u, 0u, 0u);
    note_matching_candidate();
    psx_xg_render_auth_note_code_write(7u, 7u, CALLER_SITE, 4u);
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_completed_proof_snapshot(&receipt);
    CHECK(completed_proof_receipts_equal(&receipt, &warm));
    CHECK(xg_render_auth_snapshot(auth, &after_later_events) ==
          XG_RENDER_AUTH_OK);
    CHECK(!after_later_events.native_use_permitted);

    CHECK(xg_render_auth_snapshot(auth, &before_later_events) ==
          XG_RENDER_AUTH_OK);
    for (size_t index = 0u; index < later_proof_count; ++index)
        complete_static_auth_trace();
    CHECK(xg_render_auth_snapshot(auth, &after_later_events) ==
          XG_RENDER_AUTH_OK);
    CHECK(after_later_events.next_trace_sequence -
              before_later_events.next_trace_sequence >
          xg_render_auth_trace_capacity());
    psx_xg_render_auth_completed_proof_snapshot(&receipt);
    CHECK(completed_proof_receipts_equal(&receipt, &warm));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_loader_mismatch(KUSEG_ADDRESS(PRODUCER_ENTRY));
    psx_xg_render_auth_completed_proof_snapshot(&receipt);
    CHECK(receipt.available);
    CHECK(receipt.blocked);
    CHECK(receipt.blocker_reason == XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH);
    CHECK(receipt.blocker_rejection.source ==
          PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH);
    CHECK(!receipt.blocker_rejection.has_hook);
    CHECK(receipt.blocker_rejection.hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY);
    CHECK(receipt.blocker_rejection.guest_pc == PRODUCER_ENTRY);
    CHECK(receipt.return_event_sequence == warm.return_event_sequence);
    CHECK(receipt.state_id.scene_epoch == warm.state_id.scene_epoch);
    CHECK(receipt.state_id.state_sequence == warm.state_id.state_sequence);
    CHECK(receipt.candidate_matched == warm.candidate_matched);
    CHECK(receipt.candidate_dispatched == warm.candidate_dispatched);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_native_bad_entry(PRODUCER_ENTRY, CALLER_SITE);
    psx_xg_render_auth_completed_proof_snapshot(&cold);
    CHECK(completed_proof_receipts_equal(&cold, &receipt));

    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    psx_xg_render_auth_completed_proof_snapshot(&cold);
    CHECK(completed_proof_is(&cold, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                             0, 0));
    CHECK(cold.state_id.scene_epoch > warm.state_id.scene_epoch);
    CHECK(cold.entry_event_sequence > warm.return_event_sequence);

    psx_xg_render_auth_scene_boundary();
    note_matching_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    psx_xg_render_auth_completed_proof_snapshot(&receipt);
    CHECK(completed_proof_is(&receipt, XG_RENDER_AUTH_TIER_WARM_NATIVE, 1, 1));
    CHECK(receipt.state_id.scene_epoch > cold.state_id.scene_epoch);
    CHECK(receipt.entry_event_sequence > cold.return_event_sequence);
    return 1;
}

static int test_runtime_rejection_receipt_latches_first_trigger(void) {
    set_matching_runtime_identity();

    psx_xg_render_auth_scene_boundary();
    CHECK(rejection_receipt_is(PSX_XG_RENDER_AUTH_REJECTION_SOURCE_NONE,
                               0, PSX_XG_RENDER_AUTH_HOOK_ENTRY, 0u));
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE,
                                 FIELD5_CAPTURE_NON_JAL,
                                 FIELD5_CAPTURE_DELAY);
    CHECK(rejection_receipt_is(
        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK, 1,
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, FIELD5_CAPTURE_SITE));
    CHECK(strcmp(psx_xg_render_auth_rejection_source_name(
              PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK),
                 "variant_hook") == 0);
    CHECK(strcmp(psx_xg_render_auth_hook_name(PSX_XG_RENDER_AUTH_HOOK_ENTRY),
                 "producer_entry") == 0);
    psx_xg_render_auth_loader_mismatch(PRODUCER_ENTRY);
    CHECK(rejection_receipt_is(
        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK, 1,
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, FIELD5_CAPTURE_SITE));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_loader_mismatch(KUSEG_ADDRESS(PRODUCER_ENTRY));
    CHECK(rejection_receipt_is(
        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH, 0,
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, PRODUCER_ENTRY));
    CHECK(strcmp(psx_xg_render_auth_rejection_source_name(
              PSX_XG_RENDER_AUTH_REJECTION_SOURCE_LOADER_MISMATCH),
                 "loader_mismatch") == 0);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_direct_hook_rejections_latch_trigger(void) {
    set_matching_runtime_identity();

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0x08000000u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));
    CHECK(rejection_receipt_is(
        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, 1,
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, CALLER_SITE));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(rejection_receipt_is(
        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_RUNTIME_HOOK, 1,
        PSX_XG_RENDER_AUTH_HOOK_RETURN, RETURN_SITE));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_initial_field5_chain_is_armed(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_CAPTURE_SITE, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(!snapshot.native_use_permitted);

    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_CAPTURE_SITE, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(!snapshot.native_use_permitted);

    psx_xg_render_auth_note_code_write(2u, 3u, 0x80075000u, 4u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.hook_count == 0u);
    CHECK(!snapshot.native_use_permitted);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_CAPTURE_SITE, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(!snapshot.native_use_permitted);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_idle_activation_hook_is_relevant(void) {
    PsxXgRenderAuthCandidate candidate = matching_field5_candidate();

    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, PRODUCER_ENTRY,
        UINT32_C(0x27bdff18)));
    psx_xg_render_auth_note_artifact_candidate(&candidate);
    CHECK(psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, PRODUCER_ENTRY,
        UINT32_C(0x27bdff18)));
    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, PRODUCER_ENTRY,
        UINT32_C(0x8fbf001c)));
    psx_xg_render_auth_note_code_write(
        0u, 1u, xg_render_manifest_validation.field_range_start,
        xg_render_manifest_validation.field_range_size);
    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, PRODUCER_ENTRY,
        UINT32_C(0x27bdff18)));
    psx_xg_render_auth_note_artifact_candidate(&candidate);
    CHECK(psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, PRODUCER_ENTRY,
        UINT32_C(0x27bdff18)));
    candidate.artifact_crc32 ^= 1u;
    psx_xg_render_auth_note_artifact_candidate(&candidate);
    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, PRODUCER_ENTRY,
        UINT32_C(0x27bdff18)));
    CHECK(psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, FIELD5_ACTIVATION_SITE,
        FIELD5_ACTIVATION_JAL));
    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, FIELD5_ACTIVATION_SITE,
        FIELD5_ACTIVATION_JAL));
    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, FIELD5_ACTIVATION_SITE + 4u,
        FIELD5_ACTIVATION_DELAY));
    return 1;
}

static int test_cold_ui_draw_ot_observation_is_relevant(void) {
    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    CHECK(psx_xg_render_auth_overlay_cutover_relevant(
        UINT32_C(0x801e927c), UINT32_C(0x27bdffe8)));
    CHECK(psx_xg_render_auth_overlay_cutover_relevant(
        UINT32_C(0x801e92c4), UINT32_C(0x03e00008)));
    CHECK(psx_xg_render_auth_overlay_cutover_relevant(
        UINT32_C(0x801e920c), UINT32_C(0x00a01821)));
    CHECK(psx_xg_render_auth_overlay_cutover_relevant(
        UINT32_C(0x801e7c50), UINT32_C(0x27bdffc0)));
    CHECK(!psx_xg_render_auth_overlay_cutover_relevant(
        UINT32_C(0x801e927c), UINT32_C(0x27bd0000)));
    CHECK(psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION,
        UI_DRAW_OT_SITE, UI_DRAW_OT_JAL));
    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION,
        UI_DRAW_OT_SITE + 4u, UI_DRAW_OT_JAL));
    CHECK(!psx_xg_render_auth_cold_hook_relevant(
        PSX_XG_RENDER_AUTH_HOOK_INTERNAL_OBSERVATION,
        UI_DRAW_OT_SITE, UINT32_C(0x0c0112f5)));
    return 1;
}

static int test_runtime_variant_supersedes_canonical_entry_alias(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_CAPTURE_SITE, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    return 1;
}

static int test_runtime_variant_accepts_entry_at_return_terminal(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_CAPTURE_SITE, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(!snapshot.native_use_permitted);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_variant_rearms_at_new_exact_activation(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY + 4u, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);

    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(!snapshot.native_use_permitted);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_variant_consumes_callee_hook_before_return(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 STATIC_CALLEE, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);

    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(!snapshot.native_use_permitted);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_variant_rejects_wrong_return_address(void) {
    CPUState cpu = {0};

    set_matching_runtime_identity();
    psx_xg_render_auth_scene_boundary();
    note_matching_field5_candidate();
    cpu.gpr[31] = FIELD5_ACTIVATION_RETURN;
    psx_xg_render_auth_warm_hook(&cpu, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(&cpu, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    cpu.gpr[31] = FIELD5_RETURN_SITE;
    psx_xg_render_auth_warm_hook(&cpu, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    cpu.gpr[31] = FIELD5_RETURN_SITE + 4u;
    psx_xg_render_auth_warm_hook(&cpu, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    return runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u);
}

static int test_runtime_variant_rejects_cold_wrong_return_address(void) {
    CPUState cpu = {0};

    set_matching_runtime_identity();
    psx_xg_render_auth_scene_boundary();
    cpu.gpr[31] = FIELD5_ACTIVATION_RETURN;
    test_cold_hook_with_cpu(&cpu, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                            FIELD5_ACTIVATION_SITE, FIELD5_ACTIVATION_JAL,
                            FIELD5_ACTIVATION_DELAY);
    test_cold_hook_with_cpu(&cpu, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                            FIELD5_PRODUCER_ENTRY, 0u, 0u);
    cpu.gpr[31] = FIELD5_RETURN_SITE;
    test_cold_hook_with_cpu(&cpu, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                            FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                            FIELD5_CAPTURE_DELAY);
    cpu.gpr[31] = FIELD5_RETURN_SITE + 4u;
    test_cold_hook_with_cpu(&cpu, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                            FIELD5_RETURN_SITE, 0u, 0u);
    return runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u);
}

static int test_runtime_cold_warm_auth_parity_and_fail_closed_reset(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot cold = {0};
    XgRenderAuthSnapshot warm = {0};
    XgRenderAuthTraceSnapshot trace = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &cold) == XG_RENDER_AUTH_OK);
    CHECK(cold.producer_begin_count == 1u);
    CHECK(cold.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);

    psx_xg_render_auth_scene_boundary();
    note_matching_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &warm) == XG_RENDER_AUTH_OK);
    CHECK(warm.producer_begin_count == 1u);
    CHECK(memcmp(cold.hook_sequence, warm.hook_sequence,
                 sizeof(cold.hook_sequence)) == 0);
    CHECK(cold.logical_identity.producer_entry == warm.logical_identity.producer_entry);
    CHECK(cold.logical_identity.capture_site == warm.logical_identity.capture_site);
    CHECK(cold.logical_identity.return_site == warm.logical_identity.return_site);
    CHECK(xg_render_auth_trace_snapshot(auth, &trace) == XG_RENDER_AUTH_OK);
    CHECK(trace.count >= XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(trace.events[trace.count - 3u].hook == XG_RENDER_AUTH_HOOK_ENTRY);
    CHECK(trace.events[trace.count - 2u].hook == XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
    CHECK(trace.events[trace.count - 1u].hook == XG_RENDER_AUTH_HOOK_RETURN);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0x08000000u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));

    psx_xg_render_auth_scene_boundary();
    note_matching_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR,
                                 xg_render_manifest_validation.field_range_start + 4u,
                                 0u, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR, 1u));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_note_code_write(1u, 2u, CALLER_SITE, 4u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR,
                                 xg_render_manifest_validation.field_range_start + 4u,
                                 0u, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 1u));
    CHECK(rejection_receipt_is(
        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION, 0,
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, CALLER_SITE));

    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &cold) == XG_RENDER_AUTH_OK);
    CHECK(cold.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(!cold.native_use_permitted);
    return 1;
}

static int test_runtime_canonicalizes_field5_physical_chain(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    XgRenderAuthTraceSnapshot trace = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_INTERNAL_CAPTURE, JAL_INSTRUCTION,
                                 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_INTERNAL_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_INTERNAL_RETURN, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_ACTIVATION_RETURN, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_CALLER_RETURN, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!snapshot.native_use_permitted);
    CHECK(snapshot.logical_identity.producer_entry == PRODUCER_ENTRY);
    CHECK(snapshot.logical_identity.capture_site == CALLER_SITE);
    CHECK(snapshot.logical_identity.static_callee == STATIC_CALLEE);
    CHECK(snapshot.logical_identity.return_site == RETURN_SITE);
    CHECK(xg_render_auth_trace_snapshot(auth, &trace) == XG_RENDER_AUTH_OK);
    CHECK(trace.count >= XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(trace.events[trace.count - 3u].producer_entry == PRODUCER_ENTRY);
    CHECK(trace.events[trace.count - 2u].capture_site == CALLER_SITE);
    CHECK(trace.events[trace.count - 2u].static_callee == STATIC_CALLEE);
    CHECK(trace.events[trace.count - 1u].return_site == RETURN_SITE);
    return 1;
}

static int test_runtime_canonicalizes_field5_physical_alias_chain(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    XgRenderAuthTraceSnapshot trace = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, KUSEG_ADDRESS(FIELD5_ACTIVATION_SITE),
        FIELD5_ACTIVATION_JAL, FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, KUSEG_ADDRESS(FIELD5_PRODUCER_ENTRY),
        0u, 0u);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, KUSEG_ADDRESS(FIELD5_INTERNAL_CAPTURE),
        JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_ENTRY, KUSEG_ADDRESS(FIELD5_INTERNAL_ENTRY),
        0u, 0u);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_RETURN, KUSEG_ADDRESS(FIELD5_INTERNAL_RETURN),
        0u, 0u);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_RETURN,
        KUSEG_ADDRESS(FIELD5_ACTIVATION_RETURN), 0u, 0u);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_RETURN, KUSEG_ADDRESS(FIELD5_CALLER_RETURN),
        0u, 0u);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, KUSEG_ADDRESS(FIELD5_CAPTURE_SITE),
        JAL_INSTRUCTION, FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_RETURN, KUSEG_ADDRESS(FIELD5_RETURN_SITE),
        0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!snapshot.native_use_permitted);
    CHECK(snapshot.logical_identity.producer_entry == PRODUCER_ENTRY);
    CHECK(snapshot.logical_identity.capture_site == CALLER_SITE);
    CHECK(snapshot.logical_identity.static_callee == STATIC_CALLEE);
    CHECK(snapshot.logical_identity.return_site == RETURN_SITE);
    CHECK(xg_render_auth_trace_snapshot(auth, &trace) == XG_RENDER_AUTH_OK);
    CHECK(trace.count >= XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(trace.events[trace.count - 3u].producer_entry == PRODUCER_ENTRY);
    CHECK(trace.events[trace.count - 2u].capture_site == CALLER_SITE);
    CHECK(trace.events[trace.count - 2u].static_callee == STATIC_CALLEE);
    CHECK(trace.events[trace.count - 1u].return_site == RETURN_SITE);
    return 1;
}

static int test_runtime_rejects_incomplete_or_mutated_field5_chain(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!snapshot.native_use_permitted);
    CHECK(snapshot.producer_begin_count == 0u);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE, JAL_INSTRUCTION,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!snapshot.native_use_permitted);
    CHECK(snapshot.producer_begin_count == 0u);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!snapshot.native_use_permitted);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, FIELD5_CAPTURE_NON_JAL,
                                 FIELD5_CAPTURE_DELAY);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, FIELD5_CAPTURE_SITE,
        FIELD5_CAPTURE_WRONG_TARGET_JAL, FIELD5_CAPTURE_DELAY);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE + 4u, 0u, 0u);
    return runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u);
}

static int test_runtime_warm_field5_candidate_validation(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    const PsxXgRenderAuthCandidate short_range_candidate = {
        FIELD5_PRODUCER_ENTRY & 0x1fffffffu, 0x0006f000u, 16u,
        FIELD5_PRODUCER_ENTRY & 0x1fffffffu,
    };

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!snapshot.native_use_permitted);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_note_candidate_dispatch(&short_range_candidate);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    return runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u);
}

static int test_runtime_rejects_followup_field5_capture_before_return(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE + 0x10u, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);

    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(snapshot.hook_count == 2u);
    CHECK(snapshot.scene_aborted);
    CHECK(!snapshot.native_use_permitted);
    CHECK(rejection_receipt_is(
        PSX_XG_RENDER_AUTH_REJECTION_SOURCE_VARIANT_HOOK, 1,
        PSX_XG_RENDER_AUTH_HOOK_CAPTURE, FIELD5_CAPTURE_SITE + 0x10u));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_rejects_unbound_field5_candidates(void) {
    XgRenderAuth *auth = NULL;

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    for (uint32_t rejection = 0u; rejection < 11u; rejection++) {
        PsxXgRenderAuthCandidate candidate = matching_field5_candidate();
        switch (rejection) {
        case 0u: candidate.authority_provenance = false; break;
        case 1u: candidate.pair_bound = false; break;
        case 2u: candidate.pair_id = 0u; break;
        case 3u: candidate.identity.game_sha256[0] ^= 1u; break;
        case 4u: candidate.identity.manifest_sha256[0] ^= 1u; break;
        case 5u: candidate.artifact_base += 4u; break;
        case 6u: candidate.artifact_size -= 4u; break;
        case 7u: candidate.artifact_crc32 ^= 1u; break;
        case 8u: candidate.producer_entry += 4u; break;
        case 9u: candidate.dispatch_pc += 4u; break;
        case 10u:
            candidate.range_start = 0x0006f000u;
            candidate.range_size = 16u;
            break;
        }
        psx_xg_render_auth_scene_boundary();
        psx_xg_render_auth_note_candidate_dispatch(&candidate);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                     FIELD5_ACTIVATION_SITE,
                                     FIELD5_ACTIVATION_JAL,
                                     FIELD5_ACTIVATION_DELAY);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                     FIELD5_PRODUCER_ENTRY, 0u, 0u);
        CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 0u));
    }
    return 1;
}

static int test_runtime_ignores_unrelated_watched_writes_before_field5_entry_and_return(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_note_code_write(1u, 2u,
                                       FIELD5_ACTIVATION_SITE + 0x100u, 4u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(!snapshot.native_use_permitted);

    psx_xg_render_auth_scene_boundary();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_note_code_write(2u, 3u,
                                       FIELD5_ACTIVATION_SITE + 0x100u, 4u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(!snapshot.native_use_permitted);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_preserves_pending_field5_candidate_for_protected_pre_activation_write(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    PsxXgRenderAuthProvenance provenance = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    note_matching_field5_candidate();
    psx_xg_render_auth_note_code_write(1u, 2u, PRODUCER_ENTRY, 4u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    psx_xg_render_auth_provenance_snapshot(&provenance);
    CHECK(provenance.candidate_matched);
    CHECK(provenance.candidate_dispatched);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(!snapshot.native_use_permitted);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_rejects_writes_overlapping_protected_auth_ranges(void) {
    XgRenderAuth *auth = NULL;
    const uint32_t canonical_write_addresses[] = {
        KUSEG_ADDRESS(PRODUCER_ENTRY) - 1u,
        CALLER_SITE,
    };
    const uint32_t canonical_write_sizes[] = { 2u, 4u };
    const uint32_t field5_write_addresses[] = {
        FIELD5_ACTIVATION_SITE - 8u,
        FIELD5_PRODUCER_ENTRY,
        KUSEG_ADDRESS(FIELD5_CAPTURE_SITE - 8u) - 1u,
        FIELD5_RETURN_SITE,
        FIELD5_ZOOM_RGB + 4u,
        FIELD5_ZOOM_RENDER + 0x48u,
        FIELD5_ZOOM_INITIALIZER + 0xc4u,
        UINT32_C(0x80078ef8),
        FIELD5_PARTICLE_INITIALIZER + 0x100u,
        FIELD5_PARTICLE_RENDER + 0x200u,
    };
    const uint32_t field5_write_sizes[] = {
        1u, 4u, 2u, 4u, 4u, 4u, 4u, 8u, 4u, 4u,
    };

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    for (size_t index = 0u;
         index < sizeof(canonical_write_addresses) / sizeof(canonical_write_addresses[0]);
         ++index) {
        psx_xg_render_auth_scene_boundary();
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                     PRODUCER_ENTRY, 0u, 0u);
        psx_xg_render_auth_note_code_write(
            index + 1u, index + 2u, canonical_write_addresses[index],
            canonical_write_sizes[index]);
        CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 1u));
        CHECK(rejection_receipt_is(
            PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION, 0,
            PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
            canonical_write_addresses[index] | 0x80000000u));
    }

    for (size_t index = 0u;
         index < sizeof(field5_write_addresses) / sizeof(field5_write_addresses[0]);
         ++index) {
        psx_xg_render_auth_scene_boundary();
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                     FIELD5_ACTIVATION_SITE,
                                     FIELD5_ACTIVATION_JAL,
                                     FIELD5_ACTIVATION_DELAY);
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                     FIELD5_PRODUCER_ENTRY, 0u, 0u);
        psx_xg_render_auth_note_code_write(
            index + 3u, index + 4u, field5_write_addresses[index],
            field5_write_sizes[index]);
        CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 1u));
        CHECK(rejection_receipt_is(
            PSX_XG_RENDER_AUTH_REJECTION_SOURCE_CODE_PAGE_MUTATION, 0,
            PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
            field5_write_addresses[index] | 0x80000000u));
    }

    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_cold_auth_accepts_physical_manifest_aliases(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    XgRenderAuthTraceSnapshot trace = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY & 0x1fffffffu, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE & 0x1fffffffu,
                                 JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE & 0x1fffffffu, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(!snapshot.native_use_permitted);
    CHECK(snapshot.logical_identity.producer_entry == PRODUCER_ENTRY);
    CHECK(snapshot.logical_identity.capture_site == CALLER_SITE);
    CHECK(snapshot.logical_identity.return_site == RETURN_SITE);
    CHECK(snapshot.logical_identity.static_callee == STATIC_CALLEE);
    CHECK(xg_render_auth_trace_snapshot(auth, &trace) == XG_RENDER_AUTH_OK);
    CHECK(trace.count >= XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(trace.events[trace.count - 2u].capture_site == CALLER_SITE);
    CHECK(trace.events[trace.count - 2u].static_callee == STATIC_CALLEE);
    CHECK(trace.events[trace.count - 2u].return_site == RETURN_SITE);
    return 1;
}

static int test_runtime_filters_broad_hooks_and_rearms_at_exact_entry(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    GuestRenderBridgeSnapshot bridge = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);

    psx_xg_render_auth_scene_boundary();
    note_matching_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY + 4u, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE + 4u, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE + 4u, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.hook_count == 1u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!snapshot.native_use_permitted);

    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(!snapshot.native_use_permitted);

    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR,
                                 xg_render_manifest_validation.field_range_start + 4u,
                                 0u, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR, 1u));
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY + 4u, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_FOREIGN_INTERIOR, 1u));
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);

    psx_xg_render_auth_scene_boundary();
    note_matching_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                  CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(!snapshot.native_use_permitted);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    return 1;
}

static int runtime_has_no_authenticated_observations(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    GuestRenderBridgeSnapshot bridge = {0};

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.hook_count == 0u);
    CHECK(snapshot.producer_begin_count == 0u);
    CHECK(!snapshot.native_use_permitted);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
    return 1;
}

static int runtime_has_open_entry_observation(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    GuestRenderBridgeSnapshot bridge = {0};

    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(snapshot.hook_count == 1u);
    CHECK(snapshot.producer_begin_count == 1u);
    CHECK(!snapshot.native_use_permitted);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.state_open);
    CHECK(bridge.producer_open);
    return 1;
}

static int test_source_observation_hooks_preserve_field5_variant_lifecycle(void) {
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot snapshot = {0};
    PsxXgRenderSourceSnapshot source = {0};

    set_matching_runtime_identity();
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_source_reset();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE, FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE,
                                 0x800769E4u, 0x8C630100u, 0x80010100u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT,
                                 0x800769E4u, 0x8C630100u, 0x80010100u);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &snapshot) == XG_RENDER_AUTH_OK);
    CHECK(snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(!snapshot.native_use_permitted);
    CHECK(snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);
    CHECK(snapshot.hook_sequence[0] == XG_RENDER_AUTH_HOOK_ENTRY);
    CHECK(snapshot.hook_sequence[1] == XG_RENDER_AUTH_HOOK_CAPTURE_SITE);
    CHECK(snapshot.hook_sequence[2] == XG_RENDER_AUTH_HOOK_RETURN);
    psx_xg_render_auth_source_snapshot(&source);
    CHECK(!source.blocked);
    CHECK(source.count == 2u);
    CHECK(source.events[0].stage == PSX_XG_RENDER_SOURCE_STAGE_PRE);
    CHECK(source.events[1].stage == PSX_XG_RENDER_SOURCE_STAGE_COMMIT);
    CHECK(source.events[0].pc == 0x800769E4u);
    CHECK(source.events[1].pc == 0x800769E4u);
    CHECK(source.events[0].auxiliary == 0x80010100u);
    CHECK(source.events[1].auxiliary == 0x80010100u);
    return 1;
}

static void begin_field5_source_sequence(XgRenderAuthTier tier) {
    psx_xg_render_auth_source_reset();
    psx_xg_render_auth_scene_boundary();
    if (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE)
        note_matching_field5_candidate();
    if (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE) {
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                     FIELD5_ACTIVATION_SITE,
                                     FIELD5_ACTIVATION_JAL,
                                     FIELD5_ACTIVATION_DELAY);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                     FIELD5_PRODUCER_ENTRY, 0u, 0u);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                     FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                     FIELD5_CAPTURE_DELAY);
    } else {
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                     FIELD5_ACTIVATION_SITE,
                                     FIELD5_ACTIVATION_JAL,
                                     FIELD5_ACTIVATION_DELAY);
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                     FIELD5_PRODUCER_ENTRY, 0u, 0u);
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                     FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                     FIELD5_CAPTURE_DELAY);
    }
}

static uint32_t source_pre_auxiliary(
    const XgRenderRuntimeVariantSourceSite *site, uint32_t index) {
    return site->auxiliary_rule == PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS
        ? 0x80010000u + index * 4u : 0u;
}

static uint32_t source_commit_auxiliary(
    const XgRenderRuntimeVariantSourceSite *site, uint32_t index) {
    if (site->auxiliary_rule == PSX_XG_RENDER_SOURCE_AUXILIARY_RESULT_REGISTER)
        return 0x40000000u + index;
    return source_pre_auxiliary(site, index);
}

static int observe_source_site(XgRenderAuthTier tier,
                               const XgRenderRuntimeVariantSourceSite *site,
                               uint32_t index) {
    const uint32_t pre = source_pre_auxiliary(site, index);
    const uint32_t commit = source_commit_auxiliary(site, index);

    if (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE) {
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE,
                                     site->pc, site->instruction, pre);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT,
                                     site->pc, site->instruction, commit);
        return 1;
    }
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction, pre));
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_COMMIT, site->pc, site->instruction, commit));
    return 1;
}

static int observe_source_site_cpu(
    CPUState *cpu, XgRenderAuthTier tier,
    const XgRenderRuntimeVariantSourceSite *site, uint32_t index) {
    const uint32_t pre = source_pre_auxiliary(site, index);
    const uint32_t commit = source_commit_auxiliary(site, index);

    if (tier == XG_RENDER_AUTH_TIER_WARM_NATIVE) {
        psx_xg_render_auth_warm_hook(cpu, PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE,
                                     site->pc, site->instruction, pre);
        psx_xg_render_auth_warm_hook(cpu, PSX_XG_RENDER_AUTH_HOOK_SOURCE_COMMIT,
                                     site->pc, site->instruction, commit);
        return 1;
    }
    CHECK(psx_xg_render_auth_cold_source_observe_cpu(
        cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        pre));
    CHECK(psx_xg_render_auth_cold_source_observe_cpu(
        cpu, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, site->pc, site->instruction,
        commit));
    return 1;
}

static int source_snapshots_are_equivalent(
    const PsxXgRenderSourceSnapshot *left,
    const PsxXgRenderSourceSnapshot *right) {
    CHECK(left->count == right->count);
    CHECK(left->blocked == right->blocked);
    CHECK(left->overflowed == right->overflowed);
    for (uint32_t index = 0u; index < left->count; ++index) {
        CHECK(left->events[index].sequence == index + 1u);
        CHECK(left->events[index].sequence == right->events[index].sequence);
        CHECK(left->events[index].pc == right->events[index].pc);
        CHECK(left->events[index].operation == right->events[index].operation);
        CHECK(left->events[index].width == right->events[index].width);
        CHECK(left->events[index].stage == right->events[index].stage);
        CHECK(left->events[index].auxiliary == right->events[index].auxiliary);
    }
    return 1;
}

static uint64_t source_collector_access_count(
    const FieldCharacterShadowSummary *summary,
    FieldCharacterShadowAccessKind kind) {
    uint64_t count = 0u;

    for (size_t index = 0u; index < summary->access_count; ++index) {
        if (summary->accesses[index].kind == kind)
            count += summary->accesses[index].count;
    }
    return count;
}

static int source_collector_is_empty(void) {
    FieldCharacterShadowSummary summary = {0};

    psx_xg_render_auth_source_collector_snapshot(&summary);
    CHECK(summary.phase == FIELD_CHARACTER_SHADOW_PHASE_IDLE);
    CHECK(summary.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_NONE);
    CHECK(summary.family_count == 0u);
    CHECK(summary.access_count == 0u);
    CHECK(summary.site_count == 0u);
    CHECK(summary.allocator_effect_count == 0u);
    CHECK(summary.ot_effect_count == 0u);
    return 1;
}

static int test_source_observation_cold_warm_aggregate_parity(void) {
    const XgRenderRuntimeVariantDescriptor *descriptor =
        &xg_render_runtime_variant_descriptors[0];
    PsxXgRenderSourceSnapshot cold = {0};
    PsxXgRenderSourceSnapshot cold_after_boundary = {0};
    PsxXgRenderSourceSnapshot warm = {0};
    FieldCharacterShadowSummary cold_collector = {0};
    FieldCharacterShadowSummary warm_collector = {0};

    set_matching_runtime_identity();
    CHECK(descriptor->source_site_count ==
          XG_RENDER_RUNTIME_VARIANT_SOURCE_SITE_CAP);
    CHECK(descriptor->source_sites[0].operation ==
          XG_RENDER_RUNTIME_VARIANT_SOURCE_SWC2);
    CHECK(descriptor->source_sites[3].operation ==
          XG_RENDER_RUNTIME_VARIANT_SOURCE_CALL);
    CHECK(descriptor->source_sites[3].auxiliary_rule ==
          XG_RENDER_RUNTIME_VARIANT_SOURCE_NONE);
    CHECK(descriptor->source_sites[8].operation ==
          XG_RENDER_RUNTIME_VARIANT_SOURCE_BUCKET);
    CHECK(descriptor->source_sites[8].auxiliary_rule ==
          XG_RENDER_RUNTIME_VARIANT_SOURCE_RESULT_REGISTER);
    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    for (uint32_t index = 0u; index < descriptor->source_site_count; ++index) {
        PsxXgRenderSourceSiteMetadata metadata = {0};
        const XgRenderRuntimeVariantSourceSite *site =
            &descriptor->source_sites[index];

        CHECK(psx_xg_render_auth_source_site_lookup(
            site->pc, site->instruction, &metadata));
        CHECK((uint32_t)metadata.operation == site->operation);
        CHECK((uint32_t)metadata.auxiliary_rule == site->auxiliary_rule);
        CHECK(metadata.width == site->width);
        CHECK(observe_source_site(XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                  site, index));
    }
    psx_xg_render_auth_source_snapshot(&cold);
    CHECK(cold.count == PSX_XG_RENDER_SOURCE_EVENT_CAPACITY);
    CHECK(cold.next_sequence == PSX_XG_RENDER_SOURCE_EVENT_CAPACITY + 1u);
    CHECK(!cold.blocked);
    psx_xg_render_auth_source_collector_snapshot(&cold_collector);
    CHECK(cold_collector.phase == FIELD_CHARACTER_SHADOW_PHASE_COLLECTING);
    CHECK(cold_collector.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_NONE);
    CHECK(cold_collector.family_count == 0u);
    CHECK(cold_collector.site_count == descriptor->source_site_count);
    CHECK(cold_collector.access_count == 12u);
    CHECK(source_collector_access_count(
              &cold_collector, FIELD_CHARACTER_SHADOW_ACCESS_READ) == 7u);
    CHECK(source_collector_access_count(
              &cold_collector, FIELD_CHARACTER_SHADOW_ACCESS_WRITE) == 5u);
    CHECK(cold_collector.allocator_effect_count == 1u);
    CHECK(cold_collector.ot_effect_count == 1u);
    CHECK(cold_collector.gpu_effect_count == 0u);
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_source_snapshot(&cold_after_boundary);
    CHECK(source_snapshots_are_equivalent(&cold, &cold_after_boundary));

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_WARM_NATIVE);
    for (uint32_t index = 0u; index < descriptor->source_site_count; ++index)
        CHECK(observe_source_site(XG_RENDER_AUTH_TIER_WARM_NATIVE,
                                  &descriptor->source_sites[index], index));
    psx_xg_render_auth_source_snapshot(&warm);
    psx_xg_render_auth_source_collector_snapshot(&warm_collector);
    CHECK(source_snapshots_are_equivalent(&cold, &warm));
    CHECK(memcmp(&cold_collector, &warm_collector, sizeof(cold_collector)) == 0);
    return 1;
}

static int test_source_observation_diagnostic_overflow_aggregates(void) {
    const XgRenderRuntimeVariantSourceSite *site =
        &xg_render_runtime_variant_descriptors[0].source_sites[6];
    PsxXgRenderSourceSnapshot source = {0};
    FieldCharacterShadowSummary collector = {0};

    set_matching_runtime_identity();
    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    for (uint32_t index = 0u;
         index <= FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT; ++index)
        CHECK(observe_source_site(XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                  site, 0u));
    psx_xg_render_auth_source_snapshot(&source);
    psx_xg_render_auth_source_collector_snapshot(&collector);
    CHECK(!source.blocked);
    CHECK(source.overflowed);
    CHECK(source.count == PSX_XG_RENDER_SOURCE_EVENT_CAPACITY);
    CHECK(source.next_sequence == PSX_XG_RENDER_SOURCE_EVENT_CAPACITY + 1u);
    CHECK(collector.phase == FIELD_CHARACTER_SHADOW_PHASE_COLLECTING);
    CHECK(collector.blocker == FIELD_CHARACTER_SHADOW_BLOCKER_NONE);
    CHECK(collector.site_count == 1u);
    CHECK(collector.access_count == 1u);
    CHECK(collector.accesses[0].instruction_pc == site->pc);
    CHECK(collector.accesses[0].address == 0x80010000u);
    CHECK(collector.accesses[0].width == 4u);
    CHECK(collector.accesses[0].kind == FIELD_CHARACTER_SHADOW_ACCESS_READ);
    CHECK(collector.accesses[0].count ==
          FIELD_CHARACTER_SHADOW_MINIMUM_FAMILY_COUNT + 1u);
    CHECK(collector.allocator_effect_count == 0u);
    CHECK(collector.ot_effect_count == 0u);
    return 1;
}

static int source_snapshot_is_blocked(int overflowed) {
    PsxXgRenderSourceSnapshot snapshot = {0};

    psx_xg_render_auth_source_snapshot(&snapshot);
    CHECK(snapshot.blocked);
    CHECK(snapshot.overflowed == (overflowed != 0));
    CHECK(snapshot.count <= PSX_XG_RENDER_SOURCE_EVENT_CAPACITY);
    if (overflowed)
        CHECK(snapshot.count == PSX_XG_RENDER_SOURCE_EVENT_CAPACITY);
    return 1;
}

static int test_source_observation_fail_closed_cases(void) {
    const XgRenderRuntimeVariantSourceSite *site =
        &xg_render_runtime_variant_descriptors[0].source_sites[6];
    PsxXgRenderSourceSiteMetadata metadata = {0};
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot auth_snapshot = {0};

    set_matching_runtime_identity();
    psx_xg_render_auth_source_reset();
    psx_xg_render_auth_scene_boundary();
    CHECK(!psx_xg_render_auth_source_site_lookup(
        site->pc, site->instruction, &metadata));
    CHECK(!psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    CHECK(source_snapshot_is_blocked(0));
    CHECK(source_collector_is_empty());

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(!psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_COMMIT, site->pc, site->instruction,
        0x80010100u));
    CHECK(source_snapshot_is_blocked(0));
    CHECK(source_collector_is_empty());

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    CHECK(!psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    CHECK(source_snapshot_is_blocked(0));

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    CHECK(!psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_COMMIT, site->pc, site->instruction ^ 1u,
        0x80010100u));
    CHECK(source_snapshot_is_blocked(0));

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    CHECK(!psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_COMMIT, site->pc, site->instruction,
        0x80010104u));
    CHECK(source_snapshot_is_blocked(0));
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!auth_snapshot.native_use_permitted);
    CHECK(auth_snapshot.hook_count == XG_RENDER_AUTH_HOOK_STAGE_COUNT);

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    psx_xg_render_auth_scene_boundary();
    CHECK(source_snapshot_is_blocked(0));

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    psx_xg_render_auth_loader_mismatch(FIELD5_PRODUCER_ENTRY);
    CHECK(source_snapshot_is_blocked(0));

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(psx_xg_render_auth_cold_source_observe(
        PSX_XG_RENDER_SOURCE_STAGE_PRE, site->pc, site->instruction,
        0x80010100u));
    psx_xg_render_auth_note_code_write(1u, 2u, FIELD5_CAPTURE_SITE, 4u);
    CHECK(source_snapshot_is_blocked(0));

    {
        PsxXgRenderAuthCandidate candidate = matching_field5_candidate();

        candidate.identity.game_sha256[0] ^= 1u;
        psx_xg_render_auth_source_reset();
        psx_xg_render_auth_scene_boundary();
        psx_xg_render_auth_note_candidate_dispatch(&candidate);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                     FIELD5_ACTIVATION_SITE,
                                     FIELD5_ACTIVATION_JAL,
                                     FIELD5_ACTIVATION_DELAY);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                     FIELD5_PRODUCER_ENTRY, 0u, 0u);
        psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_SOURCE_PRE,
                                     site->pc, site->instruction,
                                     0x80010100u);
        CHECK(source_snapshot_is_blocked(0));
    }

    return 1;
}

static int test_runtime_variant_context_mutations_do_not_observe(void) {
    PsxXgRenderAuthCandidate candidate = matching_field5_candidate();

    set_matching_runtime_identity();

    candidate.identity.game_sha256[0] ^= 1u;
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_note_candidate_dispatch(&candidate);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    CHECK(runtime_has_no_authenticated_observations());

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY + 4u, 0u, 0u);
    CHECK(runtime_has_no_authenticated_observations());

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE + 4u,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    CHECK(runtime_has_no_authenticated_observations());

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE + 4u, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    CHECK(runtime_has_open_entry_observation());

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_note_code_write(41u, 42u, FIELD5_CAPTURE_SITE, 4u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_CODE_PAGE_MUTATION, 1u));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE + 4u, 0u, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_FOREIGN_INTERIOR,
                                 FIELD5_CAPTURE_SITE, 0u, 0u);
    CHECK(runtime_snapshot_is(XG_RENDER_AUTH_REJECT_VALIDATION_MISMATCH, 1u));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int capture_ft4_geometry(XgRenderAuthTier tier,
                                PsxXgRenderFt4Geometry *out_geometry) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    CPUState cpu = {0};
    PsxXgRenderFt4GeometrySnapshot snapshot = {0};

    begin_field5_source_sequence(tier);
    psx_xg_render_auth_ft4_geometry_enable(true);
    CHECK(configure_host_geometry_cpu(&cpu, 0x80101000u));
    CHECK(observe_source_site_cpu(&cpu, tier, call_site, 0u));
    cpu.gpr[31] = 0x800769d0u;
    cpu.gpr[8] = 0x80101008u;
    cpu.gpr[9] = 0x80101010u;
    cpu.gpr[10] = 0x80101018u;
    cpu.gte_data[12] = producer_family_xy(
        producer_family_host_output.vertices[0].x,
        producer_family_host_output.vertices[0].y);
    cpu.gte_data[13] = producer_family_xy(
        producer_family_host_output.vertices[1].x,
        producer_family_host_output.vertices[1].y);
    cpu.gte_data[14] = producer_family_xy(
        producer_family_host_output.vertices[2].x,
        producer_family_host_output.vertices[2].y);
    cpu.gte_ctrl[31] = producer_family_host_output.rtpt_flags;
    CHECK(psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, 0x8004a7e8u,
        0xe90c0000u));
    CHECK(psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, 0x8004a7e8u,
        0xe90c0000u));
    cpu.gpr[8] = 0x80101020u;
    cpu.gte_data[14] = producer_family_xy(
        producer_family_host_output.vertices[3].x,
        producer_family_host_output.vertices[3].y);
    cpu.gte_ctrl[31] = producer_family_host_output.rtps_flags;
    CHECK(psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, 0x8004a814u,
        0xe90e0000u));
    CHECK(psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, 0x8004a814u,
        0xe90e0000u));
    psx_xg_render_auth_ft4_geometry_snapshot(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);
    CHECK(!snapshot.overflowed);
    CHECK(snapshot.completed_count == 1u);
    CHECK(snapshot.queued_count == 1u);
    CHECK(psx_xg_render_auth_ft4_geometry_pop(out_geometry));
    CHECK(!psx_xg_render_auth_ft4_geometry_pop(out_geometry));
    return 1;
}

static int reset_source_mode(GuestRenderRenderMode mode) {
    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    psx_xg_render_auth_runtime_test_reset();
    presentation_gate_succeeds = true;
    presentation_gate_saw_closed_state = false;
    return psx_xg_render_auth_configure(GUEST_RENDER_TIMING_NATIVE_59_94, mode,
                                        test_presentation_gate, NULL);
}

static int test_native_stream_authority_starts_at_authenticated_scene(void) {
    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    CHECK(!guest_render_native_stream_enabled());
    set_matching_runtime_identity();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    CHECK(guest_render_native_stream_enabled());
    psx_xg_render_auth_scene_boundary();
    CHECK(guest_render_native_stream_enabled());
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_resident_residual_recaptures_after_producer_write(void) {
    const uint32_t command = UINT32_C(0x000b20e0);
    CPUState cpu = {0};
    GuestRenderNativeStreamCommandIdentity identity = {
        .command_id = command,
        .container_id = command - 4u,
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
        .opcode = 0x62u,
        .word_count = 3u,
    };
    GpuRenderTransactionId visual_id = {0};
    GpuRenderSemantic semantic = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    memset(model_shadow_memory, 0, sizeof(model_shadow_memory));
    model_shadow_store_word(UINT32_C(0x800b2114), UINT32_C(0x00010000));
    model_shadow_store_word(UINT32_C(0x800b20fc), UINT32_C(0x00004000));
    model_shadow_store_word(UINT32_C(0x800b2100), UINT32_C(0x00005000));
    model_shadow_store_word(UINT32_C(0x800b2104), UINT32_C(0x00006000));
    cpu.gpr[5] = 0u;
    cpu.read_word = model_shadow_read_word;
    cpu.read_half = model_shadow_read_half;
    cpu.read_byte = model_shadow_read_byte;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8007da44), UINT32_C(0x27bdff88)));
    psx_xg_render_auth_note_code_write(1u, 2u, command, 1u);
    psx_xg_render_auth_capture_tile_write(
        &cpu, command, UINT32_C(0x8007dbb4), 0x70u);
    psx_xg_render_auth_note_code_write(
        2u, 3u, command + 0x1cu, 4u);
    psx_xg_render_auth_loader_mismatch(FIELD5_CAPTURE_SITE);

    psx_xg_render_auth_scene_boundary();
    guest_render_native_stream_set_enabled(true);
    CHECK(guest_render_native_stream_resolve_active_miss(
        &identity, &visual_id, &semantic));
    CHECK(visual_id.scene_epoch != 0u);
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static void configure_particle_cpu(CPUState *cpu) {
    static const int16_t table[12] = {
        -1, -2, 1, 2, 10, 20, 30, 40, 50, 60, 70, 80,
    };

    memset(cpu, 0, sizeof(*cpu));
    memset(particle_memory, 0, sizeof(particle_memory));
    memset(particle_matrix_words, 0, sizeof(particle_matrix_words));
    memcpy(particle_table, table, sizeof(table));
    particle_matrix_words[0] = pack_s16(4096, 0);
    particle_matrix_words[1] = pack_s16(0, 0);
    particle_matrix_words[2] = pack_s16(4096, 0);
    particle_matrix_words[3] = pack_s16(0, 0);
    particle_matrix_words[4] = pack_s16(4096, 0);
    particle_scale_words[0] = 4096u;
    particle_scale_words[1] = 4096u;
    particle_scale_words[2] = 4096u;
    particle_store_u32(&particle_memory[0x10], 1000u * 4096u);
    particle_store_u16(0x38u, 4096u);
    particle_store_u16(0x3au, 4096u);
    particle_store_u16(0x3cu, 4096u);
    particle_memory[0x48] = 0x40u;
    particle_memory[0x49] = 0x50u;
    particle_memory[0x4a] = 0x60u;
    particle_store_u32(&particle_memory[0x50], UINT32_C(0x09000000));
    particle_store_u32(&particle_memory[0x54], UINT32_C(0x2e000000));
    particle_ot_word = UINT32_C(0xaa123456);
    particle_payload_read_count = 0u;
    particle_buffer_index = 0u;
    particle_composition_selector = 0u;
    cpu->gpr[4] = PARTICLE_BASE;
    cpu->gpr[5] = PARTICLE_MATRIX;
    cpu->gpr[6] = 0u;
    cpu->gpr[7] = 0u;
    cpu->gpr[29] = PARTICLE_STACK;
    cpu->gpr[31] = UINT32_C(0x800aa66c);
    cpu->read_word = particle_read_word;
    cpu->read_half = particle_read_half;
    cpu->read_byte = particle_read_byte;
    cpu->write_word = particle_write_word;
    cpu->write_byte = particle_write_byte;
    cpu->gte_ctrl[0] = pack_s16(4096, 0);
    cpu->gte_ctrl[1] = pack_s16(0, 0);
    cpu->gte_ctrl[2] = pack_s16(4096, 0);
    cpu->gte_ctrl[3] = pack_s16(0, 0);
    cpu->gte_ctrl[4] = 4096u;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 112u << 16u;
    cpu->gte_ctrl[26] = 0x80u;
    cpu->gte_ctrl[30] = 0x100u;
}

static void materialize_particle_source(void) {
    static const int16_t source_x[4] = {32, 0, 32, 0};
    static const int16_t source_y[4] = {64, 64, 0, 0};
    size_t index;

    for (index = 0u; index < 4u; ++index) {
        const uint32_t offset = 0xa0u + (uint32_t)index * 8u;

        particle_store_u16(offset, (uint16_t)source_x[index]);
        particle_store_u16(offset + 2u, (uint16_t)source_y[index]);
        particle_store_u16(offset + 4u, 0u);
    }
}

static int test_native_particle_sidecar_and_cutover(void) {
    CPUState cpu;
    XgRenderIrNativePrimitive particle_primitive;
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    static const uint8_t expected_u[4] = {10, 29, 50, 69};
    static const uint8_t expected_v[4] = {84, 104, 123, 143};
    size_t index;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    configure_particle_cpu(&cpu);
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = 2u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a8eac), UINT32_C(0x27bdff68)));
    materialize_particle_source();
    cpu.gpr[5] = PARTICLE_MATRIX;
    cpu.gpr[6] = 0u;
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a9b54), UINT32_C(0x27bdff38)));
    CHECK(cpu.pc == UINT32_C(0x800aa66c));
    CHECK(particle_payload_read_count == 0u);
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(particle_load_u32(&particle_memory[0x50]) == UINT32_C(0x09123456));
    CHECK(particle_ot_word == UINT32_C(0xaa016050));
    CHECK(cpu.gpr[2] == particle_ot_word);
    CHECK(particle_memory[0x54] == 0x40u &&
          particle_memory[0x55] == 0x50u &&
          particle_memory[0x56] == 0x60u &&
          particle_memory[0x57] == 0x2eu);
    CHECK(producer_family_xy(164, 120) ==
          particle_load_u32(&particle_memory[0x58]));
    CHECK(producer_family_xy(160, 120) ==
          particle_load_u32(&particle_memory[0x60]));
    CHECK(producer_family_xy(164, 112) ==
          particle_load_u32(&particle_memory[0x68]));
    CHECK(producer_family_xy(160, 112) ==
          particle_load_u32(&particle_memory[0x70]));
    CHECK(psx_xg_render_auth_particle_test_primitive(&particle_primitive));
    CHECK(particle_primitive.material.tpage == 0x5fu);
    CHECK(particle_primitive.material.clut_x == 0x100u);
    CHECK(particle_primitive.material.clut_y == 0xf7u);
    CHECK(particle_primitive.material.semi_transparent);
    CHECK(particle_primitive.triangle_count == 2u);
    for (index = 0u; index < 3u; ++index) {
        CHECK(particle_primitive.triangles[0].vertices[index].u ==
              (int32_t)expected_u[index] * 65536);
        CHECK(particle_primitive.triangles[0].vertices[index].v ==
              (int32_t)expected_v[index] * 65536);
        CHECK(particle_primitive.triangles[0].vertices[index].r == 0x40u);
        CHECK(particle_primitive.triangles[0].vertices[index].g == 0x50u);
        CHECK(particle_primitive.triangles[0].vertices[index].b == 0x60u);
    }

    particle_store_u32(&particle_memory[0x08], 100u * 4096u);
    particle_store_u32(&particle_memory[0x78], UINT32_C(0x09000000));
    particle_store_u32(&particle_memory[0x7c], UINT32_C(0x2e000000));
    particle_scale_words[0] = 8192u;
    particle_buffer_index = 1u;
    particle_composition_selector = 3u;
    particle_ot_word = UINT32_C(0xaa654321);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a9b54), UINT32_C(0x27bdff38)));
    CHECK(particle_payload_read_count == 0u);
    CHECK(particle_load_u32(&particle_memory[0x78]) == UINT32_C(0x09654321));
    CHECK(particle_ot_word == UINT32_C(0xaa016078));
    CHECK(psx_xg_render_auth_particle_test_primitive(&particle_primitive));
    CHECK(particle_primitive.triangles[0].vertices[0].x == 189 * 65536);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_particle_requires_authenticated_sidecar(void) {
    CPUState cpu;
    GuestRenderBridgeSnapshot bridge = {0};
    PsxXgRenderProducerFamilySnapshot family = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    configure_particle_cpu(&cpu);

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a9b54), UINT32_C(0x27bdff38)));
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(bridge.fallback_reason == GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    psx_xg_render_auth_producer_family_snapshot(&family);
    CHECK(family.blocked && family.blocker == 42u);
    CHECK(particle_load_u32(&particle_memory[0x50]) == UINT32_C(0x09000000));
    CHECK(particle_ot_word == UINT32_C(0xaa123456));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_particle_respects_effective_original(void) {
    CPUState cpu;
    GuestRenderBridgeSnapshot bridge = {0};
    PsxXgRenderProducerFamilySnapshot family = {0};

    guest_render_bridge_abort_scene(GUEST_RENDER_FALLBACK_FORCED_ORIGINAL);
    psx_xg_render_auth_runtime_test_reset();
    presentation_gate_succeeds = false;
    CHECK(psx_xg_render_auth_configure(
        GUEST_RENDER_TIMING_NATIVE_59_94, GUEST_RENDER_RENDER_NATIVE,
        test_presentation_gate, NULL));
    set_matching_runtime_identity();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    configure_particle_cpu(&cpu);
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = 2u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a8eac), UINT32_C(0x27bdff68)));
    materialize_particle_source();
    cpu.gpr[5] = PARTICLE_MATRIX;
    cpu.gpr[6] = 0u;

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a9b54), UINT32_C(0x27bdff38)));
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(bridge.fallback_reason == GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    psx_xg_render_auth_producer_family_snapshot(&family);
    CHECK(!family.blocked);
    CHECK(particle_load_u32(&particle_memory[0x50]) == UINT32_C(0x09000000));
    CHECK(particle_ot_word == UINT32_C(0xaa123456));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_particle_cold_observer_rejects_tag_mismatch(void) {
    CPUState cpu;
    PsxXgRenderProducerFamilySnapshot family = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    note_matching_field5_candidate();
    configure_particle_cpu(&cpu);
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = 2u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a8eac), UINT32_C(0x27bdff68)));
    materialize_particle_source();
    particle_store_u32(&particle_memory[0x50], UINT32_C(0x08000000));
    cpu.gpr[5] = PARTICLE_MATRIX;
    cpu.gpr[6] = 0u;

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a9b54), UINT32_C(0x27bdff38)));
    psx_xg_render_auth_producer_family_snapshot(&family);
    CHECK(family.blocked && family.blocker == 43u);
    CHECK(particle_payload_read_count == 0u);
    CHECK(particle_load_u32(&particle_memory[0x50]) == UINT32_C(0x08000000));
    CHECK(particle_ot_word == UINT32_C(0xaa123456));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_particle_code_write_invalidates_sidecar(void) {
    CPUState cpu;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    note_matching_field5_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    configure_particle_cpu(&cpu);
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = 2u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a8eac), UINT32_C(0x27bdff68)));
    materialize_particle_source();
    CHECK(psx_xg_render_auth_particle_test_source_present(PARTICLE_BASE));
    psx_xg_render_auth_note_code_write(
        1u, 2u, FIELD5_PARTICLE_INITIALIZER + 4u, 4u);
    CHECK(!psx_xg_render_auth_particle_test_source_present(PARTICLE_BASE));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static void configure_zoom_memory(void) {
    uint32_t buffer;
    uint32_t quad;

    memset(zoom_memory, 0, sizeof(zoom_memory));
    memset(zoom_stack, 0, sizeof(zoom_stack));
    zoom_scale = 0x1000u;
    zoom_buffer_index = 0u;
    zoom_context_address = UINT32_C(0x800b249c);
    zoom_ot_word = UINT32_C(0xaa123456);
    zoom_payload_read_count = 0u;
    zoom_xy_write_count = 0u;
    zoom_field_identity = true;
    for (buffer = 0u; buffer < 2u; ++buffer) {
        for (quad = 0u; quad < 5u; ++quad) {
            const uint32_t ft4 = zoom_ft4_address(quad, buffer);
            const uint32_t draw_mode = zoom_draw_mode_address(quad, buffer);
            const int16_t left = (int16_t)(-80 + (int32_t)quad * 32);
            const int16_t right = (int16_t)(left + 32);
            const uint32_t vertex = UINT32_C(0x800b1404) + quad * 0x20u;

            zoom_store_word(ft4, UINT32_C(0x09000000));
            zoom_store_word(ft4 + 4u, UINT32_C(0x2e808080));
            zoom_store_word(draw_mode, UINT32_C(0x02000000));
            zoom_store_word(draw_mode + 4u, UINT32_C(0xe1000000));
            zoom_store_word(draw_mode + 8u, UINT32_C(0xe2000000));
            zoom_store_word(vertex, pack_s16(left, -56));
            zoom_store_word(vertex + 4u, 0u);
            zoom_store_word(vertex + 8u, pack_s16(right, -56));
            zoom_store_word(vertex + 12u, 0u);
            zoom_store_word(vertex + 16u, pack_s16(left, 56));
            zoom_store_word(vertex + 20u, 0u);
            zoom_store_word(vertex + 24u, pack_s16(right, 56));
            zoom_store_word(vertex + 28u, 0u);
        }
    }
}

static void configure_zoom_cpu_at_cutover(CPUState *cpu) {
    const uint32_t stack_pointer = ZOOM_ENTRY_SP - 0x88u;

    memset(cpu, 0, sizeof(*cpu));
    cpu->read_word = zoom_read_word;
    cpu->write_word = zoom_write_word;
    cpu->gpr[2] = zoom_scale;
    cpu->gpr[29] = stack_pointer;
    cpu->gpr[31] = UINT32_C(0x800a6444);
    zoom_store_word(stack_pointer + 0x28u, pack_s16(4096, 0));
    zoom_store_word(stack_pointer + 0x2cu, pack_s16(0, 0));
    zoom_store_word(stack_pointer + 0x30u, pack_s16(4096, 0));
    zoom_store_word(stack_pointer + 0x34u, pack_s16(0, 0));
    zoom_store_word(stack_pointer + 0x38u, pack_s16(4096, 0));
    zoom_store_word(stack_pointer + 0x3cu, 0u);
    zoom_store_word(stack_pointer + 0x40u, 0u);
    zoom_store_word(stack_pointer + 0x44u, 0u);
    zoom_store_word(stack_pointer + 0x84u, UINT32_C(0x80078f00));
    cpu->gte_ctrl[0] = pack_s16(4096, 0);
    cpu->gte_ctrl[1] = pack_s16(0, 0);
    cpu->gte_ctrl[2] = pack_s16(4096, 0);
    cpu->gte_ctrl[3] = pack_s16(0, 0);
    cpu->gte_ctrl[4] = 4096u;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 112u << 16u;
    cpu->gte_ctrl[26] = 0x80u;
    cpu->gte_ctrl[30] = 0x100u;
}

static int materialize_zoom_source(CPUState *cpu, uint32_t semi_transparent,
                                   uint32_t abr) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->read_word = zoom_read_word;
    cpu->write_word = zoom_write_word;
    cpu->gpr[4] = semi_transparent;
    cpu->gpr[5] = abr;
    cpu->gpr[29] = ZOOM_INIT_ENTRY_SP;
    cpu->gpr[31] = UINT32_C(0x800a5898);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x800a663c), UINT32_C(0x27bdff98)));
    cpu->gpr[29] = ZOOM_INIT_ENTRY_SP - 0x68u;
    zoom_store_word(cpu->gpr[29] + 0x64u, UINT32_C(0x800a5898));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x800a68f0), UINT32_C(0x8fbf0064)));
    return 1;
}

static int test_zoom_opcode_2e_template_contract_is_producer_scoped(void) {
    CPUState cpu;
    PsxXgRenderZoomTemplateContractSnapshot contract = {0};
    uint8_t guest_templates_before[sizeof(zoom_memory)];

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_zoom_memory();
    note_matching_field5_candidate();
    memcpy(guest_templates_before, zoom_memory, sizeof(zoom_memory));

    CHECK(materialize_zoom_source(&cpu, 1u, 1u));
    psx_xg_render_auth_zoom_template_contract_snapshot(&contract);
    CHECK(contract.authenticated);
    CHECK(contract.producer_store_pc == UINT32_C(0x800a6884));
    CHECK(contract.opcode == 0x2eu);
    CHECK(contract.template_count == 5u);
    CHECK(contract.buffer_count == 2u);
    CHECK(contract.generation != 0u);
    CHECK(contract.initializer_begin_count == 1u);
    CHECK(contract.initializer_commit_count == 1u);
    CHECK(contract.initializer_2e_count == 1u);
    CHECK(memcmp(guest_templates_before, zoom_memory, sizeof(zoom_memory)) == 0);
    CHECK(zoom_xy_write_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_overlay_same_instruction_requires_exact_artifact_candidate(void) {
    CPUState cpu = {0};
    PsxXgRenderOverlayFt4Snapshot snapshot = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e927c), UINT32_C(0x27bdffe8));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801cd984), UINT32_C(0x0c07a49f));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e5be4), UINT32_C(0x0c07a49f));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e7cb0), UINT32_C(0x0c07a49f));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e7e5c), UINT32_C(0x27bd0040));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e92c4), UINT32_C(0x03e00008));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e927c), UINT32_C(0x27bd0000));
    psx_xg_render_auth_overlay_ft4_snapshot(&snapshot);
    CHECK(snapshot.producer_entry_count == 0u);
    CHECK(snapshot.producer_return_count == 0u);
    CHECK(snapshot.caller_call_count == 0u);
    CHECK(snapshot.caller_finish_count == 0u);
    CHECK(snapshot.rectangle_helper_count == 0u);
    CHECK(snapshot.static_quad_count == 0u);
    CHECK(snapshot.dynamic_uv_template_count == 0u);
    CHECK(snapshot.rejected_site_count == 0u);
    CHECK(snapshot.last_pc == 0u);
    CHECK(snapshot.substitution_blocker == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_overlay_artifact_authority_is_not_field5_specific(void) {
    PsxXgRenderAuthCandidate candidate = {
        UINT32_C(0x801e927c), UINT32_C(0x801e927c), 0x50u,
        UINT32_C(0x801e927c),
    };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    set_candidate_provenance(&candidate, UINT32_C(0x801b2000), 270340u,
                             UINT32_C(0x12345678));
    CHECK(xg_render_authoritative_overlay_artifact_candidate_matches(
        &candidate));
    CHECK(xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
        &candidate, UINT32_C(0x801e927c)));
    CHECK(!xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
        &candidate, UINT32_C(0x800764b4)));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_overlay_ft4_2c_projected_contract_uses_only_sources(void) {
    CPUState cpu = {0};
    PsxXgRenderOverlayFt4Snapshot snapshot = {0};
    const uint32_t vectors[4] = {
        PROJECTED_EYE, PROJECTED_AT, PROJECTED_MATRIX, PROJECTED_COLORS,
    };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    configure_projected_memory();
    projected_write_byte(PROJECTED_SECOND_OBJECT + 0x7eu, 16u);
    cpu.read_word = projected_read_word;
    cpu.write_word = projected_write_word;
    cpu.read_half = projected_read_half;
    cpu.write_half = projected_write_half;
    cpu.read_byte = projected_read_byte;
    cpu.write_byte = projected_write_byte;
    cpu.gpr[4] = PROJECTED_SECOND_OBJECT;
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = 0u;
    cpu.gpr[7] = 0u;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e7c50), UINT32_C(0x27bdffc0));

    cpu.gpr[4] = PROJECTED_SECOND_OBJECT;
    cpu.gpr[5] = 10u;
    cpu.gpr[6] = 20u;
    cpu.gpr[7] = 3u;
    cpu.gpr[29] = PROJECTED_CALL_SP;
    cpu.gpr[31] = UINT32_C(0x801d2fb4);
    projected_store_word(PROJECTED_CALL_SP + 0x10u, 4u);
    projected_store_word(PROJECTED_CALL_SP + 0x14u, 16u);
    projected_store_word(PROJECTED_CALL_SP + 0x18u, 13u);
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801e920c), UINT32_C(0x00a01821));

    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        projected_store_half(vectors[vertex], (uint16_t)(vertex * 8u));
        projected_store_half(vectors[vertex] + 2u,
                             (uint16_t)(vertex * 4u));
        projected_store_half(vectors[vertex] + 4u, 0x400u);
        projected_store_half(vectors[vertex] + 6u, 0u);
        cpu.gpr[4u + vertex] = vectors[vertex];
    }
    cpu.gpr[29] = PROJECTED_CALL_SP;
    projected_store_word(
        PROJECTED_CALL_SP + 0x10u, PROJECTED_SECOND_OBJECT + 8u);
    cpu.gte_ctrl[0] = pack_s16(4096, 0);
    cpu.gte_ctrl[1] = pack_s16(0, 0);
    cpu.gte_ctrl[2] = pack_s16(4096, 0);
    cpu.gte_ctrl[3] = pack_s16(0, 0);
    cpu.gte_ctrl[4] = 4096u;
    cpu.gte_ctrl[24] = 160u << 16u;
    cpu.gte_ctrl[25] = 112u << 16u;
    cpu.gte_ctrl[26] = 0x80u;
    cpu.gte_ctrl[30] = 0x100u;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801ce23c), UINT32_C(0x0c0129cf));
    CHECK(projected_payload_read_count == 0u);

    cpu.gpr[4] = PROJECTED_OT;
    cpu.gpr[5] = PROJECTED_SECOND_OBJECT;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80043b48), UINT32_C(0x3c0600ff));
    psx_xg_render_auth_overlay_ft4_snapshot(&snapshot);
    CHECK(snapshot.projected_material_count == 2u);
    CHECK(snapshot.rectangle_template_count == 1u);
    CHECK(snapshot.projected_geometry_count == 1u);
    CHECK(snapshot.projected_add_prim_count == 1u);
    CHECK(snapshot.projected_native_count == 1u);
    CHECK(snapshot.projected_stage_failure_count == 0u);
    CHECK(snapshot.substitution_blocker == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_overlay_ft4_2e_projected_contract_uses_only_producers(void) {
    CPUState cpu = {0};
    PsxXgRenderOverlayFt4Snapshot snapshot = {0};
    const uint32_t object = PROJECTED_SECOND_OBJECT;
    const uint32_t packet = object + 0x320u;
    const uint32_t alternate_packet = object + 0x280u;
    const uint32_t narrow_packet = object + 0x1e0u;
    const uint32_t zero_u_packet = object + 0x140u;
    const uint32_t vectors[4] = {
        object + 0x650u, object + 0x658u,
        object + 0x660u, object + 0x668u,
    };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    configure_projected_memory();
    projected_use_overlay_2e = true;
    projected_store_word(PROJECTED_OBJECT + 0x308u, 0u);
    projected_store_word(PROJECTED_OBJECT + 0x364u, object);
    projected_store_word(PROJECTED_OBJECT + 0x4b8u, 0u);
    projected_store_word(PROJECTED_OBJECT + 0x4bcu, 0u);
    projected_store_word(PROJECTED_OBJECT + 0x4c0u, 0u);
    projected_store_word(PROJECTED_OBJECT + 0x4c4u, 0u);
    projected_store_word(PROJECTED_OBJECT + 0x4c8u, 0u);
    cpu.read_word = projected_read_word;
    cpu.read_half = projected_read_half;
    cpu.write_word = projected_write_word;
    cpu.gpr[4] = 0u;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d49d0), UINT32_C(0x27bdffc8));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d4688), UINT32_C(0x27bdffc8));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d433c), UINT32_C(0x27bdffc8));
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d3ff8), UINT32_C(0x27bdffc8));

    for (uint32_t vertex = 0u; vertex < 4u; ++vertex) {
        projected_store_half(vectors[vertex], (uint16_t)(vertex * 8u));
        projected_store_half(vectors[vertex] + 2u,
                             (uint16_t)(vertex * 4u));
        projected_store_half(vectors[vertex] + 4u, 0x400u);
        projected_store_half(vectors[vertex] + 6u, 0u);
        cpu.gpr[4u + vertex] = vectors[vertex];
    }
    cpu.gpr[29] = PROJECTED_CALL_SP;
    projected_store_word(PROJECTED_CALL_SP + 0x10u, packet + 8u);
    cpu.gte_ctrl[0] = pack_s16(4096, 0);
    cpu.gte_ctrl[1] = pack_s16(0, 0);
    cpu.gte_ctrl[2] = pack_s16(4096, 0);
    cpu.gte_ctrl[3] = pack_s16(0, 0);
    cpu.gte_ctrl[4] = 4096u;
    cpu.gte_ctrl[24] = 160u << 16u;
    cpu.gte_ctrl[25] = 112u << 16u;
    cpu.gte_ctrl[26] = 0x80u;
    cpu.gte_ctrl[30] = 0x100u;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d09b0), UINT32_C(0x0c0129cf));
    CHECK(projected_payload_read_count == 0u);

    cpu.gpr[4] = PROJECTED_OT;
    cpu.gpr[5] = packet;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80043b48), UINT32_C(0x3c0600ff));

    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
        cpu.gpr[4u + vertex] = vectors[vertex];
    cpu.gpr[29] = PROJECTED_CALL_SP;
    projected_store_word(
        PROJECTED_CALL_SP + 0x10u, alternate_packet + 8u);
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d09b0), UINT32_C(0x0c0129cf));
    CHECK(projected_payload_read_count == 0u);
    cpu.gpr[4] = PROJECTED_OT;
    cpu.gpr[5] = alternate_packet;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80043b48), UINT32_C(0x3c0600ff));

    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
        cpu.gpr[4u + vertex] = vectors[vertex];
    cpu.gpr[29] = PROJECTED_CALL_SP;
    projected_store_word(PROJECTED_CALL_SP + 0x10u, narrow_packet + 8u);
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d09b0), UINT32_C(0x0c0129cf));
    CHECK(projected_payload_read_count == 0u);
    cpu.gpr[4] = PROJECTED_OT;
    cpu.gpr[5] = narrow_packet;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80043b48), UINT32_C(0x3c0600ff));

    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
        cpu.gpr[4u + vertex] = vectors[vertex];
    cpu.gpr[29] = PROJECTED_CALL_SP;
    projected_store_word(PROJECTED_CALL_SP + 0x10u, zero_u_packet + 8u);
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d09b0), UINT32_C(0x0c0129cf));
    CHECK(projected_payload_read_count == 0u);
    cpu.gpr[4] = PROJECTED_OT;
    cpu.gpr[5] = zero_u_packet;
    (void)psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80043b48), UINT32_C(0x3c0600ff));
    psx_xg_render_auth_overlay_ft4_snapshot(&snapshot);
    CHECK(snapshot.projected_2e_material_count == 8u);
    CHECK(snapshot.projected_2e_geometry_count == 4u);
    CHECK(snapshot.projected_2e_add_prim_count == 4u);
    CHECK(snapshot.projected_2e_native_count == 4u);
    CHECK(snapshot.projected_2e_stage_failure_count == 0u);
    CHECK(snapshot.substitution_blocker == 0u);
    projected_use_overlay_2e = false;
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_overlay_ft4_2e_field_builder_sidecar_is_producer_scoped(void) {
    enum {
        SOURCE = 0x80062000u,
        DESCRIPTOR = 0x80062100u,
        PACKET = 0x8009e200u,
        STACK = 0x801f8100u,
    };
    CPUState cpu = {0};
    PsxXgRenderOverlayFt4Snapshot overlay = {0};
    PsxXgRenderSpriteFt4ShadowSnapshot sprite = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    memset(model_shadow_memory, 0, sizeof(model_shadow_memory));
    memset(model_shadow_stack, 0, sizeof(model_shadow_stack));
    model_shadow_store_word(SOURCE + 4u, DESCRIPTOR - SOURCE);
    model_shadow_store_word(DESCRIPTOR, 1u);
    model_shadow_store_word(DESCRIPTOR + 4u, pack_s16(10, 20));
    model_shadow_store_word(DESCRIPTOR + 8u, pack_s16(16, 24));
    model_shadow_store_word(DESCRIPTOR + 12u, pack_s16(2, 3));
    model_shadow_store_word(DESCRIPTOR + 20u, pack_s16(0, 32));
    model_shadow_store_word(DESCRIPTOR + 24u, pack_s16(1, 0));
    model_shadow_store_word(DESCRIPTOR + 28u, 0u);
    model_shadow_store_word(STACK + 0x10u, 160u);
    model_shadow_store_word(STACK + 0x14u, 150u);
    model_shadow_store_word(STACK + 0x18u, 4096u);

    cpu.gpr[4] = SOURCE;
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = PACKET;
    cpu.gpr[7] = 0u;
    cpu.gpr[29] = STACK;
    cpu.gpr[31] = UINT32_C(0x801e855c);
    cpu.read_word = model_shadow_read_word;
    cpu.read_half = model_shadow_read_half;
    cpu.read_byte = model_shadow_read_byte;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002675c), UINT32_C(0x27bdffb0)));

    model_shadow_store_word(PACKET + 4u, UINT32_C(0x2d808080));
    model_shadow_store_word(PACKET + 8u, pack_s16(162, 153));
    model_shadow_store_word(PACKET + 12u, UINT32_C(0x0042140a));
    model_shadow_store_word(PACKET + 16u, pack_s16(178, 153));
    model_shadow_store_word(PACKET + 20u, UINT32_C(0x0000141a));
    model_shadow_store_word(PACKET + 24u, pack_s16(162, 177));
    model_shadow_store_word(PACKET + 28u, UINT32_C(0x00002c0a));
    model_shadow_store_word(PACKET + 32u, pack_s16(178, 177));
    model_shadow_store_word(PACKET + 36u, UINT32_C(0x00002c1a));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800269cc), UINT32_C(0x8fa90020)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&sprite);
    CHECK(sprite.field_builder_match_count == 1u);
    CHECK(sprite.field_builder_native_primitive_count == 0u);

    for (uint32_t offset = 4u; offset < 40u; offset += 4u)
        model_shadow_store_word(PACKET + offset, UINT32_C(0xa5a5a5a5));
    cpu.gpr[2] = PACKET;
    cpu.gpr[18] = 0x20u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801cef58), UINT32_C(0xa0520006)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&sprite);
    CHECK(sprite.field_builder_template_invalidation_count == 1u);
    CHECK(sprite.field_builder_template_count == 0u);
    cpu.gpr[4] = MODEL_SHADOW_OT;
    cpu.gpr[5] = PACKET;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80043b48), UINT32_C(0x3c0600ff)));
    psx_xg_render_auth_overlay_ft4_snapshot(&overlay);
    CHECK(overlay.field_source_template_count == 1u);
    CHECK(overlay.field_base_template_count == 1u);
    CHECK(overlay.field_offset_template_count == 0u);
    CHECK(overlay.field_material_count == 1u);
    CHECK(overlay.field_add_prim_count == 1u);
    CHECK(overlay.field_native_count == 1u);
    CHECK(overlay.field_stage_failure_count == 0u);
    CHECK(overlay.last_packet == PACKET);
    CHECK(overlay.last_ot == MODEL_SHADOW_OT);
    CHECK(overlay.substitution_blocker == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_sidecar_rejects_address_reuse_across_scene_and_stale_packet(void) {
    enum {
        SOURCE = 0x80062000u,
        DESCRIPTOR = 0x80062100u,
        PACKET = 0x8009e200u,
        STACK = 0x801f8100u,
    };
    CPUState cpu = {0};
    GpuRenderSemantic first = {0};
    GpuRenderSemantic second = {0};
    GuestRenderNativeStreamMissContext miss = {
        .container_id = KUSEG_ADDRESS(PACKET),
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
        .opcode = 0x2du,
        .word_count = 9u,
    };
    GuestRenderNativeStreamSnapshot stream_snapshot = {0};
    PsxXgRenderSpriteFt4ShadowSnapshot sprite = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    note_matching_field5_candidate();
    sidecar_packet_watch_registered = false;
    sidecar_source_watch_registered = false;
    psx_xg_render_auth_register_code_watches(record_render_code_watch);
    guest_render_native_stream_set_enabled(true);
    memset(model_shadow_memory, 0, sizeof(model_shadow_memory));
    memset(model_shadow_stack, 0, sizeof(model_shadow_stack));
    model_shadow_store_word(SOURCE + 4u, DESCRIPTOR - SOURCE);
    model_shadow_store_word(DESCRIPTOR, 1u);
    model_shadow_store_word(DESCRIPTOR + 4u, pack_s16(10, 20));
    model_shadow_store_word(DESCRIPTOR + 8u, pack_s16(16, 24));
    model_shadow_store_word(DESCRIPTOR + 12u, pack_s16(2, 3));
    model_shadow_store_word(DESCRIPTOR + 20u, pack_s16(0, 32));
    model_shadow_store_word(DESCRIPTOR + 24u, pack_s16(1, 0));
    model_shadow_store_word(DESCRIPTOR + 28u, 0u);
    model_shadow_store_word(STACK + 0x10u, 160u);
    model_shadow_store_word(STACK + 0x14u, 150u);
    model_shadow_store_word(STACK + 0x18u, 4096u);
    cpu.gpr[4] = SOURCE;
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = PACKET;
    cpu.gpr[7] = 0u;
    cpu.gpr[29] = STACK;
    cpu.gpr[31] = UINT32_C(0x801cfa14);
    cpu.read_word = model_shadow_read_word;
    cpu.read_half = model_shadow_read_half;
    cpu.read_byte = model_shadow_read_byte;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002675c), UINT32_C(0x27bdffb0)));
    model_shadow_store_word(PACKET + 4u, UINT32_C(0x2d808080));
    model_shadow_store_word(PACKET + 8u, pack_s16(162, 153));
    model_shadow_store_word(PACKET + 12u, UINT32_C(0x0042140a));
    model_shadow_store_word(PACKET + 16u, pack_s16(178, 153));
    model_shadow_store_word(PACKET + 20u, UINT32_C(0x0000141a));
    model_shadow_store_word(PACKET + 24u, pack_s16(162, 177));
    model_shadow_store_word(PACKET + 28u, UINT32_C(0x00002c0a));
    model_shadow_store_word(PACKET + 32u, pack_s16(178, 177));
    model_shadow_store_word(PACKET + 36u, UINT32_C(0x00002c1a));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800269cc), UINT32_C(0x8fa90020)));
    CHECK(sidecar_packet_watch_registered);
    CHECK(sidecar_source_watch_registered);

    CHECK(guest_render_native_stream_snapshot(&stream_snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    miss.visual_id = stream_snapshot.visual_id;
    CHECK(guest_render_native_stream_activate_visual(miss.visual_id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    miss.command_id = KUSEG_ADDRESS(PACKET);
    CHECK(!guest_render_native_stream_resolve_miss(&miss, &first));
    miss.command_id = KUSEG_ADDRESS(PACKET + 4u);
    CHECK(guest_render_native_stream_resolve_miss(&miss, &first));
    note_matching_field5_candidate();
    ++miss.visual_id.state_sequence;
    CHECK(guest_render_native_stream_stage_exact(
              miss.visual_id, miss.command_id, &first) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(miss.visual_id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_resolve_miss(&miss, &second));
    CHECK(memcmp(&first, &second, sizeof(first)) == 0);
    for (uint32_t offset = 4u; offset < 40u; offset += 4u)
        model_shadow_store_word(PACKET + offset, UINT32_C(0xa5a5a5a5));
    psx_xg_render_auth_note_code_write(1u, 2u, PACKET + 4u, 4u);
    CHECK(!guest_render_native_stream_resolve_miss(&miss, &second));
    ++miss.visual_id.scene_epoch;
    miss.visual_id.state_sequence = 0u;
    CHECK(guest_render_native_stream_stage_exact(
              miss.visual_id, miss.command_id, &first) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(guest_render_native_stream_activate_visual(miss.visual_id) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(!guest_render_native_stream_resolve_miss(&miss, &second));
    model_shadow_tracked_payload = PACKET;
    model_shadow_payload_read_count = 0u;
    psx_xg_render_auth_scene_boundary();
    CHECK(!guest_render_native_stream_resolve_miss(&miss, &second));
    model_shadow_tracked_payload = 0u;
    CHECK(model_shadow_payload_read_count == 0u);
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&sprite);
    CHECK(sprite.field_builder_template_capture_count == 1u);
    CHECK(sprite.field_builder_template_update_count == 0u);
    CHECK(sprite.field_builder_template_count == 0u);
    CHECK(sprite.field_builder_dma_replay_primitive_count == 2u);
    psx_xg_render_auth_note_code_write(1u, 2u, UINT32_C(0x8002675c), 4u);
    CHECK(!guest_render_native_stream_resolve_miss(&miss, &second));
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_overlay_ft4_2e_descriptor_templates_do_not_read_packets(void) {
    enum {
        SOURCE = 0x80062000u,
        DESCRIPTOR = 0x80062100u,
        PACKET = 0x8009e200u,
        STACK = 0x801f8100u,
    };
    CPUState cpu = {0};
    PsxXgRenderOverlayFt4Snapshot overlay = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    memset(model_shadow_memory, 0, sizeof(model_shadow_memory));
    memset(model_shadow_stack, 0, sizeof(model_shadow_stack));
    model_shadow_store_word(SOURCE + 4u, DESCRIPTOR - SOURCE);
    model_shadow_store_word(DESCRIPTOR, 1u);
    model_shadow_store_word(DESCRIPTOR + 4u, pack_s16(10, 20));
    model_shadow_store_word(DESCRIPTOR + 8u, pack_s16(16, 24));
    model_shadow_store_word(DESCRIPTOR + 12u, pack_s16(2, 3));
    model_shadow_store_word(DESCRIPTOR + 20u, pack_s16(0, 32));
    model_shadow_store_word(DESCRIPTOR + 24u, pack_s16(1, 0));
    model_shadow_store_word(DESCRIPTOR + 28u, 0u);
    model_shadow_store_word(STACK + 0x10u, 160u);
    model_shadow_store_word(STACK + 0x14u, 150u);
    model_shadow_store_word(STACK + 0x18u, 4096u);
    cpu.gpr[4] = SOURCE;
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = PACKET;
    cpu.gpr[7] = 0u;
    cpu.gpr[29] = STACK;
    cpu.gpr[31] = UINT32_C(0x801d3e30);
    cpu.read_word = model_shadow_read_word;
    cpu.read_half = model_shadow_read_half;
    cpu.read_byte = model_shadow_read_byte;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x801d3db0), UINT32_C(0x27bdffb8)));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002675c), UINT32_C(0x27bdffb0)));
    for (uint32_t offset = 0u; offset < 40u; offset += 4u)
        model_shadow_store_word(PACKET + offset, UINT32_C(0xa5a5a5a5));
    model_shadow_tracked_payload = PACKET;
    model_shadow_payload_read_count = 0u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800269cc), UINT32_C(0x8fa90020)));
    model_shadow_tracked_payload = 0u;
    CHECK(model_shadow_payload_read_count == 0u);
    psx_xg_render_auth_overlay_ft4_snapshot(&overlay);
    CHECK(overlay.projected_2e_material_count == 1u);
    CHECK(overlay.substitution_blocker == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static void configure_projected_memory(void) {
    uint32_t buffer;
    uint32_t strip;
    static const uint8_t colors[12] = {
        0x10u, 0x20u, 0x30u, 0u,
        0x40u, 0x50u, 0x60u, 0u,
        0x70u, 0x80u, 0x90u, 0u,
    };

    memset(projected_memory, 0xa5, sizeof(projected_memory));
    memset(projected_stack, 0, sizeof(projected_stack));
    projected_payload_read_count = 0u;
    projected_packet_write_count = 0u;
    projected_ot_write_count = 0u;
    projected_use_field_ot = false;
    projected_use_overlay_2e = false;
    for (buffer = 0u; buffer < 2u; ++buffer) {
        for (strip = 0u; strip < 8u; ++strip)
            projected_store_word(
                PROJECTED_OBJECT + buffer * 0x140u + strip * 0x28u,
                UINT32_C(0x09000000));
        projected_store_word(
            PROJECTED_OBJECT + 0x280u + buffer * 0x18u,
            UINT32_C(0x05000000));
        projected_store_word(
            PROJECTED_OBJECT + 0x2b0u + buffer * 0x18u,
            UINT32_C(0x05000000));
        projected_store_word(
            PROJECTED_OBJECT + 0x2e0u + buffer * 0x24u,
            UINT32_C(0x08000000));
    }
    projected_store_word(PROJECTED_OBJECT + 0x328u, 0x100u);
    projected_store_word(PROJECTED_OBJECT + 0x32cu, 0x40u);
    projected_store_word(PROJECTED_OBJECT + 0x330u, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x334u, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x336u, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x338u, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x33au, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x33eu, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x340u, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x344u, 1u);
    projected_store_half(PROJECTED_OBJECT + 0x346u, 0x100u);
    projected_store_half(PROJECTED_OBJECT + 0x348u, 0u);
    projected_store_half(PROJECTED_OBJECT + 0x34au, 0u);
    projected_store_half(PROJECTED_EYE, 0u);
    projected_store_half(PROJECTED_EYE + 2u, 0u);
    projected_store_half(PROJECTED_EYE + 4u, 0u);
    projected_store_half(PROJECTED_AT, 0u);
    projected_store_half(PROJECTED_AT + 2u, 0u);
    projected_store_half(PROJECTED_AT + 4u, 1000u);
    projected_store_word(PROJECTED_MATRIX, pack_s16(4096, 0));
    projected_store_word(PROJECTED_MATRIX + 4u, pack_s16(0, 0));
    projected_store_word(PROJECTED_MATRIX + 8u, pack_s16(4096, 0));
    projected_store_word(PROJECTED_MATRIX + 12u, pack_s16(0, 0));
    projected_store_word(PROJECTED_MATRIX + 16u, pack_s16(4096, 0));
    projected_store_word(PROJECTED_MATRIX + 20u, 0u);
    projected_store_word(PROJECTED_MATRIX + 24u, 0u);
    projected_store_word(PROJECTED_MATRIX + 28u, 0u);
    memcpy(&projected_memory[PROJECTED_COLORS - PROJECTED_MEMORY_BASE],
           colors, sizeof(colors));
    projected_store_word(PROJECTED_OT, UINT32_C(0xaa123456));
}

static int materialize_projected_source(CPUState *cpu,
                                        uint32_t object_address) {
    const uint32_t return_address = UINT32_C(0x80123458);

    memset(cpu, 0, sizeof(*cpu));
    cpu->read_word = projected_read_word;
    cpu->write_word = projected_write_word;
    cpu->read_half = projected_read_half;
    cpu->write_half = projected_write_half;
    cpu->read_byte = projected_read_byte;
    cpu->write_byte = projected_write_byte;
    cpu->gpr[29] = PROJECTED_INIT_SP;
    cpu->gpr[31] = return_address;
    projected_store_word(PROJECTED_INIT_SP + 0x10u, 0x10fu);
    projected_store_word(PROJECTED_INIT_SP + 0x14u, 0xf0u);
    projected_store_word(PROJECTED_INIT_SP + 0x24u, PROJECTED_COLORS);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x8002709c), UINT32_C(0x27bdff60)));
    cpu->gpr[29] = PROJECTED_INIT_SP - 0xa0u;
    cpu->gpr[2] = object_address;
    projected_store_word(cpu->gpr[29] + 0x9cu, return_address);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x80027390), UINT32_C(0x8fbf009c)));
    return 1;
}

static void configure_projected_cpu_at_cutover(CPUState *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->read_word = projected_read_word;
    cpu->write_word = projected_write_word;
    cpu->read_half = projected_read_half;
    cpu->write_half = projected_write_half;
    cpu->read_byte = projected_read_byte;
    cpu->write_byte = projected_write_byte;
    cpu->gpr[4] = PROJECTED_OBJECT;
    cpu->gpr[5] = PROJECTED_EYE;
    cpu->gpr[6] = PROJECTED_AT;
    cpu->gpr[7] = PROJECTED_MATRIX;
    cpu->gpr[29] = PROJECTED_CALL_SP;
    cpu->gpr[31] = UINT32_C(0x80123458);
    projected_store_word(PROJECTED_CALL_SP + 0x10u, PROJECTED_OT);
    projected_store_word(PROJECTED_CALL_SP + 0x14u, 0u);
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 112u << 16u;
    cpu->gte_ctrl[26] = 0x80u;
    cpu->gte_ctrl[30] = 0x100u;
}

static int test_native_zoom_unity_sidecar_and_ordering(void) {
    CPUState cpu;
    XgRenderAuth *auth = NULL;
    XgRenderAuthSnapshot auth_snapshot = {0};
    PsxXgRenderProducerFamilySnapshot producer = {0};
    XgRenderIrNativePrimitive primitives[5];
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    uint32_t quad;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_zoom_memory();
    note_matching_field5_candidate();

    CHECK(materialize_zoom_source(&cpu, 1u, 1u));

    zoom_buffer_index = 1u;
    cpu.gpr[4] = 0x44u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a5600), UINT32_C(0x00003021)));
    for (quad = 1u; quad <= 5u; ++quad) {
        cpu.gpr[6] = quad;
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            &cpu, UINT32_C(0x800a5694), UINT32_C(0x28c20005)));
    }
    zoom_buffer_index = 0u;

    cpu.gpr[29] = ZOOM_ENTRY_SP;
    cpu.gpr[31] = UINT32_C(0x80078f00);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6408), UINT32_C(0x27bdff78)));
    configure_zoom_cpu_at_cutover(&cpu);
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6490), UINT32_C(0xafa2004c)));
    CHECK(cpu.pc == UINT32_C(0x800a6608));
    CHECK(cpu.gpr[2] == 0u);
    CHECK(zoom_payload_read_count == 0u);
    CHECK(zoom_xy_write_count == 0u);
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(zoom_ot_word == UINT32_C(0xaa0b120c));
    CHECK((zoom_read_word(zoom_ft4_address(0u, 0u)) & UINT32_C(0x00ffffff)) ==
          UINT32_C(0x00123456));
    CHECK((zoom_read_word(zoom_draw_mode_address(0u, 0u)) &
           UINT32_C(0x00ffffff)) == UINT32_C(0x000b1274));
    CHECK((zoom_read_word(zoom_ft4_address(1u, 0u)) & UINT32_C(0x00ffffff)) ==
          UINT32_C(0x000b11ac));
    CHECK(psx_xg_render_auth_zoom_test_primitives(primitives, 5u) == 5u);
    {
        PsxXgRenderZoomTemplateContractSnapshot contract = {0};
        psx_xg_render_auth_zoom_template_contract_snapshot(&contract);
        CHECK(contract.invocation_count == 1u);
        CHECK(contract.cutover_attempt_count == 1u);
        CHECK(contract.native_invocation_count == 1u);
        CHECK(contract.native_primitive_count == 5u);
        CHECK(contract.rejection_count == 0u);
    }
    for (quad = 0u; quad < 5u; ++quad) {
        CHECK(primitives[quad].material.tpage == 0x13bu + quad);
        CHECK(primitives[quad].material.semi_transparent);
        CHECK(!primitives[quad].material.dither);
        CHECK(primitives[quad].material.texture_window_mask_x == 0u);
        CHECK(primitives[quad].triangles[0].vertices[0].x ==
              (int32_t)(quad * 64u) * 65536);
        CHECK(primitives[quad].triangles[0].vertices[0].y == 0);
        CHECK(primitives[quad].triangles[0].vertices[0].r == 0x44u);
        CHECK(primitives[quad].triangles[0].vertices[0].g == 0x44u);
        CHECK(primitives[quad].triangles[0].vertices[0].b == 0x44u);
    }
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(auth_snapshot.ir_usable);
    CHECK(!auth_snapshot.scene_aborted);

    cpu.gpr[29] = ZOOM_ENTRY_SP;
    cpu.gpr[31] = UINT32_C(0x80078f00);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6408), UINT32_C(0x27bdff78)));
    configure_zoom_cpu_at_cutover(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6490), UINT32_C(0xafa2004c)));
    psx_xg_render_auth_producer_family_snapshot(&producer);
    CHECK(!producer.blocked);
    CHECK(producer.blocker == 0u);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(auth_snapshot.ir_usable);
    CHECK(!auth_snapshot.scene_aborted);
    zoom_payload_read_count = 0u;
    psx_xg_render_auth_scene_boundary();
    {
        PsxXgRenderZoomTemplateContractSnapshot contract = {0};
        psx_xg_render_auth_zoom_template_contract_snapshot(&contract);
        CHECK(contract.native_invocation_count == 2u);
        CHECK(contract.native_primitive_count == 10u);
        CHECK(contract.replay_invocation_count == 0u);
        CHECK(contract.replay_primitive_count == 0u);
    }
    return 1;
}

static int test_native_zoom_ignores_foreign_overlay_alias(void) {
    CPUState cpu;
    GuestRenderBridgeSnapshot before = {0};
    GuestRenderBridgeSnapshot after = {0};
    PsxXgRenderProducerFamilySnapshot producer = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_zoom_memory();
    zoom_field_identity = false;
    configure_zoom_cpu_at_cutover(&cpu);
    CHECK(guest_render_bridge_snapshot(&before) == GUEST_RENDER_OK);

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6490), UINT32_C(0xafa2004c)));
    psx_xg_render_auth_producer_family_snapshot(&producer);
    CHECK(!producer.blocked);
    CHECK(producer.blocker == 0u);
    CHECK(guest_render_bridge_snapshot(&after) == GUEST_RENDER_OK);
    CHECK(after.modes.effective_render_mode ==
          before.modes.effective_render_mode);
    CHECK(after.fallback_reason == before.fallback_reason);
    CHECK(after.fallback_count == before.fallback_count);
    return 1;
}

static int test_native_zoom_projects_without_packet_sources(void) {
    CPUState cpu;
    XgRenderIrNativePrimitive primitives[5];
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    const uint32_t first_packet = zoom_ft4_address(0u, 1u);
    int16_t first_x;
    int16_t first_y;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_zoom_memory();
    note_matching_field5_candidate();

    CHECK(materialize_zoom_source(&cpu, 0u, 0u));

    zoom_scale = 0x800u;
    zoom_buffer_index = 1u;
    zoom_context_address = UINT32_C(0x800b249c) + 0x80f4u;
    zoom_ot_word = UINT32_C(0xbb654321);
    cpu.gpr[29] = ZOOM_ENTRY_SP;
    cpu.gpr[31] = UINT32_C(0x80078f00);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6408), UINT32_C(0x27bdff78)));
    configure_zoom_cpu_at_cutover(&cpu);
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6490), UINT32_C(0xafa2004c)));
    CHECK(zoom_payload_read_count == 0u);
    CHECK(zoom_xy_write_count == 20u);
    CHECK(zoom_ot_word == UINT32_C(0xbb0b1218));
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(psx_xg_render_auth_zoom_test_primitives(primitives, 5u) == 5u);
    first_x = (int16_t)(primitives[0].triangles[0].vertices[0].x / 65536);
    first_y = (int16_t)(primitives[0].triangles[0].vertices[0].y / 65536);
    CHECK(zoom_read_word(first_packet + 8u) ==
          producer_family_xy(first_x, first_y));
    CHECK(primitives[0].material.tpage == 0x11bu);
    CHECK(!primitives[0].material.semi_transparent);
    CHECK(primitives[0].triangles[0].vertices[0].r == 0x80u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_zoom_requires_authenticated_initializer(void) {
    CPUState cpu;
    XgRenderIrNativePrimitive primitives[5];
    uint32_t initial_ot;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_zoom_memory();
    note_matching_field5_candidate();
    memset(&cpu, 0, sizeof(cpu));
    cpu.read_word = zoom_read_word;
    cpu.write_word = zoom_write_word;
    cpu.gpr[29] = ZOOM_ENTRY_SP;
    cpu.gpr[31] = UINT32_C(0x80078f00);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6408), UINT32_C(0x27bdff78)));
    configure_zoom_cpu_at_cutover(&cpu);
    initial_ot = zoom_ot_word;
    zoom_payload_read_count = 0u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6490), UINT32_C(0xafa2004c)));
    CHECK(zoom_ot_word == initial_ot);
    CHECK(zoom_xy_write_count == 0u);
    CHECK(zoom_payload_read_count == 0u);
    CHECK(psx_xg_render_auth_zoom_test_primitives(primitives, 5u) == 0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_zoom_memory();
    note_matching_field5_candidate();
    CHECK(materialize_zoom_source(&cpu, 1u, 1u));
    cpu.gpr[29] = ZOOM_ENTRY_SP;
    cpu.gpr[31] = UINT32_C(0x80078f04);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6408), UINT32_C(0x27bdff78)));
    configure_zoom_cpu_at_cutover(&cpu);
    initial_ot = zoom_ot_word;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6490), UINT32_C(0xafa2004c)));
    CHECK(zoom_ot_word == initial_ot);
    CHECK(zoom_xy_write_count == 0u);
    CHECK(zoom_payload_read_count == 0u);
    CHECK(psx_xg_render_auth_zoom_test_primitives(primitives, 5u) == 0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_zoom_memory();
    note_matching_field5_candidate();
    CHECK(materialize_zoom_source(&cpu, 0u, 0u));
    cpu.gpr[29] = ZOOM_ENTRY_SP;
    cpu.gpr[31] = UINT32_C(0x80078f00);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6408), UINT32_C(0x27bdff78)));
    configure_zoom_cpu_at_cutover(&cpu);
    zoom_store_word(zoom_ft4_address(0u, 0u), UINT32_C(0x08000000));
    initial_ot = zoom_ot_word;
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800a6490), UINT32_C(0xafa2004c)));
    CHECK(zoom_ot_word != initial_ot);
    CHECK(zoom_xy_write_count == 0u);
    CHECK(zoom_payload_read_count == 0u);
    CHECK(psx_xg_render_auth_zoom_test_primitives(primitives, 5u) == 5u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_projected_effect_is_resident_and_packet_free(void) {
    CPUState cpu;
    XgRenderIrNativePrimitive primitives[11];
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    uint64_t muldiv_ts_before;
    uint64_t gte_ts_before;
    uint32_t hi_before;
    uint32_t lo_before;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    CHECK(psx_xg_render_auth_configure_native_view(false, 4u, 3u, 320u, 240u));
    configure_projected_memory();
    CHECK(materialize_projected_source(&cpu, PROJECTED_OBJECT));
    memcpy(&projected_memory[PROJECTED_SECOND_OBJECT - PROJECTED_MEMORY_BASE],
           &projected_memory[PROJECTED_OBJECT - PROJECTED_MEMORY_BASE], 0x34cu);
    CHECK(materialize_projected_source(&cpu, PROJECTED_SECOND_OBJECT));
    configure_projected_cpu_at_cutover(&cpu);
    cpu.hi = UINT32_C(0x12345678);
    cpu.lo = UINT32_C(0x9abcdef0);
    cpu.muldiv_ts_done = UINT64_C(0x1122334455667788);
    cpu.gte_ts_done = UINT64_C(0x8877665544332211);
    hi_before = cpu.hi;
    lo_before = cpu.lo;
    muldiv_ts_before = cpu.muldiv_ts_done;
    gte_ts_before = cpu.gte_ts_done;
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800273c4), UINT32_C(0x27bdff80)));
    CHECK(cpu.pc == UINT32_C(0x80123458));
    CHECK(cpu.gpr[2] == 112u);
    CHECK(projected_payload_read_count == 0u);
    CHECK(projected_packet_write_count != 0u);
    CHECK(projected_ot_write_count == 6u);
    CHECK(projected_read_word(PROJECTED_OT) == UINT32_C(0xaa0b42b0));
    CHECK((projected_read_word(PROJECTED_OBJECT + 0x2b0u) &
           UINT32_C(0x00ffffff)) == UINT32_C(0x000b42e0));
    CHECK((projected_read_word(PROJECTED_OBJECT + 0x2e0u) &
           UINT32_C(0x00ffffff)) == UINT32_C(0x000b4280));
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(cpu.hi == hi_before);
    CHECK(cpu.lo == lo_before);
    CHECK(cpu.muldiv_ts_done == muldiv_ts_before);
    CHECK(cpu.gte_ts_done == gte_ts_before);
    CHECK(psx_xg_render_auth_projected_test_primitives(primitives, 11u) == 6u);
    CHECK(primitives[0].material.textured);
    CHECK(primitives[0].material.raw_texture);
    CHECK(primitives[0].material.clut_x == 0x100u);
    CHECK(primitives[0].material.clut_y == 0xf0u);
    CHECK(primitives[0].triangles[0].vertices[0].x == 0);
    CHECK(primitives[0].triangles[0].vertices[0].y == 48 * 65536);
    CHECK(primitives[3].material.shading == XG_RENDER_IR_SHADING_FLAT);
    CHECK(!primitives[3].material.textured);
    CHECK(primitives[3].triangles[0].vertices[0].r == 0x10u);
    CHECK(primitives[4].material.shading == XG_RENDER_IR_SHADING_GOURAUD);
    CHECK(primitives[4].triangles[0].vertices[0].r == 0x40u);
    CHECK(primitives[4].triangles[0].vertices[2].r == 0x70u);
    CHECK(primitives[5].triangles[0].vertices[0].r == 0x70u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 6u);
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 6u);
    CHECK(psx_xg_render_auth_projected_test_source_present(PROJECTED_OBJECT));
    psx_xg_render_auth_scene_boundary();
    CHECK(psx_xg_render_auth_projected_test_source_present(PROJECTED_OBJECT));
    CHECK(psx_xg_render_auth_configure_native_view(false, 4u, 3u, 320u, 240u));
    return 1;
}

static int test_native_view_preserves_guest_projected_background_path(void) {
    CPUState cpu;
    XgRenderIrNativePrimitive primitives[11];
    uint32_t initial_ot;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    CHECK(psx_xg_render_auth_configure_native_view(true, 16u, 9u, 320u, 240u));
    configure_projected_memory();
    CHECK(materialize_projected_source(&cpu, PROJECTED_OBJECT));
    configure_projected_cpu_at_cutover(&cpu);
    initial_ot = projected_read_word(PROJECTED_OT);

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800273c4), UINT32_C(0x27bdff80)));
    CHECK(projected_read_word(PROJECTED_OT) == initial_ot);
    CHECK(projected_packet_write_count == 0u);
    CHECK(projected_ot_write_count == 0u);
    CHECK(psx_xg_render_auth_projected_test_primitives(primitives, 11u) == 0u);
    CHECK(psx_xg_render_auth_configure_native_view(false, 4u, 3u, 320u, 240u));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_projected_effect_fails_closed_without_source_or_tag(void) {
    CPUState cpu;
    XgRenderIrNativePrimitive primitives[11];
    uint32_t initial_ot;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_projected_memory();
    configure_projected_cpu_at_cutover(&cpu);
    initial_ot = projected_read_word(PROJECTED_OT);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800273c4), UINT32_C(0x27bdff80)));
    CHECK(projected_read_word(PROJECTED_OT) == initial_ot);
    CHECK(projected_packet_write_count == 0u);
    CHECK(projected_ot_write_count == 0u);
    CHECK(psx_xg_render_auth_projected_test_primitives(primitives, 11u) == 0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_projected_memory();
    CHECK(materialize_projected_source(&cpu, PROJECTED_OBJECT));
    projected_store_word(PROJECTED_OBJECT, UINT32_C(0x08000000));
    configure_projected_cpu_at_cutover(&cpu);
    initial_ot = projected_read_word(PROJECTED_OT);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800273c4), UINT32_C(0x27bdff80)));
    CHECK(projected_read_word(PROJECTED_OT) == initial_ot);
    CHECK(projected_packet_write_count == 0u);
    CHECK(projected_ot_write_count == 0u);
    CHECK(projected_payload_read_count == 0u);
    CHECK(psx_xg_render_auth_projected_test_primitives(primitives, 11u) == 0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_projected_memory();
    CHECK(materialize_projected_source(&cpu, PROJECTED_OBJECT));
    projected_store_half(PROJECTED_OBJECT + 0x336u, (uint16_t)-256);
    configure_projected_cpu_at_cutover(&cpu);
    initial_ot = projected_read_word(PROJECTED_OT);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800273c4), UINT32_C(0x27bdff80)));
    CHECK(projected_read_word(PROJECTED_OT) == initial_ot);
    CHECK(projected_packet_write_count == 0u);
    CHECK(projected_ot_write_count == 0u);
    CHECK(projected_payload_read_count == 0u);
    CHECK(psx_xg_render_auth_projected_test_primitives(primitives, 11u) == 0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_projected_memory();
    CHECK(materialize_projected_source(&cpu, PROJECTED_OBJECT));
    configure_projected_cpu_at_cutover(&cpu);
    projected_store_word(PROJECTED_CALL_SP + 0x10u, PROJECTED_OT + 4u);
    initial_ot = projected_read_word(PROJECTED_OT);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800273c4), UINT32_C(0x27bdff80)));
    CHECK(projected_read_word(PROJECTED_OT) == initial_ot);
    CHECK(projected_packet_write_count == 0u);
    CHECK(projected_ot_write_count == 0u);
    CHECK(projected_payload_read_count == 0u);
    CHECK(psx_xg_render_auth_projected_test_primitives(primitives, 11u) == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_projected_effect_resolves_field_ot_bucket(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_projected_memory();
    CHECK(materialize_projected_source(&cpu, PROJECTED_OBJECT));
    projected_use_field_ot = true;
    configure_projected_cpu_at_cutover(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800273c4), UINT32_C(0x27bdff80)));
    CHECK(projected_ot_write_count == 6u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_projected_source_map_evicts_oldest_without_blocking(void) {
    CPUState cpu;
    uint32_t index;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_projected_memory();
    for (index = 0u; index < 17u; ++index)
        CHECK(materialize_projected_source(
            &cpu, UINT32_C(0x80010000) + index * 0x400u));
    CHECK(!psx_xg_render_auth_projected_test_source_present(
        UINT32_C(0x80010000)));
    CHECK(psx_xg_render_auth_projected_test_source_present(
        UINT32_C(0x80010000) + 16u * 0x400u));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_sky_cutover_is_packet_free(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    uint32_t hi_before;
    uint32_t lo_before;
    uint64_t muldiv_ts_before;
    uint64_t gte_ts_before;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_sky_cpu(&cpu);
    memset(cpu.gte_data, 0x5a, sizeof(cpu.gte_data));
    memset(cpu.gte_ctrl, 0xa5, sizeof(cpu.gte_ctrl));
    cpu.hi = UINT32_C(0x12345678);
    cpu.lo = UINT32_C(0x9abcdef0);
    cpu.muldiv_ts_done = UINT64_C(0x0123456789abcdef);
    cpu.gte_ts_done = UINT64_C(0xfedcba9876543210);
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));
    hi_before = cpu.hi;
    lo_before = cpu.lo;
    muldiv_ts_before = cpu.muldiv_ts_done;
    gte_ts_before = cpu.gte_ts_done;

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800737ec), UINT32_C(0x27bdffb8)));
    CHECK(cpu.pc == UINT32_C(0x80071b60));
    CHECK(world_payload_read_count == 0u);
    CHECK(world_tag_write_count == 3u);
    CHECK(world_xy_write_count == 12u);
    CHECK(world_ot_write_count == 3u);
    CHECK(world_scratch_write_count == 20u);
    CHECK(world_read_word(world_packet_address(0u) + 8u) ==
          pack_s16(-105, -18));
    CHECK(world_read_word(world_packet_address(2u) + 32u) ==
          pack_s16(438, -49));
    CHECK((int32_t)world_read_word(UINT32_C(0x1f80004c)) < 0);
    CHECK(cpu.gpr[2] == world_read_word(UINT32_C(0x1f800048)));
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(cpu.hi == hi_before);
    CHECK(cpu.lo == lo_before);
    CHECK(cpu.muldiv_ts_done == muldiv_ts_before);
    CHECK(cpu.gte_ts_done == gte_ts_before);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 3u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 3u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_sky_fails_closed_and_aborts_on_mutation(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_sky_cpu(&cpu);
    world_store_word(world_packet_address(0u), UINT32_C(0x07000000));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800737ec), UINT32_C(0x27bdffb8)));
    CHECK(world_tag_write_count == 0u);
    CHECK(world_xy_write_count == 0u);
    CHECK(world_ot_write_count == 0u);
    CHECK(world_scratch_write_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_sky_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800737ec), UINT32_C(0x27bdffb8)));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 3u);
    psx_xg_render_auth_note_code_write(
        1u, 2u, UINT32_C(0x800737ec), 4u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_effects_cutover_is_atomic_and_value_only(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    PsxXgRenderWorldEffectsShadowSnapshot effects = { 0 };
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    uint32_t hi_before;
    uint32_t lo_before;
    uint64_t muldiv_ts_before;
    uint64_t gte_ts_before;
    const uint32_t packet = world_effects_packet_address(0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_effects_cpu(&cpu);
    memset(cpu.gte_data, 0x5a, sizeof(cpu.gte_data));
    cpu.hi = UINT32_C(0x12345678);
    cpu.lo = UINT32_C(0x9abcdef0);
    cpu.muldiv_ts_done = UINT64_C(0x0123456789abcdef);
    cpu.gte_ts_done = UINT64_C(0xfedcba9876543210);
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));
    hi_before = cpu.hi;
    lo_before = cpu.lo;
    muldiv_ts_before = cpu.muldiv_ts_done;
    gte_ts_before = cpu.gte_ts_done;

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80089c78), UINT32_C(0x27bdffb0)));
    CHECK(cpu.pc == UINT32_C(0x80071aa8));
    CHECK(world_payload_read_count == 0u);
    CHECK(world_effects_packet_write_count == 10u);
    CHECK(world_ot_write_count == 1u);
    CHECK(world_scratch_write_count == 0u);
    CHECK(world_read_word(packet) == UINT32_C(0x09123456));
    CHECK(world_read_word(packet + 4u) == UINT32_C(0x2e302010));
    CHECK(world_read_word(packet + 8u) == pack_s16(144, 104));
    CHECK(world_read_word(packet + 12u) == UINT32_C(0x7fd00000));
    CHECK(world_read_word(packet + 16u) == pack_s16(176, 104));
    CHECK(world_read_word(packet + 20u) == UINT32_C(0x0123003f));
    CHECK(world_read_word(packet + 24u) == pack_s16(144, 136));
    CHECK(world_read_word(packet + 28u) == UINT32_C(0x00003f00));
    CHECK(world_read_word(packet + 32u) == pack_s16(176, 136));
    CHECK(world_read_word(packet + 36u) == UINT32_C(0x00003f3f));
    CHECK(world_read_word(WORLD_OT_BASE + 32u * 4u) ==
          UINT32_C(0xab09d800));
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(cpu.hi == hi_before);
    CHECK(cpu.lo == lo_before);
    CHECK(cpu.muldiv_ts_done == muldiv_ts_before);
    CHECK(cpu.gte_ts_done == gte_ts_before);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 1u);
    psx_xg_render_auth_world_effects_shadow_snapshot(&effects);
    CHECK(effects.native_cutover_count == 1u);
    CHECK(effects.native_primitive_count == 1u);
    CHECK(effects.begin_count == 0u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 1u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_effects_fails_before_staging_or_writes(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    const uint32_t packet = world_effects_packet_address(0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_effects_cpu(&cpu);
    world_store_word(packet, UINT32_C(0x08000000));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80089c78), UINT32_C(0x27bdffb0)));
    CHECK(world_effects_packet_write_count == 0u);
    CHECK(world_ot_write_count == 0u);
    CHECK(world_read_word(packet) == UINT32_C(0x08000000));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_effects_stages_full_capacity(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    PsxXgRenderWorldEffectsShadowSnapshot effects = { 0 };
    const uint32_t particles = UINT32_C(0x80060000);
    uint32_t index;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_effects_cpu(&cpu);
    for (index = 1u; index < XG_WORLD_EFFECTS_SOURCE_CAPACITY; ++index) {
        const uint32_t source = particles + index * 0x4cu;

        world_store_half(source + 6u, 1u);
        world_store_word(source + 8u, 0u);
        world_store_word(source + 0x0cu, 0u);
        world_store_word(source + 0x10u, 0u);
        world_store_word(source + 0x38u,
            (uint32_t)4096u | ((uint32_t)4096u << 16u));
        world_store_word(source + 0x40u, UINT32_C(0x00302010));
        world_store_word(source + 0x44u, 0u);
        world_store_half(source + 0x48u, 0x0123u);
        world_store_word(world_effects_packet_address(index),
                         UINT32_C(0x09000000));
    }

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80089c78), UINT32_C(0x27bdffb0)));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == XG_WORLD_EFFECTS_SOURCE_CAPACITY);
    CHECK(world_effects_packet_write_count ==
          XG_WORLD_EFFECTS_SOURCE_CAPACITY * 10u);
    CHECK(world_ot_write_count == XG_WORLD_EFFECTS_SOURCE_CAPACITY);
    CHECK(world_read_word(WORLD_OT_BASE + 32u * 4u) ==
          (UINT32_C(0xab000000) |
           (world_effects_packet_address(255u) & UINT32_C(0x00ffffff))));
    CHECK(world_read_word(world_effects_packet_address(255u)) ==
          (UINT32_C(0x09000000) |
           (world_effects_packet_address(254u) & UINT32_C(0x00ffffff))));
    psx_xg_render_auth_world_effects_shadow_snapshot(&effects);
    CHECK(effects.native_cutover_count == 1u);
    CHECK(effects.native_primitive_count == XG_WORLD_EFFECTS_SOURCE_CAPACITY);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == XG_WORLD_EFFECTS_SOURCE_CAPACITY);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_horizon_cutover_is_atomic(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    PsxXgRenderWorldHorizonShadowSnapshot horizon = { 0 };
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    uint32_t hi_before;
    uint32_t lo_before;
    uint64_t muldiv_ts_before;
    uint64_t gte_ts_before;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_horizon_cpu(&cpu);
    memset(cpu.gte_data, 0x5a, sizeof(cpu.gte_data));
    cpu.hi = UINT32_C(0x12345678);
    cpu.lo = UINT32_C(0x9abcdef0);
    cpu.muldiv_ts_done = UINT64_C(0x0123456789abcdef);
    cpu.gte_ts_done = UINT64_C(0xfedcba9876543210);
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));
    hi_before = cpu.hi;
    lo_before = cpu.lo;
    muldiv_ts_before = cpu.muldiv_ts_done;
    gte_ts_before = cpu.gte_ts_done;

    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b04), UINT32_C(0x27bdffc0)));
    CHECK(cpu.pc == UINT32_C(0x80071b58));
    CHECK(world_payload_read_count == 0u);
    CHECK(world_horizon_packet_write_count == 20u);
    CHECK(world_horizon_window_write_count == 2u);
    CHECK(world_ot_write_count == 1u);
    CHECK(world_scratch_write_count == 0u);
    CHECK(world_read_word(world_horizon_packet_address(0u)) ==
          UINT32_C(0x0909d3e4));
    CHECK(world_read_word(world_horizon_packet_address(1u)) ==
          UINT32_C(0x0909c76c));
    CHECK(world_read_word(UINT32_C(0x8009d3d8)) ==
          UINT32_C(0x0209c7bc));
    CHECK(world_read_word(UINT32_C(0x8009d3e4)) ==
          UINT32_C(0x02123456));
    CHECK(world_read_word(WORLD_OT_BASE + 248u * 4u) ==
          UINT32_C(0xab09d3d8));
    CHECK(world_read_word(world_horizon_packet_address(0u) + 8u) ==
          pack_s16(-113, -28));
    CHECK(world_read_word(world_horizon_packet_address(1u) + 32u) ==
          pack_s16(424, -9));
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(cpu.hi == hi_before);
    CHECK(cpu.lo == lo_before);
    CHECK(cpu.muldiv_ts_done == muldiv_ts_before);
    CHECK(cpu.gte_ts_done == gte_ts_before);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == XG_WORLD_HORIZON_QUAD_COUNT);
    psx_xg_render_auth_world_horizon_shadow_snapshot(&horizon);
    CHECK(horizon.native_cutover_count == 1u);
    CHECK(horizon.native_primitive_count == XG_WORLD_HORIZON_QUAD_COUNT);
    CHECK(horizon.begin_count == 0u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == XG_WORLD_HORIZON_QUAD_COUNT);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_resident_line_f2_stages_producer_semantics(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = {0};
    GuestRenderNativeStreamCommandIdentity identity = {
        .source_kind = GUEST_RENDER_NATIVE_STREAM_SOURCE_DMA_LINKED_LIST,
        .opcode = 0x40u,
        .word_count = 3u,
    };
    GuestRenderBridgeSnapshot bridge = {0};
    GpuRenderTransactionId visual_id = {0};
    GpuRenderSemantic semantic = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    guest_render_native_stream_set_enabled(true);
    configure_resident_line_f2_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x8007fbe0)));
    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x80073b64)));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8007fbe0), UINT32_C(0x00a04021)));
    materialize_resident_line_f2_output();
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b64), UINT32_C(0x3c03800d)));
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.binding_count == RESIDENT_LINE_F2_COUNT);

    psx_xg_render_auth_before_gpu_submission();
    for (uint32_t index = 0u; index < RESIDENT_LINE_F2_COUNT; ++index) {
        const uint32_t y = (uint32_t)(
            world_native_read_byte(
                UINT32_C(0x800c3202) + RESIDENT_LINE_F2_LEVEL * 6u + index) +
            0x5eu);

        identity.container_id = KUSEG_ADDRESS(resident_line_f2_packet(index));
        identity.command_id = identity.container_id + 4u;
        CHECK(guest_render_native_stream_reserve_exact(
                  UINT64_C(0x100) + index, &identity, &visual_id, &semantic) ==
              GUEST_RENDER_NATIVE_STREAM_OK);
        CHECK(visual_id.scene_epoch != 0u);
        CHECK(semantic.topology == GPU_RENDER_SEMANTIC_LINES);
        CHECK(semantic.line_count == 1u);
        CHECK(semantic.material.shading == GPU_RENDER_SHADING_FLAT);
        CHECK(semantic.material.draw_area_right == 319u);
        CHECK(semantic.material.draw_area_bottom == 239u);
        CHECK(semantic.lines[0].vertices[0].x == 12 * INT32_C(65536));
        CHECK(semantic.lines[0].vertices[1].x == 18 * INT32_C(65536));
        CHECK(semantic.lines[0].vertices[0].y == (int32_t)y * INT32_C(65536));
        CHECK(semantic.lines[0].vertices[1].y == (int32_t)y * INT32_C(65536));
        for (uint32_t vertex = 0u; vertex < 2u; ++vertex) {
            CHECK(semantic.lines[0].vertices[vertex].r == UINT8_C(0xff));
            CHECK(semantic.lines[0].vertices[vertex].g == UINT8_C(0xff));
            CHECK(semantic.lines[0].vertices[vertex].b == UINT8_C(0xff));
        }
        guest_render_native_stream_release_reservation(
            UINT64_C(0x100) + index);
    }
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == RESIDENT_LINE_F2_COUNT);
    psx_xg_render_auth_scene_boundary();
    guest_render_native_stream_set_enabled(false);
    return 1;
}

static int test_resident_line_f2_rejects_stale_or_mutated_source(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_resident_line_f2_cpu(&cpu);
    materialize_resident_line_f2_output();
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b64), UINT32_C(0x3c03800d)));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_resident_line_f2_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8007fbe0), UINT32_C(0x00a04021)));
    materialize_resident_line_f2_output();
    world_native_put_u32(
        resident_line_f2_packet(1u) + 4u, UINT32_C(0x40fffffe));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b64), UINT32_C(0x3c03800d)));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_horizon_fails_before_staging_or_writes(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_horizon_cpu(&cpu);
    world_store_word(UINT32_C(0x8009d3dc), UINT32_C(0xe2000011));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b04), UINT32_C(0x27bdffc0)));
    CHECK(world_horizon_packet_write_count == 0u);
    CHECK(world_horizon_window_write_count == 0u);
    CHECK(world_ot_write_count == 0u);
    CHECK(world_read_word(WORLD_OT_BASE + 248u * 4u) ==
          UINT32_C(0xab123456));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_minimap_local_cutover_is_atomic(void) {
    static const int16_t expected_xy[4][3][2] = {
        { { 208, 120 }, { 200, 112 }, { 204, 109 } },
        { { 208, 120 }, { 204, 109 }, { 208, 108 } },
        { { 208, 120 }, { 208, 108 }, { 212, 109 } },
        { { 208, 120 }, { 212, 109 }, { 216, 112 } },
    };
    CPUState cpu;
    GuestRenderCompletedState completed = { 0 };
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    PsxXgRenderWorldMinimapShadowSnapshot minimap = { 0 };
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];
    uint32_t hi_before;
    uint32_t lo_before;
    uint64_t muldiv_ts_before;
    uint64_t gte_ts_before;
    uint32_t triangle;
    uint32_t predecessor = UINT32_C(0xab123456);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_minimap_cpu(&cpu);
    memset(cpu.gte_data, 0x5a, sizeof(cpu.gte_data));
    cpu.hi = UINT32_C(0x12345678);
    cpu.lo = UINT32_C(0x9abcdef0);
    cpu.muldiv_ts_done = UINT64_C(0x0123456789abcdef);
    cpu.gte_ts_done = UINT64_C(0xfedcba9876543210);
    memcpy(gte_data_before, cpu.gte_data, sizeof(gte_data_before));
    memcpy(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before));
    hi_before = cpu.hi;
    lo_before = cpu.lo;
    muldiv_ts_before = cpu.muldiv_ts_done;
    gte_ts_before = cpu.gte_ts_done;

    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x8007412c)));
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8007412c), UINT32_C(0x266400b8)));
    CHECK(cpu.pc == UINT32_C(0x80074298));
    CHECK(cpu.gpr[3] ==
          (world_minimap_packet_address(3u) & UINT32_C(0x00ffffff)));
    CHECK(cpu.gpr[4] == WORLD_OT_BASE);
    CHECK(cpu.gpr[5] == WORLD_CONTEXT_OBJECT);
    CHECK(cpu.gpr[16] == UINT32_C(0x8009c744));
    CHECK(cpu.gpr[17] == 4u);
    CHECK(cpu.gpr[18] == UINT32_C(0x8009a3a0));
    CHECK(cpu.gpr[29] == UINT32_C(0x801fefc8));
    CHECK(world_read_word(cpu.gpr[29] + 0x30u) == UINT32_C(0x80071b84));
    CHECK(world_payload_read_count == 0u);
    CHECK(world_minimap_tag_write_count == 4u);
    CHECK(world_minimap_xy_write_count == 12u);
    CHECK(world_ot_write_count == 4u);
    CHECK(world_scratch_write_count == 40u);
    for (triangle = 0u; triangle < 4u; ++triangle) {
        const uint32_t packet = world_minimap_packet_address(triangle);

        CHECK(world_read_word(packet) ==
              (UINT32_C(0x06000000) |
               (predecessor & UINT32_C(0x00ffffff))));
        CHECK(world_read_word(packet + 4u) ==
              UINT32_C(0xa1000000) + triangle * 0x100u + 1u);
        CHECK(world_read_word(packet + 8u) ==
              pack_s16(expected_xy[triangle][0][0],
                       expected_xy[triangle][0][1]));
        CHECK(world_read_word(packet + 16u) ==
              pack_s16(expected_xy[triangle][1][0],
                       expected_xy[triangle][1][1]));
        CHECK(world_read_word(packet + 24u) ==
              pack_s16(expected_xy[triangle][2][0],
                       expected_xy[triangle][2][1]));
        predecessor = packet;
    }
    CHECK(world_read_word(WORLD_OT_BASE) ==
          (UINT32_C(0xab000000) |
           (world_minimap_packet_address(3u) & UINT32_C(0x00ffffff))));
    CHECK(world_read_word(UINT32_C(0x1f8000b8)) == 0u);
    CHECK(world_read_word(UINT32_C(0x1f8000bc)) == UINT32_C(0xcafe0000));
    CHECK(world_read_word(UINT32_C(0x1f8000f0)) == UINT32_C(0x00001000));
    CHECK(world_read_word(UINT32_C(0x1f8000f4)) == 0u);
    CHECK(world_read_word(UINT32_C(0x1f8000f8)) == UINT32_C(0x00001000));
    CHECK(world_read_word(UINT32_C(0x1f8000fc)) == 0u);
    CHECK(world_read_word(UINT32_C(0x1f800100)) == UINT32_C(0xbeef1000));
    CHECK(world_read_word(UINT32_C(0x1f800104)) == 48u);
    CHECK(world_read_word(UINT32_C(0x1f800108)) == 0u);
    CHECK(world_read_word(UINT32_C(0x1f80010c)) == 256u);
    CHECK(world_read_word(UINT32_C(0x8009c5a0)) == UINT32_C(0x01123456));
    CHECK(world_read_word(UINT32_C(0x8009c5e8)) == UINT32_C(0x09123456));
    CHECK(memcmp(gte_data_before, cpu.gte_data, sizeof(gte_data_before)) == 0);
    CHECK(memcmp(gte_ctrl_before, cpu.gte_ctrl, sizeof(gte_ctrl_before)) == 0);
    CHECK(cpu.hi == hi_before);
    CHECK(cpu.lo == lo_before);
    CHECK(cpu.muldiv_ts_done == muldiv_ts_before);
    CHECK(cpu.gte_ts_done == gte_ts_before);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == XG_WORLD_MINIMAP_TRIANGLE_COUNT);
    psx_xg_render_auth_world_minimap_shadow_snapshot(&minimap);
    CHECK(minimap.native_cutover_count == 1u);
    CHECK(minimap.native_primitive_count == XG_WORLD_MINIMAP_TRIANGLE_COUNT);
    CHECK(minimap.begin_count == 0u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == XG_WORLD_MINIMAP_TRIANGLE_COUNT);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_minimap_validates_saved_ra_and_gprs(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    PsxXgRenderWorldMinimapShadowSnapshot minimap = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_minimap_cpu(&cpu);
    world_store_word(cpu.gpr[29] + 0x30u, UINT32_C(0x80071b88));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8007412c), UINT32_C(0x266400b8)));
    CHECK(world_minimap_tag_write_count == 0u);
    CHECK(world_minimap_xy_write_count == 0u);
    CHECK(world_ot_write_count == 0u);
    CHECK(world_scratch_write_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_world_minimap_shadow_snapshot(&minimap);
    CHECK(minimap.native_cutover_count == 0u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_minimap_cpu(&cpu);
    cpu.gpr[18] += 8u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8007412c), UINT32_C(0x266400b8)));
    CHECK(world_minimap_tag_write_count == 0u);
    CHECK(world_minimap_xy_write_count == 0u);
    CHECK(world_ot_write_count == 0u);
    CHECK(world_scratch_write_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_world_minimap_aborts_partial_staging(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = { 0 };
    PsxXgRenderWorldMinimapShadowSnapshot minimap = { 0 };
    const uint32_t duplicate_packet = world_minimap_packet_address(3u);

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_effects_cpu(&cpu);
    world_store_word(UINT32_C(0x8009be20), duplicate_packet);
    world_store_word(duplicate_packet, UINT32_C(0x09000000));
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80089c78), UINT32_C(0x27bdffb0)));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 1u);

    configure_world_minimap_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8007412c), UINT32_C(0x266400b8)));
    CHECK(world_minimap_tag_write_count == 0u);
    CHECK(world_minimap_xy_write_count == 0u);
    CHECK(world_ot_write_count == 0u);
    CHECK(world_scratch_write_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_world_minimap_shadow_snapshot(&minimap);
    CHECK(minimap.native_cutover_count == 0u);
    CHECK(minimap.native_primitive_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static void configure_empty_world_terrain_native_cpu(CPUState *cpu) {
    uint32_t index;

    configure_empty_world_native_cpu(cpu);
    cpu->gpr[4] = WORLD_NATIVE_OT;
    cpu->gpr[5] = WORLD_NATIVE_PACKETS;
    cpu->gpr[6] = XG_WORLD_TERRAIN_WATER_NATIVE_POSITION_ADDRESS;
    cpu->gpr[31] = UINT32_C(0x80071b38);
    cpu->gte_ctrl[26] = 32u;
    world_native_put_u32(XG_WORLD_TERRAIN_WATER_NATIVE_CONTEXT_ADDRESS,
                         WORLD_NATIVE_CONTEXT);
    world_native_put_u32(WORLD_NATIVE_CONTEXT + 0x70u, WORLD_NATIVE_OT);
    world_native_put_u32(WORLD_NATIVE_CONTEXT + 0x74u,
                         WORLD_NATIVE_PACKETS);
    world_native_put_identity_matrix(UINT32_C(0x8009c808), 2048);
    world_native_put_identity_matrix(UINT32_C(0x8009d534), 0);
    world_native_put_u32(UINT32_C(0x8009c618), 0x400u);
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_TILE_COUNT; ++index)
        world_native_put_u16(UINT32_C(0x8009d618) + index * 2u,
                             UINT16_MAX);
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_CLUT_COUNT; ++index)
        world_native_put_u16(UINT32_C(0x8009ccb4) + index * 2u,
                             (uint16_t)((432u + index) << 6u));
    for (index = 0u; index < XG_WORLD_TERRAIN_WATER_PAGE_COUNT - 1u; ++index)
        world_native_put_u16(UINT32_C(0x8009cd54) + index * 2u,
                             (uint16_t)(0x88u + index * 2u));
    world_native_put_u16(UINT32_C(0x1f800316), 0x9cu);
}

static void configure_empty_world_entity_native_cpu(CPUState *cpu) {
    configure_empty_world_native_cpu(cpu);
    cpu->gpr[31] = XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC;
    world_native_put_u32(UINT32_C(0x8009be38), 0u);
}

static void configure_empty_world_decorations_native_cpu(CPUState *cpu) {
    uint32_t index;

    configure_empty_world_native_cpu(cpu);
    cpu->gpr[31] = XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN;
    world_native_put_identity_matrix(UINT32_C(0x8009c808), 512);
    world_native_put_identity_matrix(UINT32_C(0x8009a180), 0);
    world_native_put_u16(UINT32_C(0x8009bd3c), 1024u);
    world_native_put_u32(UINT32_C(0x800533f0), UINT32_C(0x00001000));
    world_native_put_u32(UINT32_C(0x8009d160), 32u);
    world_native_put_u32(UINT32_C(0x8009d2b4), 32u);
    world_native_put_u32(UINT32_C(0x8009c7ec), UINT32_C(0x80013000));
    for (index = 0u;
         index < XG_WORLD_DECORATIONS_NATIVE_GRID_CELL_COUNT; ++index)
        world_native_put_u16(UINT32_C(0x8009d618) + index * 2u,
                             UINT16_MAX);
    for (index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT; ++index)
        world_native_put_u16(UINT32_C(0x8009d478) + index * 2u,
            (uint16_t)(((0x1f0u + index) << 6u) | 0x0fu));
    world_native_put_u32(XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS,
                         UINT32_C(0x00001234));
}

static void configure_empty_world_models_native_cpu(CPUState *cpu) {
    configure_empty_world_native_cpu(cpu);
    cpu->gpr[29] = WORLD_NATIVE_STACK;
    cpu->gpr[31] = XG_WORLD_MODELS_PRODUCER_CONTINUATION_0;
    world_native_put_u16(UINT32_C(0x8009d7e0), 0u);
}

static void configure_nonempty_world_terrain_native_cpu(CPUState *cpu) {
    static const uint32_t camera[8] = {
        UINT32_C(0x00001000), 0u, UINT32_C(0xf0000000), 0u,
        0u, 0u, 0u, 2048u,
    };
    const uint32_t resource = UINT32_C(0x80012000);
    uint32_t index;

    configure_empty_world_terrain_native_cpu(cpu);
    for (index = 0u; index < 8u; ++index)
        world_native_put_u32(UINT32_C(0x8009c808) + index * 4u,
                             camera[index]);
    world_native_put_u16(UINT32_C(0x8009c838), 2u);
    world_native_put_u16(UINT32_C(0x8009c83c), 2u);
    world_native_put_u16(UINT32_C(0x8009d618), 0u);
    world_native_put_u16(UINT32_C(0x8009d650), 0u);
    world_native_put_u16(UINT32_C(0x8009d652), UINT16_MAX);
    world_native_put_u16(UINT32_C(0x8009d654), UINT16_MAX);
    world_native_put_u16(UINT32_C(0x8009d656), UINT16_MAX);
    for (index = 0u; index < 81u; ++index)
        world_native_put_u16(UINT32_C(0x8009d570) + index * 2u, 0u);
    world_native_put_u32(UINT32_C(0x8009c184), resource);
    for (index = 0u; index < 81u; ++index)
        world_native_put_u32(resource + index * 4u, 0u);
    for (index = 0u; index < 128u; ++index) {
        const uint32_t packet = WORLD_NATIVE_PACKETS + index * 0x20u;

        world_native_put_u32(packet, UINT32_C(0x07000000));
        world_native_put_u32(
            packet + 4u,
            XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_MATERIAL_WORD);
    }
    world_native_write_count = 0u;
}

static void configure_nonempty_world_entity_native_cpu(CPUState *cpu) {
    static const int16_t types[3] = {0, 1, 2};
    const uint32_t list = UINT32_C(0x80012000);
    const uint32_t terrain = UINT32_C(0x80013000);
    const uint32_t packets = UINT32_C(0x80016000);
    uint32_t index;

    configure_empty_world_entity_native_cpu(cpu);
    world_native_put_identity_matrix(UINT32_C(0x8009c808), 512);
    world_native_put_identity_matrix(UINT32_C(0x8009a180), 0);
    world_native_put_u32(UINT32_C(0x8009be38), 3u);
    world_native_put_u32(UINT32_C(0x8009d30c), list);
    world_native_put_u32(UINT32_C(0x8009d160), 8u);
    world_native_put_u32(UINT32_C(0x8009b244), 1u);
    world_native_put_u32(UINT32_C(0x8009c184), terrain);
    world_native_put_u32(UINT32_C(0x800523f0), UINT32_C(0x10000000));
    world_native_put_u16(UINT32_C(0x8006ee66), 0u);
    for (index = 0u; index < 3u; ++index) {
        const uint32_t entry = list + index * 8u;
        const uint32_t packet = packets + index * 0x28u;

        world_native_put_u32(entry, pack_s16(0, types[index]));
        world_native_put_u16(entry + 4u, 0u);
        world_native_put_u32(packet, UINT32_C(0x09000000));
        world_native_put_u32(packet + 4u, UINT32_C(0x2e484040));
        world_native_put_u32(packet + 12u, UINT32_C(0x7f92f080));
        world_native_put_u32(packet + 20u, UINT32_C(0x001ef08f));
        world_native_put_u32(packet + 28u, UINT32_C(0x0000ff80));
        world_native_put_u32(packet + 36u, UINT32_C(0x0000ff8f));
    }
    world_native_put_u32(XG_WORLD_ENTITY_SHADOWS_BUFFER_INDEX_ADDRESS, 0u);
    world_native_put_u32(XG_WORLD_ENTITY_SHADOWS_PACKET_BASES_ADDRESS,
                         packets);
    world_native_put_u32(XG_WORLD_ENTITY_SHADOWS_CONTEXT_ADDRESS,
                         WORLD_NATIVE_CONTEXT);
    world_native_put_u32(
        WORLD_NATIVE_CONTEXT + XG_WORLD_ENTITY_SHADOWS_CONTEXT_OT_OFFSET,
        WORLD_NATIVE_OT);
    world_native_put_u32(WORLD_NATIVE_OT + 27u * 4u,
                         UINT32_C(0xab123456));
    world_native_put_u32(WORLD_NATIVE_OT + 30u * 4u,
                         UINT32_C(0xab234567));
    world_native_put_u32(WORLD_NATIVE_OT + 31u * 4u,
                         UINT32_C(0xab345678));
    world_native_write_count = 0u;
}

static void configure_nonempty_world_decorations_native_cpu(CPUState *cpu) {
    const uint32_t positions = UINT32_C(0x80015000);
    const uint32_t descriptors = UINT32_C(0x80015800);
    const uint32_t packets = WORLD_NATIVE_DECORATION_PACKETS;
    uint32_t index;

    configure_empty_world_decorations_native_cpu(cpu);
    world_native_put_u16(UINT32_C(0x8009d618), 0u);
    world_native_put_u16(UINT32_C(0x8009d570), 3u);
    world_native_put_u32(UINT32_C(0x8009c7ec), descriptors);
    world_native_put_u32(descriptors + 3u * 8u, positions);
    world_native_put_u32(descriptors + 3u * 8u + 4u, 1u);
    world_native_put_u32(positions, pack_s16(0, 0));
    world_native_put_u32(positions + 4u, pack_s16(-2048, 0));
    world_native_put_u32(UINT32_C(0x8009d7e8), packets);
    world_native_put_u32(UINT32_C(0x8009be3c), WORLD_NATIVE_CONTEXT);
    world_native_put_u32(WORLD_NATIVE_CONTEXT + 0x70u, WORLD_NATIVE_OT);
    world_native_put_u32(packets, UINT32_C(0x09000000));
    world_native_put_u32(packets + 4u, UINT32_C(0x2c808080));
    world_native_put_u32(packets + 12u, UINT32_C(0x00004000));
    world_native_put_u32(packets + 20u, UINT32_C(0x001e401f));
    world_native_put_u32(packets + 28u, UINT32_C(0xcd036f00));
    world_native_put_u32(packets + 36u, UINT32_C(0xab046f1f));
    for (index = 0u; index < XG_WORLD_DECORATIONS_DEPTH_CLUT_COUNT; ++index)
        world_native_put_u16(UINT32_C(0x8009d478) + index * 2u,
            (uint16_t)(((0x1f0u + index) << 6u) | 0x0fu));
    world_native_put_u32(WORLD_NATIVE_OT + 160u * 4u,
                          UINT32_C(0x00123456));
    world_native_write_count = 0u;
}

static int test_remaining_world_native_nonempty_cutovers_are_atomic(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = {0};
    GuestRenderTransactionPendingSnapshot pending = {0};
    XgWorldTerrainWaterShadowSnapshot terrain = {0};
    XgWorldEntityShadowsShadowSnapshot entity = {0};
    XgWorldDecorationsShadowSnapshot decorations = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_nonempty_world_terrain_native_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
        UINT32_C(0x27bdffc8)));
    CHECK(cpu.pc == UINT32_C(0x80071b38));
    CHECK(world_native_read_word(
              XG_WORLD_TERRAIN_WATER_NATIVE_FINAL_COUNT_ADDRESS) == 128u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 128u);
    xg_world_terrain_water_shadow_snapshot(&terrain);
    CHECK(terrain.native_cutover_count == 1u);
    CHECK(terrain.native_primitive_count == 128u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 128u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_nonempty_world_entity_native_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800747dc), UINT32_C(0x3c02800a)));
    CHECK(cpu.pc == XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC);
    CHECK(world_native_read_word(
              XG_WORLD_ENTITY_SHADOWS_PENDING_COUNT_ADDRESS) == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 3u);
    xg_world_entity_shadows_shadow_snapshot(&entity);
    CHECK(entity.native_cutover_count == 1u);
    CHECK(entity.native_primitive_count == 3u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 3u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_nonempty_world_decorations_native_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
        UINT32_C(0x27bdffd8)));
    CHECK(cpu.pc == XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN);
    CHECK(world_native_read_word(
              XG_WORLD_DECORATIONS_NATIVE_SHARED_COUNT_ADDRESS) == 1u);
    CHECK(world_native_read_word(
              WORLD_NATIVE_DECORATION_PACKETS + 28u) ==
          UINT32_C(0xcd036f00));
    CHECK(world_native_read_word(
              WORLD_NATIVE_DECORATION_PACKETS + 36u) ==
          UINT32_C(0xab046f1f));
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 1u);
    xg_world_decorations_shadow_snapshot(&decorations);
    CHECK(decorations.native_cutover_count == 1u);
    CHECK(decorations.native_primitive_count == 1u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 1u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int observe_world_model_initializer_primitive(
    CPUState *cpu, uint32_t family, uint32_t packet,
    uint32_t initializer_function) {
    world_native_put_u32(
        XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL, packet);
    cpu->gpr[2] = 1u;
    cpu->gpr[17] = UINT32_C(0x8004fe50) + family * 0x28u;
    cpu->gpr[19] = initializer_function;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x8002caa4), UINT32_C(0x3c028006)));

    switch (family) {
    case 2u:
    case 6u: {
        static const uint32_t color = UINT32_C(0x00e0e0e0);
        uint32_t final_words[3];

        cpu->gpr[7] = packet + 4u;
        cpu->gpr[8] = packet + 12u;
        cpu->gpr[9] = packet + 20u;
        final_words[0] =
            (world_native_read_word(cpu->gpr[7]) & UINT32_C(0xff000000)) |
            color;
        final_words[1] =
            (world_native_read_word(cpu->gpr[8]) & UINT32_C(0xff000000)) |
            color;
        final_words[2] =
            (world_native_read_word(cpu->gpr[9]) & UINT32_C(0xff000000)) |
            color;
        world_native_put_u32(cpu->gpr[7], color);
        world_native_put_u32(cpu->gpr[8], color);
        world_native_put_u32(cpu->gpr[9], color);
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            cpu, UINT32_C(0x8004a1e8), UINT32_C(0xe9360000)));
        world_native_put_u32(cpu->gpr[7], final_words[0]);
        world_native_put_u32(cpu->gpr[8], final_words[1]);
        world_native_put_u32(cpu->gpr[9], final_words[2]);
        break;
    }
    case 3u:
    case 7u:
        cpu->gpr[8] = packet + 4u;
        cpu->gpr[9] = packet + 16u;
        cpu->gpr[10] = packet + 28u;
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            cpu, UINT32_C(0x8004a2b8), UINT32_C(0xe9560000)));
        break;
    case 10u:
    case 14u:
        cpu->gpr[5] = packet + 4u;
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            cpu, UINT32_C(0x8004a1ac), UINT32_C(0xe8b60000)));
        cpu->gpr[7] = packet + 12u;
        cpu->gpr[8] = packet + 20u;
        cpu->gpr[9] = packet + 28u;
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            cpu, UINT32_C(0x8004a1e8), UINT32_C(0xe9360000)));
        break;
    case 11u:
    case 15u:
        cpu->gpr[6] = packet + 4u;
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            cpu, UINT32_C(0x8004a274), UINT32_C(0xe8d60000)));
        cpu->gpr[8] = packet + 16u;
        cpu->gpr[9] = packet + 28u;
        cpu->gpr[10] = packet + 40u;
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            cpu, UINT32_C(0x8004a2b8), UINT32_C(0xe9560000)));
        break;
    default:
        break;
    }
    return 1;
}

static uint32_t configure_all_family_world_models_native_cpu(CPUState *cpu) {
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
    static const uint8_t attribute_sizes[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        4u, 8u, 4u, 8u, 4u, 8u, 4u, 8u, 4u,
        12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u,
    };
    static const uint8_t packet_sizes[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        0x14u, 0x20u, 0x1cu, 0x28u, 0x14u, 0x20u, 0x1cu, 0x28u,
        0x18u, 0x28u, 0x24u, 0x34u, 0x18u, 0x28u, 0x24u, 0x34u,
        0x20u,
    };
    static const uint8_t commands[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        0x20u, 0x24u, 0x30u, 0x34u, 0x20u, 0x24u, 0x30u, 0x34u,
        0x28u, 0x2cu, 0x38u, 0x3cu, 0x28u, 0x2cu, 0x38u, 0x3cu,
        0x24u,
    };
    enum {
        RECORD = 0x80012000u,
        MODEL = 0x80012100u,
        VERTICES = 0x80012200u,
        AUXILIARY_VERTICES = 0x80012300u,
        TOPOLOGY = 0x80012400u,
        ATTRIBUTES = 0x80013000u,
        PACKETS = 0x80014000u,
        REINITIALIZED_PACKETS = 0x80014240u,
        CONTEXT = 0x80018000u,
        OT = 0x80019000u,
    };
    static const XgHost3dVector vertices[XG_HOST_3D_VERTEX_COUNT] = {
        {-32, -32, 0, 0u}, {32, -32, 0, 0u},
        {-32, 32, 0, 0u}, {32, 32, 0, 0u},
    };
    uint32_t topology = TOPOLOGY;
    uint32_t attribute = ATTRIBUTES;
    uint32_t packet = PACKETS;
    uint32_t packet_capacity;
    uint32_t family;
    uint32_t offset;
    uint32_t vertex;

    configure_empty_world_native_cpu(cpu);
    world_native_put_u16(MODEL + 4u, XG_HOST_3D_VERTEX_COUNT);
    world_native_put_u16(MODEL + 6u,
                         XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT);
    world_native_put_u32(MODEL + 8u, VERTICES);
    world_native_put_u32(MODEL + 0x0cu, AUXILIARY_VERTICES);
    world_native_put_u32(MODEL + 0x10u, TOPOLOGY);
    world_native_put_u32(MODEL + 0x14u, ATTRIBUTES);
    world_native_put_u32(MODEL + 0x18u, UINT32_C(0x12345678));
    world_native_put_u32(MODEL + 0x20u, pack_s16(-32, -32));
    world_native_put_u32(MODEL + 0x24u, 0u);
    world_native_put_u32(MODEL + 0x28u, pack_s16(32, 32));
    world_native_put_u32(MODEL + 0x2cu, 0u);
    for (vertex = 0u; vertex < XG_HOST_3D_VERTEX_COUNT; ++vertex) {
        world_native_put_u32(VERTICES + vertex * 8u,
            pack_s16(vertices[vertex].x, vertices[vertex].y));
        world_native_put_u32(VERTICES + vertex * 8u + 4u,
            pack_s16(vertices[vertex].z, (int16_t)vertices[vertex].pad));
        world_native_put_u32(AUXILIARY_VERTICES + vertex * 8u,
            pack_s16(vertices[vertex].x, vertices[vertex].y));
        world_native_put_u32(AUXILIARY_VERTICES + vertex * 8u + 4u,
            pack_s16(vertices[vertex].z, (int16_t)vertices[vertex].pad));
    }
    for (family = 0u;
         family < XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT; ++family) {
        const uint32_t table = UINT32_C(0x8004fe50) + family * 0x28u;
        uint32_t word;

        world_native_put_u8(topology, (uint8_t)family);
        world_native_put_u16(topology + 2u, 1u);
        world_native_put_u16(topology + 4u, 0u);
        world_native_put_u16(topology + 6u, 1u);
        world_native_put_u16(topology + 8u, 2u);
        world_native_put_u16(topology + 10u, 3u);
        topology += 12u;
        world_native_put_u32(
            attribute,
            (uint32_t)(commands[family] |
                       (family == 9u || family == 13u ? 1u : 0u)) << 24u);
        for (word = 4u; word < attribute_sizes[family]; word += 4u)
            world_native_put_u32(attribute + word, 0u);
        attribute += attribute_sizes[family];
        world_native_put_u32(packet,
            ((uint32_t)packet_sizes[family] / 4u -
             (family == 8u ? 2u : 1u)) << 24u);
        world_native_put_u32(
            packet + 4u,
            (uint32_t)(commands[family] | (family == 9u ? 1u : 0u)) << 24u);
        for (word = 8u; word < packet_sizes[family]; word += 4u)
            world_native_put_u32(packet + word, 0u);
        packet += packet_sizes[family];
        world_native_put_u32(table + 0x18u, initializer_functions[family]);
        world_native_put_u32(table + 0x1cu, 8u);
        world_native_put_u32(table + 0x20u, attribute_sizes[family]);
        world_native_put_u32(table + 0x24u, packet_sizes[family]);
    }
    packet_capacity = packet - PACKETS;
    world_native_put_u32(MODEL + 0x34u, packet_capacity);

    world_native_put_u16(RECORD + XG_WORLD_MODELS_RECORD_STATE_OFFSET, 0u);
    world_native_put_u16(
        RECORD + XG_WORLD_MODELS_RECORD_DISPATCH_SELECTOR_OFFSET, 0u);
    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_POSITION_X_OFFSET,
                         100u);
    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_STORED_Z_OFFSET,
                         (uint32_t)-512);
    world_native_put_identity_matrix(
        RECORD + XG_WORLD_MODELS_RECORD_MATRIX_OFFSET, 0);
    world_native_put_u32(
        RECORD + XG_WORLD_MODELS_RECORD_MODEL_HEADER_OFFSET, MODEL);
    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_PACKET_BASE_0_OFFSET,
                         PACKETS);
    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_PACKET_BASE_1_OFFSET,
                         PACKETS);
    world_native_put_u32(XG_WORLD_MODELS_RECORD_BASE_GLOBAL, RECORD);
    world_native_put_u16(XG_WORLD_MODELS_RECORD_COUNT_GLOBAL, 1u);
    world_native_put_u32(XG_WORLD_MODELS_BUFFER_INDEX_GLOBAL, 0u);
    world_native_put_u32(XG_WORLD_MODELS_WRAP_X_GLOBAL, 20u);
    world_native_put_u32(XG_WORLD_MODELS_WRAP_Z_GLOBAL, 20u);
    world_native_put_identity_matrix(XG_WORLD_MODELS_CAMERA_MATRIX_GLOBAL, 512);
    world_native_put_u32(XG_WORLD_MODELS_CONTEXT_POINTER_GLOBAL, CONTEXT);
    world_native_put_u32(CONTEXT + XG_WORLD_MODELS_CONTEXT_OT_OFFSET, OT);
    world_native_put_u32(UINT32_C(0x800500f8), 319u);
    world_native_put_u32(UINT32_C(0x800500fc), UINT32_C(0x00ef0000));
    world_native_put_u32(UINT32_C(0x80050100), 4u);
    world_native_put_u32(UINT32_C(0x80050108), 0u);
    world_native_put_u32(UINT32_C(0x8005010c), 0u);
    world_native_put_u32(OT + 32u * 4u, UINT32_C(0xab123456));

    cpu->gpr[4] = MODEL;
    cpu->gpr[5] = PACKETS;
    cpu->gpr[6] = 0u;
    cpu->gpr[29] = WORLD_NATIVE_STACK;
    cpu->gpr[31] = UINT32_C(0x80084750);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x8002c8cc), UINT32_C(0x27bdffd0)));

    packet = PACKETS;
    for (family = 0u;
         family < XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT; ++family) {
        CHECK(observe_world_model_initializer_primitive(
            cpu, family, packet, initializer_functions[family]));
        packet += packet_sizes[family];
    }

    world_native_put_u32(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL,
                         packet);
    cpu->gpr[29] = WORLD_NATIVE_STACK;
    cpu->gpr[31] = UINT32_C(0x80084750);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x8002cb4c), UINT32_C(0x03e00008)));

    for (offset = 0u; offset < packet_capacity; offset += 4u)
        world_native_put_u32(
            REINITIALIZED_PACKETS + offset,
            world_native_read_word(PACKETS + offset));
    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_PACKET_BASE_0_OFFSET,
                          REINITIALIZED_PACKETS);
    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_PACKET_BASE_1_OFFSET,
                          REINITIALIZED_PACKETS);
    cpu->gpr[2] = REINITIALIZED_PACKETS;
    cpu->gpr[4] = REINITIALIZED_PACKETS + packet_capacity;
    cpu->gpr[5] = PACKETS + packet_capacity;
    cpu->gpr[6] = 0u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        cpu, UINT32_C(0x80084778), UINT32_C(0x26100010)));
    packet = REINITIALIZED_PACKETS + packet_capacity;
    cpu->gpr[31] = XG_WORLD_MODELS_PRODUCER_CONTINUATION_0;
    cpu->gte_ctrl[24] = 160u << 16u;
    cpu->gte_ctrl[25] = 120u << 16u;
    cpu->gte_ctrl[26] = 256u;
    cpu->gte_ctrl[29] = 1024u;
    cpu->gte_ctrl[30] = 1024u;
    world_native_write_count = 0u;
    return packet;
}

static int test_world_models_all_family_sidecar_cutover(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = {0};
    GuestRenderTransactionPendingSnapshot pending = {0};
    PsxXgRenderWorldNativeSnapshot models = {0};
    uint32_t final_packet;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    final_packet = configure_all_family_world_models_native_cpu(&cpu);
    CHECK(final_packet > UINT32_C(0x80014240));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_MODELS_PRODUCER_ENTRY, UINT32_C(0x24020800)));
    CHECK(world_native_write_count == 0u);
    CHECK(psx_xg_render_auth_runtime_test_materialize_world_models_original(
        &cpu));
    CHECK((world_native_read_word(final_packet - 0x150u) >> 24u) == 5u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80084cd0), UINT32_C(0x8fbf0038)));
    CHECK(world_native_write_count != 0u);
    CHECK(world_native_read_word(
              XG_WORLD_MODELS_RESIDENT_VERTEX_TOTAL_GLOBAL) ==
          XG_HOST_3D_VERTEX_COUNT);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count ==
          XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT);
    psx_xg_render_auth_world_models_native_snapshot(&models);
    CHECK(models.native_cutover_count == 1u);
    CHECK(models.native_primitive_count ==
          XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == pending.binding_count);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_model_reinitialization_preserves_other_packet_buffer(void) {
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
    static const uint8_t packet_sizes[
        XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT] = {
        0x14u, 0x20u, 0x1cu, 0x28u, 0x14u, 0x20u, 0x1cu, 0x28u,
        0x18u, 0x28u, 0x24u, 0x34u, 0x18u, 0x28u, 0x24u, 0x34u,
        0x20u,
    };
    enum {
        RECORD = 0x80012000u,
        MODEL = 0x80012100u,
        PACKETS = 0x80014000u,
        REINITIALIZED_PACKETS = 0x80014240u,
    };
    CPUState cpu;
    uint32_t family;
    uint32_t packet;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    (void)configure_all_family_world_models_native_cpu(&cpu);

    cpu.gpr[4] = MODEL;
    cpu.gpr[5] = REINITIALIZED_PACKETS;
    cpu.gpr[6] = 0u;
    cpu.gpr[29] = WORLD_NATIVE_STACK;
    cpu.gpr[31] = UINT32_C(0x80084750);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002c8cc), UINT32_C(0x27bdffd0)));
    packet = REINITIALIZED_PACKETS;
    for (family = 0u;
         family < XG_WORLD_MODELS_PRIMITIVE_FAMILY_COUNT; ++family) {
        CHECK(observe_world_model_initializer_primitive(
            &cpu, family, packet, initializer_functions[family]));
        packet += packet_sizes[family];
    }
    world_native_put_u32(XG_WORLD_MODELS_RESIDENT_PACKET_CURSOR_GLOBAL,
                         packet);
    cpu.gpr[29] = WORLD_NATIVE_STACK;
    cpu.gpr[31] = UINT32_C(0x80084750);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002cb4c), UINT32_C(0x03e00008)));

    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_PACKET_BASE_0_OFFSET,
                         PACKETS);
    world_native_put_u32(RECORD + XG_WORLD_MODELS_RECORD_PACKET_BASE_1_OFFSET,
                         PACKETS);
    cpu.gpr[31] = XG_WORLD_MODELS_PRODUCER_CONTINUATION_0;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_MODELS_PRODUCER_ENTRY, UINT32_C(0x24020800)));
    CHECK(psx_xg_render_auth_runtime_test_materialize_world_models_original(
        &cpu));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_model_templates_only_track_model_code(void) {
    CPUState cpu;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    (void)configure_all_family_world_models_native_cpu(&cpu);
    psx_xg_render_auth_note_code_write(
        1u, 2u, UINT32_C(0x80071a90), 1u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_MODELS_PRODUCER_ENTRY, UINT32_C(0x24020800)));
    CHECK(psx_xg_render_auth_runtime_test_materialize_world_models_original(
        &cpu));

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    (void)configure_all_family_world_models_native_cpu(&cpu);
    psx_xg_render_auth_note_code_write(
        2u, 3u, UINT32_C(0x8002c8cc), 4u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_MODELS_PRODUCER_ENTRY, UINT32_C(0x24020800)));
    CHECK(!psx_xg_render_auth_runtime_test_materialize_world_models_original(
        &cpu));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_remaining_world_native_empty_cutovers_are_authenticated(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = {0};
    XgWorldTerrainWaterShadowSnapshot terrain = {0};
    XgWorldEntityShadowsShadowSnapshot entity = {0};
    XgWorldDecorationsShadowSnapshot decorations = {0};
    PsxXgRenderWorldNativeSnapshot models = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_terrain_native_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
        UINT32_C(0x27bdffc8)));
    CHECK(cpu.pc == UINT32_C(0x80071b38));
    CHECK(world_native_write_count != 0u);
    CHECK(world_native_read_word(
              XG_WORLD_TERRAIN_WATER_NATIVE_FINAL_COUNT_ADDRESS) == 0u);
    xg_world_terrain_water_shadow_snapshot(&terrain);
    CHECK(terrain.native_cutover_count == 1u);
    CHECK(terrain.native_primitive_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_entity_native_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800747dc), UINT32_C(0x3c02800a)));
    CHECK(cpu.pc == XG_WORLD_ENTITY_SHADOWS_CONTINUATION_PC);
    CHECK(world_native_write_count == 0u);
    xg_world_entity_shadows_shadow_snapshot(&entity);
    CHECK(entity.native_cutover_count == 1u);
    CHECK(entity.native_primitive_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_decorations_native_cpu(&cpu);
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
        UINT32_C(0x27bdffd8)));
    CHECK(cpu.pc == XG_WORLD_DECORATIONS_NATIVE_CALLER_RETURN);
    CHECK(world_native_write_count != 0u);
    xg_world_decorations_shadow_snapshot(&decorations);
    CHECK(decorations.native_cutover_count == 1u);
    CHECK(decorations.native_primitive_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_models_native_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_MODELS_PRODUCER_ENTRY, UINT32_C(0x24020800)));
    CHECK(world_native_write_count == 0u);
    CHECK(psx_xg_render_auth_runtime_test_materialize_world_models_original(
        &cpu));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80084cd0), UINT32_C(0x8fbf0038)));
    CHECK(world_native_write_count != 0u);
    psx_xg_render_auth_world_models_native_snapshot(&models);
    CHECK(models.native_cutover_count == 1u);
    CHECK(models.native_primitive_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_remaining_world_native_cutovers_reject_wrong_callers(void) {
    CPUState cpu;
    GuestRenderTransactionPendingSnapshot pending = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_terrain_native_cpu(&cpu);
    cpu.gpr[31] += 4u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC,
        UINT32_C(0x27bdffc8)));
    CHECK(world_native_write_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_entity_native_cpu(&cpu);
    cpu.gpr[31] += 4u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800747dc), UINT32_C(0x3c02800a)));
    CHECK(world_native_write_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_decorations_native_cpu(&cpu);
    cpu.gpr[31] += 4u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_DECORATIONS_NATIVE_ENTRY_PC,
        UINT32_C(0x27bdffd8)));
    CHECK(world_native_write_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_empty_world_models_native_cpu(&cpu);
    cpu.gpr[31] = UINT32_C(0x80071acc);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_MODELS_PRODUCER_ENTRY, UINT32_C(0x24020800)));
    CHECK(world_native_write_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static void configure_world_actor_native_fixture(
    CPUState *cpu, uint32_t entry_stack_pointer) {
    configure_empty_world_native_cpu(cpu);
    world_native_put_u16(WORLD_NATIVE_ACTOR + 10u, 512u);
    world_native_put_u32(WORLD_NATIVE_ACTOR + 0x20u,
                         WORLD_NATIVE_ACTOR_DATA);
    world_native_put_u16(WORLD_NATIVE_ACTOR + 0x2cu, 4096u);
    world_native_put_u32(WORLD_NATIVE_ACTOR + 0x3cu, 4u);
    world_native_put_u32(WORLD_NATIVE_ACTOR + 0x40u, 4u);
    world_native_put_identity_matrix(WORLD_NATIVE_ACTOR_DATA + 0x0cu, 512);
    world_native_put_u32(WORLD_NATIVE_ACTOR_DATA + 0x30u,
                         WORLD_NATIVE_ACTOR_DESCRIPTOR);
    world_native_put_u16(WORLD_NATIVE_ACTOR_DESCRIPTOR, (uint16_t)-32);
    world_native_put_u16(WORLD_NATIVE_ACTOR_DESCRIPTOR + 2u, (uint16_t)-48);
    world_native_put_u8(WORLD_NATIVE_ACTOR_DESCRIPTOR + 4u, 8u);
    world_native_put_u8(WORLD_NATIVE_ACTOR_DESCRIPTOR + 5u, 16u);
    world_native_put_u8(WORLD_NATIVE_ACTOR_DESCRIPTOR + 6u, 64u);
    world_native_put_u8(WORLD_NATIVE_ACTOR_DESCRIPTOR + 7u, 96u);
    world_native_put_u16(WORLD_NATIVE_ACTOR_DESCRIPTOR + 0x0au, 0x23u);
    world_native_put_u16(WORLD_NATIVE_ACTOR_DESCRIPTOR + 0x0cu, 0x42u);
    world_native_put_u32(WORLD_NATIVE_ACTOR_DESCRIPTOR + 0x10u,
                         UINT32_C(0x2d806040));
    world_native_put_identity_matrix(XG_WORLD_ACTOR_SPRITES_BODY_SCRATCH +
                                         0x20u,
                                     0);
    world_native_put_u32(UINT32_C(0x8009be3c), WORLD_NATIVE_CONTEXT);
    world_native_put_u32(WORLD_NATIVE_CONTEXT + 0x70u, WORLD_NATIVE_OT);
    world_native_put_u32(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR,
                         WORLD_NATIVE_PACKETS);
    world_native_put_u32(XG_WORLD_ACTOR_SPRITES_PACKET_LIMIT,
                         WORLD_NATIVE_PACKETS + 0x100u);
    world_native_put_u32(WORLD_NATIVE_OT + 32u * 4u,
                          UINT32_C(0xab123456));
    world_native_put_u32(
        entry_stack_pointer - 0x48u + 0x18u,
        XG_WORLD_ACTOR_SPRITES_RESIDENT_CALLER_RETURN);
    world_native_put_u32(
        entry_stack_pointer - 0x48u + 0x40u,
        XG_WORLD_ACTOR_SPRITES_WORLD_CALLER_RETURN);
    cpu->gpr[29] = entry_stack_pointer;
    cpu->gpr[31] = XG_WORLD_ACTOR_SPRITES_WORLD_CALLER_RETURN;
}

static int test_world_actor_native_seam_is_context_bound_and_atomic(void) {
    CPUState cpu;
    GuestRenderCompletedState completed = {0};
    GuestRenderTransactionPendingSnapshot pending = {0};
    PsxXgRenderWorldNativeSnapshot actor = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_actor_native_fixture(&cpu, WORLD_NATIVE_STACK);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_WORLD_ENTRY, UINT32_C(0x3c02800a)));
    CHECK(world_native_write_count == 0u);

    cpu.gpr[16] = WORLD_NATIVE_ACTOR;
    cpu.gpr[17] = WORLD_NATIVE_OT + 32u * 4u;
    cpu.gpr[29] = WORLD_NATIVE_STACK - 0x48u;
    cpu.gpr[31] = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        UINT32_C(0x02002021)));
    CHECK(world_native_write_count == 0u);
    CHECK(psx_xg_render_auth_runtime_test_materialize_world_actor_original(
        &cpu));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e2e0), UINT32_C(0x8fbf0018)));
    CHECK(world_native_read_word(XG_WORLD_ACTOR_SPRITES_PACKET_CURSOR) ==
          WORLD_NATIVE_PACKETS + 0x50u);
    CHECK(world_native_read_word(WORLD_NATIVE_OT + 32u * 4u) ==
          (UINT32_C(0xab000000) |
           ((WORLD_NATIVE_PACKETS + 0x28u) & UINT32_C(0x00ffffff))));
    CHECK(world_native_write_count != 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 2u);
    psx_xg_render_auth_world_actor_sprites_native_snapshot(&actor);
    CHECK(actor.native_cutover_count == 1u);
    CHECK(actor.native_primitive_count == 2u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 2u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_actor_native_fixture(&cpu, WORLD_NATIVE_STACK);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_WORLD_ENTRY, UINT32_C(0x3c02800a)));
    cpu.gpr[16] = WORLD_NATIVE_ACTOR;
    cpu.gpr[17] = WORLD_NATIVE_OT + 32u * 4u;
    cpu.gpr[29] = WORLD_NATIVE_STACK - 0x44u;
    cpu.gpr[31] = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        UINT32_C(0x02002021)));
    CHECK(world_native_write_count == 0u);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    psx_xg_render_auth_world_actor_sprites_native_snapshot(&actor);
    CHECK(actor.native_cutover_count == 0u);
    CHECK(actor.native_primitive_count == 0u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_actor_native_fixture(&cpu, WORLD_NATIVE_SCRATCH_STACK);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_WORLD_ENTRY, UINT32_C(0x3c02800a)));
    cpu.gpr[16] = WORLD_NATIVE_ACTOR;
    cpu.gpr[17] = WORLD_NATIVE_OT + 32u * 4u;
    cpu.gpr[29] = WORLD_NATIVE_SCRATCH_STACK - 0x48u;
    cpu.gpr[31] = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        UINT32_C(0x02002021)));
    CHECK(psx_xg_render_auth_runtime_test_materialize_world_actor_original(
        &cpu));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e2e0), UINT32_C(0x8fbf0018)));
    cpu.gpr[29] = WORLD_NATIVE_SCRATCH_STACK - 0x28u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80085f38), UINT32_C(0x8fbf0020)));
    psx_xg_render_auth_world_actor_sprites_native_snapshot(&actor);
    CHECK(actor.native_cutover_count == 1u);
    CHECK(actor.native_primitive_count == 2u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 2u);
    psx_xg_render_auth_scene_boundary();

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    configure_world_actor_native_fixture(&cpu, WORLD_NATIVE_SCRATCH_STACK);
    cpu.gpr[16] = WORLD_NATIVE_ACTOR;
    cpu.gpr[17] = WORLD_NATIVE_OT + 32u * 4u;
    cpu.gpr[29] = WORLD_NATIVE_SCRATCH_STACK - 0x48u;
    cpu.gpr[31] = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM;
    world_native_put_u32(cpu.gpr[29] + 0x18u, UINT32_C(0x8007622c));
    world_native_put_u32(cpu.gpr[29] + 0x40u, UINT32_C(0x00000c00));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        UINT32_C(0x02002021)));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_CONTINUATION,
        UINT32_C(0x8fbf0018)));
    CHECK(world_native_write_count == 0u);

    configure_world_actor_native_fixture(&cpu, WORLD_NATIVE_SCRATCH_STACK);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_WORLD_ENTRY, UINT32_C(0x3c02800a)));
    cpu.gpr[16] = WORLD_NATIVE_ACTOR;
    cpu.gpr[17] = WORLD_NATIVE_OT + 32u * 4u;
    cpu.gpr[29] = WORLD_NATIVE_SCRATCH_STACK - 0x48u;
    cpu.gpr[31] = XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, XG_WORLD_ACTOR_SPRITES_PREPARED_SEAM,
        UINT32_C(0x02002021)));
    CHECK(psx_xg_render_auth_runtime_test_materialize_world_actor_original(
        &cpu));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e2e0), UINT32_C(0x8fbf0018)));
    cpu.gpr[29] = WORLD_NATIVE_SCRATCH_STACK - 0x28u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80085f38), UINT32_C(0x8fbf0020)));
    psx_xg_render_auth_world_actor_sprites_native_snapshot(&actor);
    CHECK(actor.native_cutover_count == 1u);
    CHECK(actor.native_primitive_count == 2u);
    psx_xg_render_auth_before_gpu_submission();
    CHECK(guest_render_bridge_present(&completed) == GUEST_RENDER_OK);
    CHECK(completed.binding_count == 2u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_family_execution_census_is_exact_instruction_bound(void) {
    static const struct {
        uint32_t pc;
        uint32_t instruction;
    } entries[] = {
        { UINT32_C(0x800983a0), UINT32_C(0x27bdff90) },
        { UINT32_C(0x800848f4), UINT32_C(0x24020800) },
        { UINT32_C(0x80085cdc), UINT32_C(0x3c02800a) },
        { UINT32_C(0x800747dc), UINT32_C(0x3c02800a) },
        { UINT32_C(0x80086798), UINT32_C(0x3c02800a) },
        { UINT32_C(0x80089c78), UINT32_C(0x27bdffb0) },
        { UINT32_C(0x8008615c), UINT32_C(0x27bdffd8) },
        { UINT32_C(0x80073b04), UINT32_C(0x27bdffc0) },
        { UINT32_C(0x800737ec), UINT32_C(0x27bdffb8) },
        { UINT32_C(0x800740b8), UINT32_C(0x27bdffc8) },
    };
    CPUState cpu = { 0 };
    PsxXgRenderWorldExecutionSnapshot snapshot = { 0 };
    uint32_t index;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80071a58), UINT32_C(0x3c02800a)));
    for (index = 0u; index < sizeof(entries) / sizeof(entries[0]); ++index)
        CHECK(!psx_xg_render_auth_native_ft4_bypass(
            &cpu, entries[index].pc, entries[index].instruction));
    psx_xg_render_auth_world_execution_snapshot(&snapshot);
    CHECK(snapshot.full_dispatcher_count == 1u);
    CHECK(snapshot.observed_family_mask == UINT32_C(0x3ff));
    CHECK(snapshot.instruction_mismatch_mask == 0u);
    CHECK(!snapshot.overflowed);
    for (index = 0u; index < PSX_XG_RENDER_WORLD_FAMILY_COUNT; ++index)
        CHECK(snapshot.family_entry_count[index] == 1u);

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800848f4), UINT32_C(0x24020801)));
    psx_xg_render_auth_world_execution_snapshot(&snapshot);
    CHECK(snapshot.family_entry_count[PSX_XG_RENDER_WORLD_MODELS] == 1u);
    CHECK(snapshot.instruction_mismatch_mask == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_horizon_shadow_matches_atomic_transaction(void) {
    CPUState cpu;
    PsxXgRenderWorldHorizonShadowSnapshot snapshot = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_world_horizon_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b04), UINT32_C(0x27bdffc0)));
    psx_xg_render_auth_world_horizon_shadow_snapshot(&snapshot);
    CHECK(snapshot.begin_count == 1u);
    CHECK(snapshot.completion_count == 0u);
    CHECK(snapshot.source_read_count == 29u);
    CHECK(snapshot.source_read_bytes == 114u);
    CHECK(snapshot.last_ot_bucket == 248u);
    CHECK(snapshot.pending);
    CHECK(!snapshot.blocked);

    materialize_world_horizon_output(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073e0c), UINT32_C(0x8fbf003c)));
    psx_xg_render_auth_world_horizon_shadow_snapshot(&snapshot);
    CHECK(snapshot.begin_count == 1u);
    CHECK(snapshot.completion_count == 1u);
    CHECK(snapshot.accepted_invocation_count == 1u);
    CHECK(snapshot.primitive_count == 2u);
    CHECK(snapshot.match_count == 1u);
    CHECK(snapshot.mismatch_count == 0u);
    CHECK(snapshot.source_capture_failure_count == 0u);
    CHECK(snapshot.payload_mismatch_count == 0u);
    CHECK(snapshot.geometry_mismatch_count == 0u);
    CHECK(snapshot.tag_mismatch_count == 0u);
    CHECK(snapshot.ot_mismatch_count == 0u);
    CHECK(snapshot.texture_window_mismatch_count == 0u);
    CHECK(snapshot.first_payload_mismatch.field_bits == 0u);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_horizon_shadow_reports_payload_mismatch(void) {
    CPUState cpu;
    PsxXgRenderWorldHorizonShadowSnapshot snapshot = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_world_horizon_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b04), UINT32_C(0x27bdffc0)));
    materialize_world_horizon_output(&cpu);
    world_store_word(world_horizon_packet_address(0u) + 12u,
                     UINT32_C(0x7f910001));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073e0c), UINT32_C(0x8fbf003c)));
    psx_xg_render_auth_world_horizon_shadow_snapshot(&snapshot);
    CHECK(snapshot.completion_count == 1u);
    CHECK(snapshot.match_count == 0u);
    CHECK(snapshot.mismatch_count == 1u);
    CHECK(snapshot.payload_mismatch_count == 1u);
    CHECK(snapshot.first_mismatch_packet == world_horizon_packet_address(0u));
    CHECK(snapshot.first_payload_mismatch.field_bits != 0u);
    CHECK(!snapshot.blocked);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_horizon_shadow_fails_closed(void) {
    CPUState cpu;
    PsxXgRenderWorldHorizonShadowSnapshot snapshot = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_world_horizon_cpu(&cpu);
    cpu.gpr[31] += 4u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b04), UINT32_C(0x27bdffc0)));
    psx_xg_render_auth_world_horizon_shadow_snapshot(&snapshot);
    CHECK(snapshot.begin_count == 0u);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);

    configure_world_horizon_cpu(&cpu);
    world_store_word(UINT32_C(0x8009a300),
                     pack_s16(-4095, -896));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80073b04), UINT32_C(0x27bdffc0)));
    psx_xg_render_auth_world_horizon_shadow_snapshot(&snapshot);
    CHECK(snapshot.begin_count == 1u);
    CHECK(snapshot.source_capture_failure_count == 1u);
    CHECK(snapshot.blocked);
    CHECK(snapshot.blocker == 92u);
    CHECK(!snapshot.pending);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_effects_shadow_matches_value_only_source(void) {
    CPUState cpu;
    PsxXgRenderWorldEffectsShadowSnapshot snapshot = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_world_effects_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80089c78), UINT32_C(0x27bdffb0)));
    psx_xg_render_auth_world_effects_shadow_snapshot(&snapshot);
    CHECK(snapshot.begin_count == 1u);
    CHECK(snapshot.completion_count == 0u);
    CHECK(snapshot.active_source_count == 1u);
    CHECK(snapshot.source_capture_failure_count == 0u);
    CHECK(snapshot.source_read_count == 385u);
    CHECK(snapshot.source_read_bytes == 1024u);
    CHECK(snapshot.pending);
    CHECK(!snapshot.blocked);

    materialize_world_effects_output();
    complete_world_effects_cpu(&cpu, 1u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8008a294), UINT32_C(0x8fbf004c)));
    psx_xg_render_auth_world_effects_shadow_snapshot(&snapshot);
    CHECK(snapshot.begin_count == 1u);
    CHECK(snapshot.completion_count == 1u);
    CHECK(snapshot.primitive_count == 1u);
    CHECK(snapshot.candidate_count == 1u);
    CHECK(snapshot.match_count == 1u);
    CHECK(snapshot.mismatch_count == 0u);
    CHECK(snapshot.invocation_match_count == 1u);
    CHECK(snapshot.invocation_mismatch_count == 0u);
    CHECK(snapshot.count_mismatch_count == 0u);
    CHECK(snapshot.geometry_mismatch_count == 0u);
    CHECK(snapshot.payload_mismatch_count == 0u);
    CHECK(snapshot.tag_mismatch_count == 0u);
    CHECK(snapshot.ot_mismatch_count == 0u);
    CHECK(snapshot.last_primitive_count == 1u);
    CHECK(snapshot.last_candidate_count == 1u);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_world_effects_shadow_rejects_invalid_cursor(void) {
    CPUState cpu;
    PsxXgRenderWorldEffectsShadowSnapshot snapshot = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_world_effects_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x80089c78), UINT32_C(0x27bdffb0)));
    complete_world_effects_cpu(&cpu, 1u);
    ++cpu.gpr[19];
    ++cpu.gpr[18];
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8008a294), UINT32_C(0x8fbf004c)));
    psx_xg_render_auth_world_effects_shadow_snapshot(&snapshot);
    CHECK(snapshot.begin_count == 1u);
    CHECK(snapshot.completion_count == 0u);
    CHECK(snapshot.primitive_count == 0u);
    CHECK(snapshot.blocked);
    CHECK(snapshot.blocker == 103u);
    CHECK(!snapshot.pending);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_model_ft4_raw_shadow_reconstructs_source(void) {
    CPUState cpu;
    PsxXgRenderModelFt4ShadowSnapshot snapshot = { 0 };
    XgModelFt4RawSource source = { 0 };
    XgModelFt4RawRecord record;
    uint32_t vertex;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_model_ft4_shadow_cpu(&cpu);
    cpu.gpr[7] = XG_MODEL_FT4_RAW_DISPATCH_FARTHEST;
    model_shadow_store_word(MODEL_SHADOW_OT + 8u * 4u,
                            UINT32_C(0x00123456));
    model_shadow_store_word(UINT32_C(0x80059308), 0x123u);
    model_shadow_store_word(UINT32_C(0x8005930c), 0x42u);
    model_shadow_store_word(UINT32_C(0x80059424),
                            MODEL_SHADOW_PACKET + 0x100u);
    cpu.gpr[16] = MODEL_SHADOW_MATERIAL + 8u;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002d100), UINT32_C(0x3c048006)));
    model_shadow_store_word(UINT32_C(0x80059424), MODEL_SHADOW_PACKET);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002c700), UINT32_C(0x27bdffd0)));
    cpu.gpr[4] = MODEL_SHADOW_TOPOLOGY + 4u;
    cpu.gpr[5] = 1u;
    cpu.gpr[31] = UINT32_C(0x8002c86c);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002e688), UINT32_C(0x34190008)));
    psx_xg_render_auth_model_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.pending);
    CHECK(!snapshot.blocked);

    source.vertices[0] = (XgHost3dVector){ -64, -64, 0, 0u };
    source.vertices[1] = (XgHost3dVector){ 64, -64, 0, 0u };
    source.vertices[2] = (XgHost3dVector){ -64, 64, 0, 0u };
    source.vertices[3] = (XgHost3dVector){ 64, 64, 0, 0u };
    source.projection.rotation[0][0] = 4096;
    source.projection.rotation[1][1] = 4096;
    source.projection.rotation[2][2] = 4096;
    source.projection.translation[2] = 512;
    source.projection.screen_offset_x = 160 << 16;
    source.projection.screen_offset_y = 120 << 16;
    source.projection.projection_distance = 256u;
    source.projection.average_z_scale4 = 1024;
    source.material.tpage = 0x123u;
    source.material.texture_page_x = 3u;
    source.material.texture_depth = XG_RENDER_IR_TEXTURE_15_BIT;
    source.material.blend_mode = XG_RENDER_IR_BLEND_ADD;
    source.material.clut_x = 0x20u;
    source.material.clut_y = 1u;
    source.material.draw_area_right = 319u;
    source.material.draw_area_bottom = 239u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = true;
    source.material.semi_transparent = true;
    source.uv[0][0] = 0u;
    source.uv[0][1] = 0u;
    source.uv[1][0] = 0x10u;
    source.uv[1][1] = 0x20u;
    source.uv[2][0] = 0x20u;
    source.uv[2][1] = 0u;
    source.uv[3][0] = 0x30u;
    source.uv[3][1] = 0x40u;
    source.screen_right = 319;
    source.packed_screen_bottom = UINT32_C(0x00ee0000);
    source.packet_address = MODEL_SHADOW_PACKET;
    source.ordering_shift = 4u;
    source.dispatch_mode = XG_MODEL_FT4_RAW_DISPATCH_FARTHEST;
    CHECK(xg_model_ft4_raw_build(&source, &record) == XG_MODEL_FT4_RAW_OK);
    CHECK(record.accepted);
    CHECK(record.ordering_bucket == 8u);

    model_shadow_store_word(MODEL_SHADOW_PACKET, UINT32_C(0x09123456));
    model_shadow_store_word(MODEL_SHADOW_PACKET + 4u,
                            UINT32_C(0x2f808080));
    model_shadow_store_word(MODEL_SHADOW_PACKET + 12u,
                            UINT32_C(0x00420000));
    model_shadow_store_word(MODEL_SHADOW_PACKET + 20u,
                            UINT32_C(0x01232010));
    model_shadow_store_word(MODEL_SHADOW_PACKET + 28u,
                            UINT32_C(0x00000020));
    model_shadow_store_word(MODEL_SHADOW_PACKET + 36u,
                            UINT32_C(0x00004030));
    for (vertex = 0u; vertex < 4u; ++vertex) {
        model_shadow_store_word(MODEL_SHADOW_PACKET + 8u + vertex * 8u,
            (uint16_t)record.vertices[vertex].x |
            ((uint32_t)(uint16_t)record.vertices[vertex].y << 16u));
    }
    model_shadow_store_word(MODEL_SHADOW_OT + 8u * 4u,
                            MODEL_SHADOW_PACKET & UINT32_C(0x00ffffff));
    model_shadow_store_word(UINT32_C(0x80059424),
                            (MODEL_SHADOW_PACKET + 0x28u) &
                                UINT32_C(0x00ffffff));
    model_shadow_store_word(UINT32_C(0x80059578), 8u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002c86c), UINT32_C(0x86230002)));
    psx_xg_render_auth_model_ft4_shadow_snapshot(&snapshot);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);
    CHECK(snapshot.invocation_count == 1u);
    CHECK(snapshot.primitive_count == 1u);
    CHECK(snapshot.match_count == 1u);
    CHECK(snapshot.mismatch_count == 0u);
    CHECK(snapshot.payload_mismatch_count == 0u);
    CHECK(snapshot.geometry_mismatch_count == 0u);
    CHECK(snapshot.tag_mismatch_count == 0u);
    CHECK(snapshot.ot_mismatch_count == 0u);
    CHECK(snapshot.cursor_mismatch_count == 0u);
    CHECK(snapshot.counter_mismatch_count == 0u);
    CHECK(snapshot.template_capture_count == 1u);
    CHECK(snapshot.template_hit_count == 1u);
    CHECK(snapshot.template_miss_count == 0u);
    CHECK(snapshot.prepare_failure_detail == 0u);
    CHECK(snapshot.first_payload_mismatch.field_bits == 0u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8002c86c), UINT32_C(0x86230002)));
    psx_xg_render_auth_model_ft4_shadow_snapshot(&snapshot);
    CHECK(!snapshot.blocked);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x800257dc), UINT32_C(0x8fbf0044)));
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_model_ft3_native_cull_uses_wide_margin(void) {
    static const int32_t translations[2] = { -400, 400 };

    for (uint32_t side = 0u; side < 2u; ++side) {
        for (uint32_t wide = 0u; wide < 2u; ++wide) {
            CPUState cpu;
            PsxXgRenderModelFt3ShadowSnapshot snapshot = { 0 };

            CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
            set_matching_runtime_identity();
            note_matching_field5_candidate();
            xg_host_3d_configure_native_view(wide != 0u, 54 << 16);
            configure_model_ft4_shadow_cpu(&cpu);
            model_shadow_store_word(MODEL_SHADOW_TOPOLOGY,
                                    UINT32_C(0x00010005));
            model_shadow_store_word(MODEL_SHADOW_MATERIAL + 8u,
                                    UINT32_C(0x25808080));
            model_shadow_store_word(
                MODEL_SHADOW_SP + 0x10u + 5u * 4u,
                (uint32_t)translations[side]);
            model_shadow_store_word(UINT32_C(0x80059424),
                                    MODEL_SHADOW_PACKET + 0x100u);
            cpu.gpr[16] = MODEL_SHADOW_MATERIAL + 8u;
            CHECK(!psx_xg_render_auth_native_ft4_bypass(
                &cpu, UINT32_C(0x8002da00), UINT32_C(0x8fbf0014)));
            model_shadow_store_word(UINT32_C(0x80059424),
                                    MODEL_SHADOW_PACKET);
            CHECK(!psx_xg_render_auth_native_ft4_bypass(
                &cpu, UINT32_C(0x8002c700), UINT32_C(0x27bdffd0)));
            cpu.gpr[4] = MODEL_SHADOW_TOPOLOGY + 4u;
            cpu.gpr[5] = 1u;
            cpu.gpr[31] = UINT32_C(0x8002c86c);
            CHECK(!psx_xg_render_auth_native_ft4_bypass(
                &cpu, UINT32_C(0x8002e484), UINT32_C(0x34190008)));
            psx_xg_render_auth_model_ft3_shadow_snapshot(&snapshot);
            CHECK(!snapshot.blocked);
            CHECK(snapshot.prepare_failure_detail == 0u);
            CHECK(snapshot.native_cutover_count == side * 2u + wide + 1u);
            CHECK(snapshot.native_primitive_count == side + wide);
            psx_xg_render_auth_scene_boundary();
        }
    }
    xg_host_3d_configure_native_view(0, 0);
    return 1;
}

static int test_battle_sprite_ft4_shadow_uses_descriptor_source(void) {
    CPUState cpu;
    PsxXgRenderSpriteFt4ShadowSnapshot snapshot = { 0 };
    XgSpriteFt4Source source = { 0 };
    XgSpriteFt4Record record;
    uint32_t vertex;

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_sprite_ft4_shadow_cpu(&cpu);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e298), UINT32_C(0x27bdffe0)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.context_active);
    CHECK(snapshot.caller_count == 1u);
    CHECK(snapshot.empty_caller_count == 0u);
    CHECK(snapshot.last_caller == UINT32_C(0x800babc4));
    CHECK(snapshot.last_sprite_address == SPRITE_SHADOW_INSTANCE);
    CHECK(snapshot.last_data_address == SPRITE_SHADOW_DATA);
    CHECK(snapshot.last_descriptor_address == SPRITE_SHADOW_DESCRIPTOR);
    CHECK(snapshot.last_primitive_count == 1u);
    CHECK(snapshot.blocker_detail == 0u);
    cpu.gpr[16] = SPRITE_SHADOW_PACKET;
    cpu.gpr[18] = SPRITE_SHADOW_VERTICES;
    cpu.gpr[19] = SPRITE_SHADOW_DESCRIPTOR;
    cpu.gpr[20] = SPRITE_SHADOW_INSTANCE;
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e86c), UINT32_C(0x0c0129cf)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.context_active);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);

    source.vertices[0] = (XgHost3dVector){ -32, -48, 0, 0u };
    source.vertices[1] = (XgHost3dVector){ 32, -48, 0, 0u };
    source.vertices[2] = (XgHost3dVector){ -32, 48, 0, 0u };
    source.vertices[3] = (XgHost3dVector){ 32, 48, 0, 0u };
    source.projection.rotation[0][0] = 4096;
    source.projection.rotation[1][1] = 4096;
    source.projection.rotation[2][2] = 4096;
    source.projection.translation[2] = 512;
    source.projection.screen_offset_x = 160 << 16;
    source.projection.screen_offset_y = 120 << 16;
    source.projection.projection_distance = 256u;
    source.material.tpage = 0x23u;
    source.material.texture_page_x = 3u;
    source.material.blend_mode = XG_RENDER_IR_BLEND_ADD;
    source.material.clut_x = 0x20u;
    source.material.clut_y = 1u;
    source.material.draw_area_right = 319u;
    source.material.draw_area_bottom = 239u;
    source.material.shading = XG_RENDER_IR_SHADING_FLAT;
    source.material.textured = true;
    source.material.raw_texture = true;
    source.material.semi_transparent = true;
    source.color[0] = source.color[1] = source.color[2] = 0x80u;
    source.uv[1][0] = source.uv[3][0] = 31u;
    source.uv[2][1] = source.uv[3][1] = 47u;
    source.packet_vertex_for_projection[0] = 0u;
    source.packet_vertex_for_projection[1] = 1u;
    source.packet_vertex_for_projection[2] = 3u;
    source.packet_vertex_for_projection[3] = 2u;
    CHECK(xg_sprite_ft4_build(&source, &record) == XG_SPRITE_FT4_OK);
    for (vertex = 0u; vertex < 4u; ++vertex) {
        model_shadow_store_word(SPRITE_SHADOW_PACKET + 8u + vertex * 8u,
            (uint16_t)record.vertices[vertex].x |
            ((uint32_t)(uint16_t)record.vertices[vertex].y << 16u));
    }
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e874), UINT32_C(0x92680006)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.pending);
    CHECK(!snapshot.blocked);
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 4u,
                            UINT32_C(0x2f808080));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 12u,
                            UINT32_C(0x00420000));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 20u,
                            UINT32_C(0x0023001f));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 28u,
                            UINT32_C(0x00002f00));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 36u,
                            UINT32_C(0x00002f1f));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e8d8), UINT32_C(0x8e82003c)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);
    CHECK(snapshot.projection_count == 1u);
    CHECK(snapshot.match_count == 1u);
    CHECK(snapshot.mismatch_count == 0u);
    CHECK(snapshot.geometry_mismatch_count == 0u);
    CHECK(snapshot.payload_mismatch_count == 0u);
    CHECK(snapshot.first_payload_mismatch.field_bits == 0u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e2c0), UINT32_C(0x8e02003c)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(!snapshot.context_active);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_battle_sprite_ft4_shadow_reports_descriptor_blocker(void) {
    CPUState cpu;
    PsxXgRenderSpriteFt4ShadowSnapshot snapshot = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_sprite_ft4_shadow_cpu(&cpu);
    model_shadow_store_word(
        SPRITE_SHADOW_DATA + 0x30u, UINT32_C(0x80200000));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e298), UINT32_C(0x27bdffe0)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.caller_count == 1u);
    CHECK(snapshot.last_caller == UINT32_C(0x800babc4));
    CHECK(snapshot.last_sprite_address == SPRITE_SHADOW_INSTANCE);
    CHECK(snapshot.last_data_address == SPRITE_SHADOW_DATA);
    CHECK(snapshot.last_descriptor_address == UINT32_C(0x80200000));
    CHECK(snapshot.last_primitive_count == 1u);
    CHECK(snapshot.blocked);
    CHECK(snapshot.blocker == 82u);
    CHECK(snapshot.blocker_detail == 8u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_battle_sprite_ft4_shadow_skips_empty_callers(void) {
    CPUState cpu;
    PsxXgRenderSpriteFt4ShadowSnapshot snapshot = { 0 };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    configure_sprite_ft4_shadow_cpu(&cpu);
    model_shadow_store_word(SPRITE_SHADOW_INSTANCE + 0x40u, 0u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e298), UINT32_C(0x27bdffe0)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.caller_count == 1u);
    CHECK(snapshot.empty_caller_count == 1u);
    CHECK(!snapshot.context_active);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);
    model_shadow_store_word(SPRITE_SHADOW_INSTANCE + 0x40u, 4u);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e298), UINT32_C(0x27bdffe0)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.caller_count == 2u);
    CHECK(snapshot.empty_caller_count == 1u);
    CHECK(snapshot.context_active);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_sprite_ft4_native_contract_is_wrapper_scoped(void) {
    CPUState cpu;
    PsxXgRenderSpriteFt4ShadowSnapshot snapshot = { 0 };
    static const uint32_t xy[4] = {
        UINT32_C(0x00600090), UINT32_C(0x006000b0),
        UINT32_C(0x009000b0), UINT32_C(0x00900090),
    };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x8001e3d8)));
    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x8001e988)));
    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x80045ed0)));
    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x80079784)));
    CHECK(psx_xg_render_auth_native_cutover_pc_relevant(
        UINT32_C(0x8007da44)));
    configure_sprite_ft4_shadow_cpu(&cpu);
    cpu.gpr[31] = UINT32_C(0x80123458);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e298), UINT32_C(0x27bdffe0)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.context_active);
    CHECK(snapshot.last_caller == UINT32_C(0x80123458));

    cpu.gpr[16] = SPRITE_SHADOW_PACKET;
    cpu.gpr[18] = SPRITE_SHADOW_VERTICES;
    cpu.gpr[19] = SPRITE_SHADOW_DESCRIPTOR;
    cpu.gpr[20] = SPRITE_SHADOW_INSTANCE;
    for (uint32_t vertex = 0u; vertex < 4u; ++vertex)
        model_shadow_store_word(
            SPRITE_SHADOW_PACKET + 8u + vertex * 8u, xy[vertex]);
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e874), UINT32_C(0x92680006)));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 4u,
                            UINT32_C(0x2f808080));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 12u,
                            UINT32_C(0x00420000));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 20u,
                            UINT32_C(0x0023001f));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 28u,
                            UINT32_C(0x00002f00));
    model_shadow_store_word(SPRITE_SHADOW_PACKET + 36u,
                            UINT32_C(0x00002f1f));
    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e8d8), UINT32_C(0x8e82003c)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.match_count == 1u);
    CHECK(snapshot.native_cutover_count == 0u);
    CHECK(snapshot.native_primitive_count == 0u);

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e988), UINT32_C(0x8fbf0094)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(snapshot.context_active);
    CHECK(snapshot.native_cutover_count == 0u);

    CHECK(!psx_xg_render_auth_native_ft4_bypass(
        &cpu, UINT32_C(0x8001e2c0), UINT32_C(0x8e02003c)));
    psx_xg_render_auth_sprite_ft4_shadow_snapshot(&snapshot);
    CHECK(!snapshot.context_active);
    CHECK(!snapshot.blocked);
    CHECK(snapshot.native_cutover_count == 1u);
    CHECK(snapshot.native_primitive_count == 1u);
    psx_xg_render_auth_before_gpu_submission();
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_resident_render_dependencies_register_memory_watches(void) {
    projected_watch_registered = false;
    projected_table_watch_registered = false;
    world_sky_watch_registered = false;
    world_sky_table_watch_registered = false;
    world_horizon_watch_registered = false;
    world_horizon_table_watch_registered = false;
    world_effects_watch_registered = false;
    model_ft4_watch_registered = false;
    sprite_ft4_watch_registered = false;
    psx_xg_render_auth_register_code_watches(record_render_code_watch);
    CHECK(projected_watch_registered);
    CHECK(projected_table_watch_registered);
    CHECK(world_sky_watch_registered);
    CHECK(world_sky_table_watch_registered);
    CHECK(world_horizon_watch_registered);
    CHECK(world_horizon_table_watch_registered);
    CHECK(world_effects_watch_registered);
    CHECK(model_ft4_watch_registered);
    CHECK(sprite_ft4_watch_registered);
    return 1;
}

static int test_resident_ft4_geometry_cold_warm_parity(void) {
    PsxXgRenderFt4Geometry cold = {0};
    PsxXgRenderFt4Geometry warm = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    CHECK(capture_ft4_geometry(XG_RENDER_AUTH_TIER_COLD_INTERPRETER, &cold));
    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    CHECK(capture_ft4_geometry(XG_RENDER_AUTH_TIER_WARM_NATIVE, &warm));
    CHECK(cold.sequence == 1u);
    CHECK(warm.sequence == 1u);
    CHECK(cold.packet_guest_address == 0x80101000u);
    CHECK(warm.packet_guest_address == cold.packet_guest_address);
    CHECK(memcmp(cold.x, warm.x, sizeof(cold.x)) == 0);
    CHECK(memcmp(cold.y, warm.y, sizeof(cold.y)) == 0);
    CHECK(cold.host_transformed);
    for (size_t index = 0u; index < 4u; ++index) {
        CHECK(cold.x[index] == producer_family_host_output.vertices[index].x);
        CHECK(cold.y[index] == producer_family_host_output.vertices[index].y);
    }
    CHECK(cold.tier == XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(warm.tier == XG_RENDER_AUTH_TIER_WARM_NATIVE);
    return 1;
}

static int test_resident_ft4_geometry_accepts_scratchpad_stack(void) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    CPUState cpu = {0};
    PsxXgRenderFt4GeometrySnapshot snapshot = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    psx_xg_render_auth_ft4_geometry_enable(true);
    CHECK(configure_host_geometry_cpu(&cpu, 0x80101000u));
    producer_family_stack_base = UINT32_C(0x1f8002c0);
    cpu.gpr[29] = producer_family_stack_base;
    CHECK(observe_source_site_cpu(&cpu,
                                  XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                  call_site, 0u));
    psx_xg_render_auth_ft4_geometry_snapshot(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(snapshot.pending);
    CHECK(!snapshot.blocked);
    CHECK(snapshot.host_transform_count == 1u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_native_ft4_bypass_accepts_scratchpad_outputs(void) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    CPUState cpu = {0};
    PsxXgRenderFt4GeometrySnapshot snapshot = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    psx_xg_render_auth_ft4_geometry_enable(true);
    CHECK(configure_host_geometry_cpu(&cpu, 0x80101000u));
    producer_family_stack_base = UINT32_C(0x1f8002c0);
    cpu.gpr[29] = producer_family_stack_base;
    CHECK(observe_source_site_cpu(&cpu,
                                  XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                  call_site, 0u));
    CHECK(psx_xg_render_auth_native_ft4_bypass(
        &cpu, call_site->pc, call_site->instruction));
    psx_xg_render_auth_ft4_geometry_snapshot(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(!snapshot.pending);
    CHECK(!snapshot.blocked);
    CHECK(snapshot.completed_count == 1u);
    CHECK(snapshot.queued_count == 1u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_resident_ft4_geometry_rejects_wrong_caller_and_stride(void) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    CPUState cpu = {0};
    PsxXgRenderFt4GeometrySnapshot snapshot = {0};

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    psx_xg_render_auth_ft4_geometry_enable(true);
    CHECK(configure_host_geometry_cpu(&cpu, 0x80101000u));
    CHECK(observe_source_site_cpu(&cpu,
                                  XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                  call_site, 0u));
    cpu.gpr[31] = 0x800769d4u;
    CHECK(!psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, 0x8004a7e8u,
        0xe90c0000u));
    psx_xg_render_auth_ft4_geometry_snapshot(&snapshot);
    CHECK(snapshot.blocked);
    CHECK(snapshot.queued_count == 0u);

    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    psx_xg_render_auth_ft4_geometry_enable(true);
    CHECK(configure_host_geometry_cpu(&cpu, 0x80101000u));
    CHECK(observe_source_site_cpu(&cpu,
                                  XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                  call_site, 0u));
    cpu.gpr[31] = 0x800769d0u;
    cpu.gpr[8] = 0x80101008u;
    cpu.gpr[9] = 0x80101010u;
    cpu.gpr[10] = 0x80101018u;
    cpu.gte_data[12] = producer_family_xy(
        producer_family_host_output.vertices[0].x,
        producer_family_host_output.vertices[0].y);
    cpu.gte_data[13] = producer_family_xy(
        producer_family_host_output.vertices[1].x,
        producer_family_host_output.vertices[1].y);
    cpu.gte_data[14] = producer_family_xy(
        producer_family_host_output.vertices[2].x,
        producer_family_host_output.vertices[2].y);
    cpu.gte_ctrl[31] = producer_family_host_output.rtpt_flags;
    CHECK(psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, 0x8004a7e8u,
        0xe90c0000u));
    CHECK(psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, 0x8004a7e8u,
        0xe90c0000u));
    cpu.gpr[8] = 0x80101024u;
    CHECK(!psx_xg_render_auth_resident_ft4_observe(
        &cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, 0x8004a814u,
        0xe90e0000u));
    psx_xg_render_auth_ft4_geometry_snapshot(&snapshot);
    CHECK(snapshot.blocked);
    CHECK(snapshot.completed_count == 0u);
    CHECK(snapshot.queued_count == 0u);
    return 1;
}

static uint32_t producer_family_xy(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16u);
}

static int produce_family_candidate_for_actor(
    CPUState *cpu, const XgRenderRuntimeVariantSourceSite *call_site,
    const XgRenderRuntimeVariantSourceSite *bucket_site,
    uint32_t packet_address, uint32_t ot_bucket, uint32_t actor_index) {
    GuestRenderBridgeSnapshot bridge = {0};
    uint32_t gte_data_before[32];
    uint32_t gte_ctrl_before[32];

    CHECK(configure_host_geometry_cpu_for_actor(cpu, packet_address,
                                                actor_index));
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    if (bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_NATIVE) {
        memcpy(gte_data_before, cpu->gte_data, sizeof(gte_data_before));
        memcpy(gte_ctrl_before, cpu->gte_ctrl, sizeof(gte_ctrl_before));
        CHECK(psx_xg_render_auth_native_ft4_bypass(
            cpu, UINT32_C(0x800765dc), UINT32_C(0xafa00028)));
        CHECK(cpu->pc == UINT32_C(0x80076a28));
        CHECK(memcmp(gte_data_before, cpu->gte_data,
                     sizeof(gte_data_before)) == 0);
        CHECK(memcmp(gte_ctrl_before, cpu->gte_ctrl,
                     sizeof(gte_ctrl_before)) == 0);
        CHECK((producer_family_packet_tag & UINT32_C(0x00ffffff)) ==
              UINT32_C(0x00123456));
        CHECK((producer_family_ot_word & UINT32_C(0x00ffffff)) ==
              (producer_family_packet_address & UINT32_C(0x00ffffff)));
        return 1;
    }
    CHECK(observe_source_site_cpu(cpu, XG_RENDER_AUTH_TIER_COLD_INTERPRETER,
                                  call_site, 0u));
    if (bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_SHADOW) {
        cpu->gpr[31] = 0x800769d0u;
        cpu->gpr[8] = packet_address + 8u;
        cpu->gpr[9] = packet_address + 16u;
        cpu->gpr[10] = packet_address + 24u;
        cpu->gte_data[12] = producer_family_xy(
            producer_family_host_output.vertices[0].x,
            producer_family_host_output.vertices[0].y);
        cpu->gte_data[13] = producer_family_xy(
            producer_family_host_output.vertices[1].x,
            producer_family_host_output.vertices[1].y);
        cpu->gte_data[14] = producer_family_xy(
            producer_family_host_output.vertices[2].x,
            producer_family_host_output.vertices[2].y);
        cpu->gte_ctrl[31] = producer_family_host_output.rtpt_flags;
        CHECK(psx_xg_render_auth_resident_ft4_observe(
            cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, 0x8004a7e8u,
            0xe90c0000u));
        CHECK(psx_xg_render_auth_resident_ft4_observe(
            cpu, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, 0x8004a7e8u,
            0xe90c0000u));
        cpu->gpr[8] = packet_address + 32u;
        cpu->gte_data[14] = producer_family_xy(
            producer_family_host_output.vertices[3].x,
            producer_family_host_output.vertices[3].y);
        cpu->gte_ctrl[31] = producer_family_host_output.rtps_flags;
        CHECK(psx_xg_render_auth_resident_ft4_observe(
            cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, 0x8004a814u,
            0xe90e0000u));
        CHECK(psx_xg_render_auth_resident_ft4_observe(
            cpu, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, 0x8004a814u,
            0xe90e0000u));
    }

    producer_family_packet_words[0] = 0x2e808080u;
    producer_family_packet_words[1] = producer_family_xy(
        producer_family_host_output.vertices[0].x,
        producer_family_host_output.vertices[0].y);
    producer_family_packet_words[2] = 0x3cd0e000u;
    producer_family_packet_words[3] = producer_family_xy(
        producer_family_host_output.vertices[1].x,
        producer_family_host_output.vertices[1].y);
    producer_family_packet_words[4] = 0x005ae00fu;
    producer_family_packet_words[5] = producer_family_xy(
        producer_family_host_output.vertices[2].x,
        producer_family_host_output.vertices[2].y);
    producer_family_packet_words[6] = 0x0000ef00u;
    producer_family_packet_words[7] = producer_family_xy(
        producer_family_host_output.vertices[3].x,
        producer_family_host_output.vertices[3].y);
    producer_family_packet_words[8] = 0x0000ef0fu;
    CHECK(psx_xg_render_auth_cold_source_observe_cpu(
        cpu, PSX_XG_RENDER_SOURCE_STAGE_PRE, bucket_site->pc,
        bucket_site->instruction, 0u));
    CHECK(psx_xg_render_auth_cold_source_observe_cpu(
        cpu, PSX_XG_RENDER_SOURCE_STAGE_COMMIT, bucket_site->pc,
        bucket_site->instruction, ot_bucket));
    return 1;
}

static int produce_family_candidate(
    CPUState *cpu, const XgRenderRuntimeVariantSourceSite *call_site,
    const XgRenderRuntimeVariantSourceSite *bucket_site,
    uint32_t packet_address, uint32_t ot_bucket) {
    return produce_family_candidate_for_actor(
        cpu, call_site, bucket_site, packet_address, ot_bucket, 0u);
}

static int test_producer_family_native_stages_authenticated_source(void) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    const XgRenderRuntimeVariantSourceSite *bucket_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[8];
    CPUState cpu = {0};
    PsxXgRenderProducerFamilySnapshot snapshot = {0};
    PsxXgRenderFt4GeometrySnapshot geometry_snapshot = {0};
    GuestRenderNativeStreamSnapshot stream_snapshot = {0};
    GuestRenderBridgeSnapshot bridge = {0};
    XgRenderAuthSnapshot auth_snapshot = {0};
    XgRenderAuth *auth = NULL;

    guest_render_transaction_test_reset();
    CHECK(reset_source_mode(GUEST_RENDER_RENDER_NATIVE));
    set_matching_runtime_identity();
    psx_xg_render_auth_source_reset();
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    psx_xg_render_auth_producer_family_enable(true);
    cpu.read_half = producer_family_read_half;
    cpu.read_word = producer_family_read_word;
    CHECK(produce_family_candidate_for_actor(
        &cpu, call_site, bucket_site, 0x800b06dcu, 250u, 0u));
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_CAPTURE_SITE, JAL_INSTRUCTION,
                                 FIELD5_CAPTURE_DELAY);
    CHECK(produce_family_candidate_for_actor(
        &cpu, call_site, bucket_site, 0x800b071cu, 250u, 1u));
    psx_xg_render_auth_producer_family_snapshot(&snapshot);
    CHECK(snapshot.enabled);
    CHECK(!snapshot.blocked);
    CHECK(snapshot.blocker == 0u);
    CHECK(snapshot.source_capture_count == 2u);
    CHECK(snapshot.source_incomplete_count == 0u);
    CHECK(snapshot.geometry_count == 2u);
    CHECK(snapshot.candidate_count == 2u);
    CHECK(snapshot.match_count == 0u);
    CHECK(snapshot.mismatch_count == 0u);
    CHECK(snapshot.last_runtime_result == 0u);
    psx_xg_render_auth_ft4_geometry_snapshot(&geometry_snapshot);
    CHECK(geometry_snapshot.host_transform_count == 2u);
    CHECK(geometry_snapshot.oracle_match_count == 0u);
    CHECK(geometry_snapshot.oracle_mismatch_count == 0u);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.state_open && bridge.producer_open);
    CHECK(bridge.binding_count == 2u);
    CHECK(guest_render_native_stream_snapshot(&stream_snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(stream_snapshot.staged_count == 2u);
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!auth_snapshot.scene_aborted);
    CHECK(auth_snapshot.native_item_count == 0u);
    CHECK(!auth_snapshot.native_use_permitted);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 FIELD5_RETURN_SITE, 0u, 0u);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!auth_snapshot.scene_aborted);
    CHECK(auth_snapshot.native_item_count == 2u);
    CHECK(auth_snapshot.native_use_permitted);
    CHECK(auth_snapshot.effective_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(!bridge.state_open && !bridge.producer_open);
    CHECK(bridge.binding_count == 2u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_producer_family_shadow_compares_without_staging(void) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    const XgRenderRuntimeVariantSourceSite *bucket_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[8];
    CPUState cpu = {0};
    PsxXgRenderProducerFamilySnapshot family = {0};
    GuestRenderTransactionPendingSnapshot pending = {0};
    GuestRenderBridgeSnapshot bridge = {0};
    XgRenderAuthSnapshot auth_snapshot = {0};
    XgRenderAuth *auth = NULL;

    guest_render_transaction_test_reset();
    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    psx_xg_render_auth_producer_family_enable(true);
    cpu.read_half = producer_family_read_half;
    cpu.read_word = producer_family_read_word;
    CHECK(produce_family_candidate(&cpu, call_site, bucket_site,
                                   0x800b06dcu, 250u));
    CHECK(produce_family_candidate(&cpu, call_site, bucket_site,
                                   0x800b071cu, 250u));
    psx_xg_render_auth_producer_family_snapshot(&family);
    CHECK(family.enabled && !family.blocked);
    CHECK(family.blocker == 0u);
    CHECK(family.geometry_count == 2u);
    CHECK(family.candidate_count == 2u);
    CHECK(family.match_count == 2u);
    CHECK(family.mismatch_count == 0u);
    CHECK(family.first_mismatch_word == UINT32_MAX);
    CHECK(family.first_mismatch_byte == UINT32_MAX);
    CHECK(guest_render_transaction_pending_snapshot(&pending) ==
          GUEST_RENDER_TRANSACTION_OK);
    CHECK(pending.binding_count == 0u);
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!auth_snapshot.scene_aborted);
    CHECK(auth_snapshot.reject_reason == XG_RENDER_AUTH_REJECT_NONE);
    CHECK(auth_snapshot.native_item_count == 0u);
    CHECK(!auth_snapshot.native_use_permitted);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.state_open && bridge.producer_open);
    CHECK(bridge.binding_count == 0u);
    psx_xg_render_auth_scene_boundary();
    return 1;
}

static int test_runtime_render_modes_are_independent_and_gate_staging(void) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    const XgRenderRuntimeVariantSourceSite *bucket_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[8];
    const GuestRenderRenderMode modes[] = {
        GUEST_RENDER_RENDER_ORIGINAL,
        GUEST_RENDER_RENDER_SHADOW,
        GUEST_RENDER_RENDER_NATIVE,
    };

    for (size_t index = 0u; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        CPUState cpu = {0};
        GuestRenderBridgeSnapshot bridge = {0};
        GuestRenderTransactionPendingSnapshot pending = {0};
        GuestRenderNativeStreamSnapshot stream_snapshot = {0};
        XgRenderAuthSnapshot auth_snapshot = {0};
        XgRenderAuth *auth = NULL;

        psx_xg_render_auth_runtime_test_reset();
        guest_render_transaction_test_reset();
        presentation_gate_succeeds = true;
        presentation_gate_saw_closed_state = false;
        CHECK(psx_xg_render_auth_configure(
            GUEST_RENDER_TIMING_NATIVE_59_94, modes[index],
            test_presentation_gate, NULL));
        set_matching_runtime_identity();
        psx_xg_render_auth_cold_enable(true);
        begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
        psx_xg_render_auth_producer_family_enable(true);
        CHECK(produce_family_candidate(&cpu, call_site, bucket_site,
                                       0x800b06dcu, 250u));
        CHECK(presentation_gate_saw_closed_state);
        CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
        CHECK(bridge.modes.requested_timing_mode ==
              GUEST_RENDER_TIMING_NATIVE_59_94);
        CHECK(bridge.modes.effective_timing_mode ==
              GUEST_RENDER_TIMING_NATIVE_59_94);
        CHECK(bridge.modes.requested_render_mode == modes[index]);
        CHECK(guest_render_transaction_pending_snapshot(&pending) ==
              GUEST_RENDER_TRANSACTION_OK);
        CHECK(guest_render_native_stream_snapshot(&stream_snapshot) ==
              GUEST_RENDER_NATIVE_STREAM_OK);
        if (modes[index] == GUEST_RENDER_RENDER_NATIVE) {
            CHECK(bridge.modes.effective_render_mode ==
                  GUEST_RENDER_RENDER_NATIVE);
            CHECK(bridge.fallback_reason == GUEST_RENDER_FALLBACK_NONE);
            CHECK(bridge.binding_count == 1u);
            CHECK(pending.binding_count == 0u);
            CHECK(stream_snapshot.staged_count == 1u);
        } else {
            CHECK(bridge.modes.effective_render_mode == modes[index]);
            CHECK(bridge.binding_count == 0u);
            CHECK(pending.binding_count == 0u);
            CHECK(stream_snapshot.staged_count == 0u);
        }
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                     FIELD5_RETURN_SITE, 0u, 0u);
        CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
        CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) ==
              XG_RENDER_AUTH_OK);
        if (modes[index] == GUEST_RENDER_RENDER_NATIVE) {
            CHECK(!auth_snapshot.scene_aborted);
            CHECK(auth_snapshot.native_use_permitted);
            CHECK(auth_snapshot.native_item_count == 1u);
            CHECK(auth_snapshot.effective_render_mode ==
                  GUEST_RENDER_RENDER_NATIVE);
        } else {
            CHECK(auth_snapshot.effective_render_mode == modes[index]);
            CHECK(!auth_snapshot.native_use_permitted);
        }
    }
    return 1;
}

static int test_runtime_discards_pre_capture_provisional_candidate(void) {
    const XgRenderRuntimeVariantSourceSite *call_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[3];
    const XgRenderRuntimeVariantSourceSite *bucket_site =
        &xg_render_runtime_variant_descriptors[0].source_sites[8];
    CPUState cpu = {0};
    GuestRenderBridgeSnapshot bridge = {0};
    GuestRenderNativeStreamSnapshot stream_snapshot = {0};
    XgRenderAuthSnapshot auth_snapshot = {0};
    XgRenderAuth *auth = NULL;

    psx_xg_render_auth_runtime_test_reset();
    guest_render_transaction_test_reset();
    presentation_gate_succeeds = true;
    presentation_gate_saw_closed_state = false;
    CHECK(psx_xg_render_auth_configure(
        GUEST_RENDER_TIMING_ORIGINAL, GUEST_RENDER_RENDER_NATIVE,
        test_presentation_gate, NULL));
    set_matching_runtime_identity();
    psx_xg_render_auth_source_reset();
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_producer_family_enable(true);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 FIELD5_ACTIVATION_SITE,
                                 FIELD5_ACTIVATION_JAL,
                                 FIELD5_ACTIVATION_DELAY);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 FIELD5_PRODUCER_ENTRY, 0u, 0u);
    CHECK(produce_family_candidate(&cpu, call_site, bucket_site,
                                   0x800b06dcu, 250u));
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.state_open && bridge.producer_open);
    CHECK(bridge.binding_count == 1u);
    CHECK(guest_render_native_stream_snapshot(&stream_snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(stream_snapshot.staged_count == 1u);
    CHECK(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    CHECK(xg_render_auth_snapshot(auth, &auth_snapshot) == XG_RENDER_AUTH_OK);
    CHECK(!auth_snapshot.scene_aborted);
    CHECK(auth_snapshot.native_item_count == 0u);
    CHECK(!auth_snapshot.native_use_permitted);
    psx_xg_render_auth_scene_boundary();
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(!bridge.state_open && !bridge.producer_open);
    CHECK(bridge.binding_count == 0u);
    CHECK(guest_render_native_stream_snapshot(&stream_snapshot) ==
          GUEST_RENDER_NATIVE_STREAM_OK);
    CHECK(stream_snapshot.staged_count == 0u);
    return 1;
}

static int test_runtime_gate_failure_demotes_only_render(void) {
    GuestRenderBridgeSnapshot bridge = {0};

    psx_xg_render_auth_runtime_test_reset();
    guest_render_transaction_test_reset();
    presentation_gate_succeeds = false;
    presentation_gate_saw_closed_state = false;
    CHECK(psx_xg_render_auth_configure(
        GUEST_RENDER_TIMING_NATIVE_59_94, GUEST_RENDER_RENDER_NATIVE,
        test_presentation_gate, NULL));
    set_matching_runtime_identity();
    begin_field5_source_sequence(XG_RENDER_AUTH_TIER_COLD_INTERPRETER);
    CHECK(presentation_gate_saw_closed_state);
    CHECK(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    CHECK(bridge.state_open && bridge.producer_open);
    CHECK(bridge.modes.requested_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(bridge.modes.effective_timing_mode ==
          GUEST_RENDER_TIMING_NATIVE_59_94);
    CHECK(bridge.modes.requested_render_mode == GUEST_RENDER_RENDER_NATIVE);
    CHECK(bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
    CHECK(bridge.fallback_reason == GUEST_RENDER_FALLBACK_PRESENTATION_GATE);
    return 1;
}

static int test_runtime_gte_attribution_producer_lifecycle(void) {
    GteAttributionContextCounter contexts[2] = {0};
    GteAttributionSiteCounter sites[2] = {0};
    GteAttributionSnapshot snapshot = {0};
    const GteAttributionSite site = {
        .guest_pc = STATIC_CALLEE,
        .caller = RETURN_SITE,
        .guest_pc_known = true,
        .caller_known = true,
    };

    CHECK(reset_source_mode(GUEST_RENDER_RENDER_SHADOW));
    set_matching_runtime_identity();
    gte_attribution_reset();
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    CHECK(gte_attribution_snapshot(&snapshot, NULL, 0u, NULL, 0u) ==
          GTE_ATTRIBUTION_OK);
    CHECK(snapshot.current_context.inside_producer);
    CHECK(snapshot.current_context.producer_id ==
          XG_RENDER_AUTH_PRODUCER_RECORD_ID);
    CHECK(snapshot.current_context.tier == GTE_ATTRIBUTION_TIER_COLD);
    CHECK(snapshot.current_context.visual_state_id.scene_epoch != 0u);

    CHECK(gte_attribution_record_execute_in_tier(
              &site, GTE_ATTRIBUTION_TIER_STATIC) == GTE_ATTRIBUTION_OK);
    CHECK(gte_attribution_snapshot(&snapshot, contexts, 2u, sites, 2u) ==
          GTE_ATTRIBUTION_OK);
    CHECK(snapshot.total_count == 1u);
    CHECK(snapshot.inside_producer_count == 1u);
    CHECK(snapshot.outside_producer_count == 0u);
    CHECK(snapshot.tier_counts[GTE_ATTRIBUTION_TIER_STATIC] == 1u);
    CHECK(contexts[0].context.inside_producer);
    CHECK(contexts[0].context.producer_id ==
          XG_RENDER_AUTH_PRODUCER_RECORD_ID);

    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 CALLER_SITE, JAL_INSTRUCTION, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 RETURN_SITE, 0u, 0u);
    CHECK(gte_attribution_snapshot(&snapshot, contexts, 2u, sites, 2u) ==
          GTE_ATTRIBUTION_OK);
    CHECK(!snapshot.current_context.inside_producer);

    psx_xg_render_auth_scene_boundary();
    note_matching_candidate();
    psx_xg_render_auth_warm_hook(NULL, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 PRODUCER_ENTRY, 0u, 0u);
    CHECK(gte_attribution_snapshot(&snapshot, contexts, 2u, sites, 2u) ==
          GTE_ATTRIBUTION_OK);
    CHECK(snapshot.current_context.inside_producer);
    CHECK(snapshot.current_context.tier == GTE_ATTRIBUTION_TIER_WARM);
    psx_xg_render_auth_scene_boundary();
    CHECK(gte_attribution_snapshot(&snapshot, contexts, 2u, sites, 2u) ==
          GTE_ATTRIBUTION_OK);
    CHECK(!snapshot.current_context.inside_producer);
    return 1;
}

int main(void) {
    int ok = 1;

    guest_render_bridge_test_reset();
    if (!psx_xg_render_auth_configure(
            GUEST_RENDER_TIMING_NATIVE_59_94, GUEST_RENDER_RENDER_NATIVE,
            test_presentation_gate, NULL))
        return 1;
    ok &= test_runtime_completed_proof_receipt_lifecycle();
    ok &= test_runtime_idle_activation_hook_is_relevant();
    ok &= test_cold_ui_draw_ot_observation_is_relevant();
    ok &= test_runtime_variant_supersedes_canonical_entry_alias();
    ok &= test_runtime_initial_field5_chain_is_armed();
    ok &= test_runtime_variant_accepts_entry_at_return_terminal();
    ok &= test_runtime_variant_rearms_at_new_exact_activation();
    ok &= test_runtime_variant_consumes_callee_hook_before_return();
    ok &= test_runtime_variant_rejects_wrong_return_address();
    ok &= test_runtime_variant_rejects_cold_wrong_return_address();
    ok &= test_identity_mismatch_forces_original();
    ok &= test_site_and_delay_mismatch_block_proof();
    ok &= test_synthetic_trace_selects_next_scene_only();
    ok &= test_static_auth_rejects_wrong_return_address_before_observation();
    ok &= test_trace_schema_is_metadata_only();
    ok &= test_runtime_rejection_receipt_latches_first_trigger();
    ok &= test_runtime_direct_hook_rejections_latch_trigger();
    ok &= test_runtime_cold_warm_auth_parity_and_fail_closed_reset();
    ok &= test_runtime_canonicalizes_field5_physical_chain();
    ok &= test_runtime_canonicalizes_field5_physical_alias_chain();
    ok &= test_runtime_rejects_incomplete_or_mutated_field5_chain();
    ok &= test_runtime_rejects_followup_field5_capture_before_return();
    ok &= test_runtime_warm_field5_candidate_validation();
    ok &= test_runtime_rejects_unbound_field5_candidates();
    ok &= test_runtime_ignores_unrelated_watched_writes_before_field5_entry_and_return();
    ok &= test_runtime_preserves_pending_field5_candidate_for_protected_pre_activation_write();
    ok &= test_runtime_rejects_writes_overlapping_protected_auth_ranges();
    ok &= test_runtime_cold_auth_accepts_physical_manifest_aliases();
    ok &= test_runtime_filters_broad_hooks_and_rearms_at_exact_entry();
    ok &= test_runtime_variant_context_mutations_do_not_observe();
    ok &= test_source_observation_hooks_preserve_field5_variant_lifecycle();
    ok &= test_source_observation_cold_warm_aggregate_parity();
    ok &= test_source_observation_diagnostic_overflow_aggregates();
    ok &= test_source_observation_fail_closed_cases();
    ok &= test_resident_ft4_geometry_cold_warm_parity();
    ok &= test_resident_ft4_geometry_accepts_scratchpad_stack();
    ok &= test_native_ft4_bypass_accepts_scratchpad_outputs();
    ok &= test_native_stream_authority_starts_at_authenticated_scene();
    ok &= test_resident_residual_recaptures_after_producer_write();
    ok &= test_native_particle_sidecar_and_cutover();
    ok &= test_native_particle_requires_authenticated_sidecar();
    ok &= test_native_particle_respects_effective_original();
    ok &= test_native_particle_cold_observer_rejects_tag_mismatch();
    ok &= test_native_particle_code_write_invalidates_sidecar();
    ok &= test_native_zoom_ignores_foreign_overlay_alias();
    ok &= test_zoom_opcode_2e_template_contract_is_producer_scoped();
    ok &= test_overlay_same_instruction_requires_exact_artifact_candidate();
    ok &= test_overlay_artifact_authority_is_not_field5_specific();
    ok &= test_sidecar_rejects_address_reuse_across_scene_and_stale_packet();
    ok &= test_native_zoom_unity_sidecar_and_ordering();
    ok &= test_native_zoom_projects_without_packet_sources();
    ok &= test_native_zoom_requires_authenticated_initializer();
    ok &= test_native_projected_effect_is_resident_and_packet_free();
    ok &= test_native_view_preserves_guest_projected_background_path();
    ok &= test_native_projected_effect_fails_closed_without_source_or_tag();
    ok &= test_native_projected_effect_resolves_field_ot_bucket();
    ok &= test_projected_source_map_evicts_oldest_without_blocking();
    ok &= test_native_world_sky_cutover_is_packet_free();
    ok &= test_native_world_sky_fails_closed_and_aborts_on_mutation();
    ok &= test_native_world_effects_cutover_is_atomic_and_value_only();
    ok &= test_native_world_effects_fails_before_staging_or_writes();
    ok &= test_native_world_effects_stages_full_capacity();
    ok &= test_native_world_horizon_cutover_is_atomic();
    ok &= test_resident_line_f2_stages_producer_semantics();
    ok &= test_resident_line_f2_rejects_stale_or_mutated_source();
    ok &= test_native_world_horizon_fails_before_staging_or_writes();
    ok &= test_native_world_minimap_local_cutover_is_atomic();
    ok &= test_native_world_minimap_validates_saved_ra_and_gprs();
    ok &= test_native_world_minimap_aborts_partial_staging();
    ok &= test_remaining_world_native_empty_cutovers_are_authenticated();
    ok &= test_remaining_world_native_cutovers_reject_wrong_callers();
    ok &= test_remaining_world_native_nonempty_cutovers_are_atomic();
    ok &= test_world_models_all_family_sidecar_cutover();
    ok &= test_world_model_reinitialization_preserves_other_packet_buffer();
    ok &= test_world_model_templates_only_track_model_code();
    ok &= test_world_actor_native_seam_is_context_bound_and_atomic();
    ok &= test_world_family_execution_census_is_exact_instruction_bound();
    ok &= test_world_horizon_shadow_matches_atomic_transaction();
    ok &= test_world_horizon_shadow_reports_payload_mismatch();
    ok &= test_world_horizon_shadow_fails_closed();
    ok &= test_world_effects_shadow_matches_value_only_source();
    ok &= test_world_effects_shadow_rejects_invalid_cursor();
    ok &= test_model_ft4_raw_shadow_reconstructs_source();
    ok &= test_model_ft3_native_cull_uses_wide_margin();
    ok &= test_battle_sprite_ft4_shadow_uses_descriptor_source();
    ok &= test_battle_sprite_ft4_shadow_reports_descriptor_blocker();
    ok &= test_battle_sprite_ft4_shadow_skips_empty_callers();
    ok &= test_sprite_ft4_native_contract_is_wrapper_scoped();
    ok &= test_resident_render_dependencies_register_memory_watches();
    ok &= test_resident_ft4_geometry_rejects_wrong_caller_and_stride();
    ok &= test_producer_family_native_stages_authenticated_source();
    ok &= test_producer_family_shadow_compares_without_staging();
    ok &= test_runtime_render_modes_are_independent_and_gate_staging();
    ok &= test_runtime_discards_pre_capture_provisional_candidate();
    ok &= test_runtime_gate_failure_demotes_only_render();
    ok &= test_runtime_gte_attribution_producer_lifecycle();
    return ok ? 0 : 1;
}
