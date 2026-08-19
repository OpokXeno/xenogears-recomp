#include "xg_render_runtime_variant_auth.h"

#include "xg_render_manifest_generated.h"
#include "xg_render_runtime_variants_generated.h"

#include <stdlib.h>
#include <string.h>

#define XG_RENDER_OVERLAY_AUTH_IMPLEMENTATION 1
#include "xg_render_overlay_cutovers.inc"
#undef XG_RENDER_OVERLAY_AUTH_IMPLEMENTATION

_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_READ ==
                   PSX_XG_RENDER_SOURCE_OPERATION_READ,
               "source read operation values must match");
_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_WRITE ==
                   PSX_XG_RENDER_SOURCE_OPERATION_WRITE,
               "source write operation values must match");
_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_SWC2 ==
                   PSX_XG_RENDER_SOURCE_OPERATION_SWC2,
               "source SWC2 operation values must match");
_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_CALL ==
                   PSX_XG_RENDER_SOURCE_OPERATION_CALL,
               "source call operation values must match");
_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_BUCKET ==
                   PSX_XG_RENDER_SOURCE_OPERATION_BUCKET,
               "source bucket operation values must match");
_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_EFFECTIVE_ADDRESS ==
                   PSX_XG_RENDER_SOURCE_AUXILIARY_EFFECTIVE_ADDRESS,
               "source effective-address values must match");
_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_NONE ==
                   PSX_XG_RENDER_SOURCE_AUXILIARY_NONE,
               "source none values must match");
_Static_assert(XG_RENDER_RUNTIME_VARIANT_SOURCE_RESULT_REGISTER ==
                   PSX_XG_RENDER_SOURCE_AUXILIARY_RESULT_REGISTER,
               "source result-register values must match");

typedef enum XgRenderRuntimeVariantPhase {
    XG_RENDER_RUNTIME_VARIANT_IDLE = 0,
    XG_RENDER_RUNTIME_VARIANT_EXPECT_ENTRY,
    XG_RENDER_RUNTIME_VARIANT_EXPECT_CAPTURE,
    XG_RENDER_RUNTIME_VARIANT_EXPECT_RETURN,
} XgRenderRuntimeVariantPhase;

typedef struct XgRenderRuntimeVariantState {
    const XgRenderRuntimeVariantDescriptor *descriptor;
    uint64_t scene_generation;
    XgRenderRuntimeVariantPhase phase;
} XgRenderRuntimeVariantState;

static XgRenderRuntimeVariantState state;

bool xg_render_runtime_variant_no_gates_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *value = getenv("PSX_OVERLAY_NO_GATES");
        /* Keep the strict rejection path as the default; opt into the
         * no-gates diagnostic variant explicitly with a nonzero value. */
        cached = value != NULL && value[0] != '0';
    }
    return cached != 0;
}

static bool digest_present(const uint8_t digest[XG_RENDER_MANIFEST_DIGEST_SIZE]) {
    uint32_t index;

    for (index = 0u; index < XG_RENDER_MANIFEST_DIGEST_SIZE; ++index) {
        if (digest[index] != 0u) return true;
    }
    return false;
}

static bool range_contains(uint32_t start, uint32_t size, uint32_t value,
                           uint32_t value_size) {
    const uint64_t end = (uint64_t)start + size;
    const uint64_t value_end = (uint64_t)value + value_size;

    return size != 0u && value_size != 0u && value >= start && value_end <= end;
}

static bool normalized_range_contains(uint32_t start, uint32_t size,
                                      uint32_t value, uint32_t value_size) {
    return range_contains(start & 0x1fffffffu, size, value & 0x1fffffffu,
                          value_size);
}

static bool normalized_ranges_overlap(uint32_t left_start, uint32_t left_size,
                                      uint32_t right_start, uint32_t right_size) {
    const uint64_t left_begin = left_start & 0x1fffffffu;
    const uint64_t left_end = left_begin + left_size;
    const uint64_t right_begin = right_start & 0x1fffffffu;
    const uint64_t right_end = right_begin + right_size;

    return left_size != 0u && right_size != 0u &&
           left_begin < right_end && right_begin < left_end;
}

static bool physical_address_equals(uint32_t left, uint32_t right) {
    return (left & 0x1fffffffu) == (right & 0x1fffffffu);
}

static bool jal_matches(uint32_t site, uint32_t instruction_word,
                        uint32_t target, uint32_t delay_slot_word,
                        uint32_t required_delay) {
    const uint32_t jal_target =
        (site & 0xf0000000u) | ((instruction_word & 0x03ffffffu) << 2u);

    return instruction_word >> 26u == 3u &&
           physical_address_equals(jal_target, target) &&
           delay_slot_word == required_delay;
}

static bool source_site_is_valid(
    const XgRenderRuntimeVariantSourceSite *site) {
    switch (site->operation) {
    case XG_RENDER_RUNTIME_VARIANT_SOURCE_READ:
    case XG_RENDER_RUNTIME_VARIANT_SOURCE_WRITE:
        return site->auxiliary_rule ==
                   XG_RENDER_RUNTIME_VARIANT_SOURCE_EFFECTIVE_ADDRESS &&
               (site->width == 1u || site->width == 2u || site->width == 4u);
    case XG_RENDER_RUNTIME_VARIANT_SOURCE_SWC2:
        return site->auxiliary_rule ==
                   XG_RENDER_RUNTIME_VARIANT_SOURCE_EFFECTIVE_ADDRESS &&
               site->width == 4u;
    case XG_RENDER_RUNTIME_VARIANT_SOURCE_CALL:
        return site->auxiliary_rule == XG_RENDER_RUNTIME_VARIANT_SOURCE_NONE &&
               site->width == 0u;
    case XG_RENDER_RUNTIME_VARIANT_SOURCE_BUCKET:
        return site->auxiliary_rule ==
                   XG_RENDER_RUNTIME_VARIANT_SOURCE_RESULT_REGISTER &&
               site->width == 0u;
    }
    return false;
}

static bool descriptor_source_sites_are_valid(
    const XgRenderRuntimeVariantDescriptor *descriptor) {
    uint32_t call_count = 0u;
    uint32_t bucket_count = 0u;

    for (uint32_t index = 0u; index < descriptor->source_site_count; ++index) {
        const XgRenderRuntimeVariantSourceSite *site =
            &descriptor->source_sites[index];

        if (!source_site_is_valid(site) || (site->pc & 3u) != 0u ||
            !range_contains(descriptor->artifact_range_start,
                            descriptor->artifact_range_size, site->pc, 4u))
            return false;
        if (index != 0u &&
            (descriptor->source_sites[index - 1u].pc & 0x1fffffffu) >=
                (site->pc & 0x1fffffffu))
            return false;
        call_count += site->operation == XG_RENDER_RUNTIME_VARIANT_SOURCE_CALL;
        bucket_count +=
            site->operation == XG_RENDER_RUNTIME_VARIANT_SOURCE_BUCKET;
        for (uint32_t prior = 0u; prior < index; ++prior) {
            const XgRenderRuntimeVariantSourceSite *other =
                &descriptor->source_sites[prior];
            if (physical_address_equals(site->pc, other->pc) &&
                site->instruction == other->instruction)
                return false;
        }
    }
    return call_count == 1u && bucket_count == 1u;
}

static bool descriptor_cutovers_are_valid(
        const XgRenderRuntimeVariantDescriptor *descriptor) {
    if (descriptor->cutover_count == 0u ||
        descriptor->cutover_count > XG_RENDER_RUNTIME_VARIANT_CUTOVER_CAP)
        return false;
    for (uint32_t index = 0u; index < descriptor->cutover_count; ++index) {
        const XgRenderRuntimeVariantCutover *cutover =
            &descriptor->cutovers[index];
        uint8_t expected_transfer;

        switch ((XgRenderRuntimeVariantCutoverHandler)cutover->handler) {
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ACTOR:
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_ZOOM_NATIVE:
            expected_transfer = 1u;
            break;
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_COMPASS_WORLD:
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_COMPASS_SCREEN:
        case XG_RENDER_RUNTIME_VARIANT_CUTOVER_PARTICLE_NATIVE:
            expected_transfer = 2u;
            break;
        default:
            expected_transfer = 0u;
            break;
        }
        if ((cutover->pc & 3u) != 0u || cutover->transfer > 2u ||
            cutover->handler >=
                XG_RENDER_RUNTIME_VARIANT_CUTOVER_HANDLER_COUNT ||
            cutover->transfer != expected_transfer ||
            !digest_present(cutover->code_range_identity) ||
            !range_contains(descriptor->artifact_range_start,
                            descriptor->artifact_range_size,
                            cutover->code_range_start,
                            cutover->code_range_size) ||
            !range_contains(cutover->code_range_start,
                            cutover->code_range_size, cutover->pc, 4u) ||
            ((cutover->transfer == 1u) != (cutover->continuation != 0u)) ||
            (cutover->transfer == 1u &&
             !range_contains(descriptor->artifact_range_start,
                             descriptor->artifact_range_size,
                             cutover->continuation, 4u)))
            return false;
    }
    return true;
}

static bool descriptor_is_valid_uncached(
        const XgRenderRuntimeVariantDescriptor *descriptor) {
    const XgRenderManifestValidation *validation = &xg_render_manifest_validation;
    uint32_t range_start;

    if (descriptor == NULL ||
        descriptor->producer_record_id != validation->producer_record_id ||
        descriptor->site_record_id != validation->site_record_id ||
        memcmp(descriptor->canonical_game_identity, xg_render_game_identity,
               sizeof(descriptor->canonical_game_identity)) != 0 ||
        memcmp(descriptor->canonical_manifest_identity,
               xg_render_manifest_identity,
               sizeof(descriptor->canonical_manifest_identity)) != 0 ||
        !digest_present(descriptor->companion_manifest_identity) ||
        !digest_present(descriptor->artifact_identity) ||
        !digest_present(descriptor->artifact_range_identity) ||
        !digest_present(descriptor->activation_window_identity) ||
        !digest_present(descriptor->capture_window_identity) ||
        descriptor->canonical_producer_entry != validation->producer_entry ||
        descriptor->canonical_capture_site != validation->caller_site ||
        descriptor->canonical_static_callee != validation->static_callee ||
        descriptor->canonical_return_site != validation->return_site ||
        descriptor->artifact_size == 0u || descriptor->artifact_range_size == 0u ||
        !range_contains(descriptor->artifact_base, descriptor->artifact_size,
                        descriptor->artifact_range_start,
                        descriptor->artifact_range_size) ||
        descriptor->activation_window_size < 8u ||
        descriptor->capture_window_size < 8u ||
        descriptor->activation_required_jal_opcode != 3u ||
        descriptor->activation_jal_target != descriptor->physical_producer_entry ||
            descriptor->capture_required_jal_opcode != 3u ||
            descriptor->capture_jal_target != validation->static_callee ||
            descriptor->physical_return_site != descriptor->capture_site + 8u ||
            descriptor->model_dispatch_instruction_count == 0u ||
            descriptor->model_dispatch_instruction_count >
                XG_RENDER_RUNTIME_VARIANT_MODEL_DISPATCH_CAP ||
            (descriptor->model_dispatch_matrix_stack_offset & 3u) != 0u ||
            descriptor->source_site_count == 0u ||
            descriptor->source_site_count >
                XG_RENDER_RUNTIME_VARIANT_SOURCE_SITE_CAP)
        return false;
    range_start = descriptor->artifact_range_start;
    return range_contains(range_start, descriptor->artifact_range_size,
                          descriptor->activation_window_start,
                          descriptor->activation_window_size) &&
           range_contains(range_start, descriptor->artifact_range_size,
                          descriptor->physical_producer_entry, 4u) &&
           range_contains(range_start, descriptor->artifact_range_size,
                          descriptor->capture_window_start,
                          descriptor->capture_window_size) &&
           range_contains(range_start, descriptor->artifact_range_size,
                          descriptor->physical_return_site, 4u) &&
           range_contains(descriptor->activation_window_start,
                          descriptor->activation_window_size,
                          descriptor->activation_site, 8u) &&
           range_contains(descriptor->capture_window_start,
                           descriptor->capture_window_size,
                           descriptor->capture_site, 8u) &&
            range_contains(range_start, descriptor->artifact_range_size,
                           descriptor->model_dispatch_window_start,
                           descriptor->model_dispatch_instruction_count * 4u) &&
            descriptor_source_sites_are_valid(descriptor) &&
            descriptor_cutovers_are_valid(descriptor);
}

static bool descriptor_is_valid(const XgRenderRuntimeVariantDescriptor *descriptor) {
    static const XgRenderRuntimeVariantDescriptor *cached_descriptor;
    static bool cached_result;

    if (descriptor == NULL) return false;
    if (cached_descriptor == descriptor) return cached_result;
    cached_result = descriptor_is_valid_uncached(descriptor);
    cached_descriptor = descriptor;
    return cached_result;
}

bool xg_render_runtime_variant_source_site_lookup(
    uint32_t pc, uint32_t instruction_word,
    PsxXgRenderSourceSiteMetadata *out_metadata) {
    const XgRenderRuntimeVariantDescriptor *descriptor = state.descriptor;
    uint32_t first = 0u;
    uint32_t last;

    if (descriptor == NULL || out_metadata == NULL) return false;
    pc &= 0x1fffffffu;
    last = descriptor->source_site_count;
    while (first < last) {
        const uint32_t index = first + (last - first) / 2u;
        const XgRenderRuntimeVariantSourceSite *site =
            &descriptor->source_sites[index];
        const uint32_t site_pc = site->pc & 0x1fffffffu;

        if (pc < site_pc) {
            last = index;
        } else if (pc > site_pc) {
            first = index + 1u;
        } else {
            if (instruction_word != site->instruction ||
                !source_site_is_valid(site))
                return false;
            *out_metadata = (PsxXgRenderSourceSiteMetadata){
                (PsxXgRenderSourceOperation)site->operation,
                (PsxXgRenderSourceAuxiliaryRule)site->auxiliary_rule,
                site->width,
            };
            return true;
        }
    }
    return false;
}

bool xg_render_runtime_variant_source_pc_relevant(uint32_t pc) {
    const XgRenderRuntimeVariantDescriptor *descriptor = state.descriptor;
    const XgRenderRuntimeVariantSourceSite *first;
    const XgRenderRuntimeVariantSourceSite *last;

    if (descriptor == NULL || descriptor->source_site_count == 0u)
        return false;
    first = &descriptor->source_sites[0];
    last = &descriptor->source_sites[descriptor->source_site_count - 1u];
    pc &= 0x1fffffffu;
    return pc >= (first->pc & 0x1fffffffu) &&
           pc <= (last->pc & 0x1fffffffu);
}

bool xg_render_runtime_variant_native_cutover_lookup(
    uint32_t pc, uint32_t instruction_word,
    XgRenderRuntimeVariantCutoverHandler *out_handler,
    uint32_t *out_continuation) {
    if (out_handler == NULL || out_continuation == NULL) return false;
    for (uint32_t descriptor_index = 0u;
         descriptor_index < xg_render_runtime_variant_descriptor_count;
         ++descriptor_index) {
        const XgRenderRuntimeVariantDescriptor *descriptor =
            state.descriptor != NULL ? state.descriptor :
                &xg_render_runtime_variant_descriptors[descriptor_index];
        if (!descriptor_is_valid(descriptor)) {
            if (state.descriptor != NULL) return false;
            continue;
        }
        for (uint32_t index = 0u; index < descriptor->cutover_count; ++index) {
            const XgRenderRuntimeVariantCutover *cutover =
                &descriptor->cutovers[index];
            if (physical_address_equals(pc, cutover->pc) &&
                instruction_word == cutover->instruction) {
                if (cutover->handler ==
                        XG_RENDER_RUNTIME_VARIANT_CUTOVER_ACTOR &&
                    (state.descriptor == NULL ||
                     (state.phase != XG_RENDER_RUNTIME_VARIANT_EXPECT_CAPTURE &&
                      state.phase != XG_RENDER_RUNTIME_VARIANT_EXPECT_RETURN)))
                    return false;
                *out_handler =
                    (XgRenderRuntimeVariantCutoverHandler)cutover->handler;
                *out_continuation = cutover->continuation;
                return true;
            }
        }
        if (state.descriptor != NULL) break;
    }
    return false;
}

bool xg_render_runtime_variant_hook_relevant(uint32_t hook, uint32_t pc) {
    const XgRenderRuntimeVariantDescriptor *descriptor = state.descriptor;

    if (descriptor == NULL) {
        if (hook != PSX_XG_RENDER_AUTH_HOOK_CAPTURE) return false;
        for (uint32_t index = 0u;
             index < xg_render_runtime_variant_descriptor_count; ++index) {
            const XgRenderRuntimeVariantDescriptor *candidate =
                &xg_render_runtime_variant_descriptors[index];
            if (descriptor_is_valid(candidate) && physical_address_equals(
                    pc, candidate->activation_site))
                return true;
        }
        return false;
    }
    switch (state.phase) {
    case XG_RENDER_RUNTIME_VARIANT_EXPECT_ENTRY:
        return true;
    case XG_RENDER_RUNTIME_VARIANT_EXPECT_CAPTURE:
        return physical_address_equals(pc, descriptor->capture_site) ||
               physical_address_equals(pc,
                                       descriptor->physical_producer_entry) ||
               physical_address_equals(pc,
                                       descriptor->physical_return_site);
    case XG_RENDER_RUNTIME_VARIANT_EXPECT_RETURN:
        return normalized_range_contains(descriptor->artifact_range_start,
                                         descriptor->artifact_range_size,
                                         pc, 1u);
    case XG_RENDER_RUNTIME_VARIANT_IDLE:
        return false;
    }
    return false;
}

static bool candidate_artifact_matches(
    const XgRenderRuntimeVariantDescriptor *descriptor,
    const PsxXgRenderAuthCandidate *candidate) {
    return descriptor != NULL && candidate != NULL &&
           physical_address_equals(candidate->artifact_base,
                                   descriptor->artifact_base) &&
           candidate->artifact_size == descriptor->artifact_size &&
           candidate->runtime_variant_bound &&
           memcmp(candidate->runtime_variant_identity,
                  descriptor->companion_manifest_identity,
                  sizeof(candidate->runtime_variant_identity)) == 0;
}

static bool candidate_matches(const XgRenderRuntimeVariantDescriptor *descriptor,
                               const PsxXgRenderAuthCandidate *candidate) {
    if (xg_render_runtime_variant_no_gates_enabled())
        return candidate != NULL;
    return descriptor != NULL && candidate != NULL &&
           candidate->authority_provenance && candidate->pair_bound &&
           candidate->pair_id != 0u &&
           memcmp(candidate->identity.game_sha256,
                  descriptor->canonical_game_identity,
                  sizeof(candidate->identity.game_sha256)) == 0 &&
           memcmp(candidate->identity.manifest_sha256,
                  descriptor->canonical_manifest_identity,
                  sizeof(candidate->identity.manifest_sha256)) == 0 &&
            candidate_artifact_matches(descriptor, candidate) &&
           physical_address_equals(candidate->producer_entry,
                                   descriptor->physical_producer_entry) &&
           physical_address_equals(candidate->dispatch_pc,
                                   descriptor->physical_producer_entry) &&
           normalized_range_contains(descriptor->artifact_range_start,
                                     descriptor->artifact_range_size,
                                     candidate->range_start,
                                     candidate->range_size) &&
            normalized_range_contains(candidate->range_start,
                                      candidate->range_size,
                                      descriptor->physical_producer_entry, 4u);
}

static bool artifact_candidate_matches(
    const XgRenderRuntimeVariantDescriptor *descriptor,
    const PsxXgRenderAuthCandidate *candidate) {
    if (xg_render_runtime_variant_no_gates_enabled())
        return candidate != NULL;
    return descriptor != NULL && candidate != NULL &&
           candidate->authority_provenance && candidate->pair_bound &&
           candidate->pair_id != 0u &&
           memcmp(candidate->identity.game_sha256,
                  descriptor->canonical_game_identity,
                  sizeof(candidate->identity.game_sha256)) == 0 &&
           memcmp(candidate->identity.manifest_sha256,
                  descriptor->canonical_manifest_identity,
                  sizeof(candidate->identity.manifest_sha256)) == 0 &&
           candidate_artifact_matches(descriptor, candidate) &&
           normalized_range_contains(descriptor->artifact_range_start,
                                     descriptor->artifact_range_size,
                                     candidate->range_start,
                                     candidate->range_size) &&
           normalized_range_contains(candidate->range_start,
                                     candidate->range_size,
                                     candidate->dispatch_pc, 4u);
}

void xg_render_runtime_variant_reset(void) {
    state = (XgRenderRuntimeVariantState){ 0 };
}

bool xg_render_runtime_variant_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate) {
    if (state.descriptor != NULL)
        return candidate_matches(state.descriptor, candidate);
    for (uint32_t index = 0u;
         index < xg_render_runtime_variant_descriptor_count; ++index) {
        const XgRenderRuntimeVariantDescriptor *descriptor =
            &xg_render_runtime_variant_descriptors[index];
        if (descriptor_is_valid(descriptor) &&
            candidate_matches(descriptor, candidate))
            return true;
    }
    return false;
}

bool xg_render_runtime_variant_artifact_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate) {
    if (state.descriptor != NULL)
        return artifact_candidate_matches(state.descriptor, candidate);
    for (uint32_t index = 0u;
         index < xg_render_runtime_variant_descriptor_count; ++index) {
        const XgRenderRuntimeVariantDescriptor *descriptor =
            &xg_render_runtime_variant_descriptors[index];
        if (descriptor_is_valid(descriptor) &&
            artifact_candidate_matches(descriptor, candidate))
            return true;
    }
    return false;
}

uint32_t xg_render_runtime_variant_model_dispatch_contract_count(void) {
    return xg_render_runtime_variant_descriptor_count;
}

bool xg_render_runtime_variant_model_dispatch_contract_at(
    uint32_t index, XgRenderRuntimeVariantModelDispatchContract *out_contract) {
    const XgRenderRuntimeVariantDescriptor *descriptor;

    if (out_contract == NULL ||
        index >= xg_render_runtime_variant_descriptor_count)
        return false;
    descriptor = &xg_render_runtime_variant_descriptors[index];
    if (!descriptor_is_valid(descriptor)) return false;
    *out_contract = (XgRenderRuntimeVariantModelDispatchContract){
        descriptor->model_dispatch_instructions,
        descriptor->model_dispatch_instruction_count,
        descriptor->model_dispatch_matrix_stack_offset,
    };
    return true;
}

bool xg_render_runtime_variant_artifact_candidate_authorizes_pc(
    const PsxXgRenderAuthCandidate *candidate, uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled())
        return candidate != NULL &&
               normalized_range_contains(candidate->range_start,
                                          candidate->range_size, pc, 4u);
    if (state.descriptor != NULL)
        return artifact_candidate_matches(state.descriptor, candidate) &&
            normalized_range_contains(candidate->range_start,
                                      candidate->range_size, pc, 4u);
    for (uint32_t index = 0u;
         index < xg_render_runtime_variant_descriptor_count; ++index) {
        const XgRenderRuntimeVariantDescriptor *descriptor =
            &xg_render_runtime_variant_descriptors[index];
        if (descriptor_is_valid(descriptor) &&
            artifact_candidate_matches(descriptor, candidate) &&
            normalized_range_contains(candidate->range_start,
                                      candidate->range_size, pc, 4u))
            return true;
    }
    return false;
}

bool xg_render_authoritative_overlay_artifact_candidate_matches(
    const PsxXgRenderAuthCandidate *candidate) {
    if (xg_render_runtime_variant_no_gates_enabled())
        return candidate != NULL;
    return xg_render_overlay_artifact_candidate_matches(candidate);
}

bool xg_render_authoritative_overlay_artifact_candidate_authorizes_pc(
    const PsxXgRenderAuthCandidate *candidate, uint32_t pc) {
    if (xg_render_runtime_variant_no_gates_enabled())
        return candidate != NULL &&
               normalized_range_contains(candidate->range_start,
                                         candidate->range_size, pc, 4u);
    return xg_render_overlay_artifact_candidate_authorizes_pc(candidate, pc);
}

typedef struct XgRenderProtectedCodeRange {
    uint32_t start;
    uint32_t size;
} XgRenderProtectedCodeRange;

static const XgRenderProtectedCodeRange zoom_code_ranges[] = {
        { UINT32_C(0x8003f738), 0x178u },
        { UINT32_C(0x80043a1c), 0x3cu },
        { UINT32_C(0x80043bfc), 0x28u },
        { UINT32_C(0x80043cb0), 0x14u },
        { UINT32_C(0x800454dc), 0x58u },
        { UINT32_C(0x800459dc), 0x50u },
        { UINT32_C(0x80045c10), 0x78u },
        { UINT32_C(0x80049dcc), 0x124u },
        { UINT32_C(0x80049efc), 0x30u },
        { UINT32_C(0x80049f8c), 0x20u },
        { UINT32_C(0x8004a7bc), 0x7cu },
        { UINT32_C(0x80078ef8), 8u }, { UINT32_C(0x80078fb8), 8u },
        { UINT32_C(0x80079168), 8u }, { UINT32_C(0x800791fc), 8u },
        { UINT32_C(0x800a58a4), 8u }, { UINT32_C(0x800a58f0), 8u },
        { UINT32_C(0x800a5ac0), 8u }, { UINT32_C(0x800a5b8c), 8u },
        { UINT32_C(0x800a5bec), 8u }, { UINT32_C(0x800a5e60), 8u },
        { UINT32_C(0x800a5f6c), 8u }, { UINT32_C(0x800a6088), 8u },
        { UINT32_C(0x800a6150), 8u }, { UINT32_C(0x800a61ec), 8u },
        { UINT32_C(0x800a6278), 8u }, { UINT32_C(0x800a62c0), 8u },
        { UINT32_C(0x800a637c), 8u },
        { UINT32_C(0x800a5de8), 8u }, { UINT32_C(0x800a5e90), 8u },
        { UINT32_C(0x800abc44), 8u }, { UINT32_C(0x800abc68), 8u },
        { UINT32_C(0x800abca4), 8u },
};

static const XgRenderProtectedCodeRange projected_effect_code_ranges[] = {
    { UINT32_C(0x8002709c), 0x328u },
    { UINT32_C(0x800273c4), 0x534u },
    { UINT32_C(0x800278f8), 0x448u },
    { UINT32_C(0x80043a1c), 0x54u },
    { UINT32_C(0x80043c24), 0x20u },
    { UINT32_C(0x80043c9c), 0x3cu },
    { UINT32_C(0x80048c4c), 0x84u },
    { UINT32_C(0x80048d68), 0x12cu },
    { UINT32_C(0x8004b32c), 0x180u },
    { UINT32_C(0x80056a00), 0x180u },
    { UINT32_C(0x80056b94), 0x180u },
    { UINT32_C(0x80057030), 0x802u },
};

static const XgRenderProtectedCodeRange world_sky_code_ranges[] = {
    { UINT32_C(0x80071b50), 0x18u },
    { UINT32_C(0x800736dc), 0x110u },
    { UINT32_C(0x800737ec), 0x1ccu },
    { UINT32_C(0x8009a280), 0x80u },
    { UINT32_C(0x80048ab0), 0x10cu },
    { UINT32_C(0x8004931c), 0x160u },
    { UINT32_C(0x80049efc), 0x30u },
    { UINT32_C(0x80049f8c), 0x20u },
    { UINT32_C(0x8004a12c), 0x18u },
    { UINT32_C(0x8004a14c), 0x0cu },
    { UINT32_C(0x8004a73c), 0x78u },
    { UINT32_C(0x8004a92c), 0x68u },
};

static const XgRenderProtectedCodeRange world_horizon_code_ranges[] = {
    { UINT32_C(0x80071b50), 0x18u },
    { UINT32_C(0x800739b8), 0x14cu },
    { UINT32_C(0x80073b04), 0x32cu },
    { UINT32_C(0x8009a300), 0x40u },
    { UINT32_C(0x80043a1c), 0x54u },
    { UINT32_C(0x80043bfc), 0x28u },
    { UINT32_C(0x800453ac), 0x3cu },
    { UINT32_C(0x8004931c), 0x160u },
    { UINT32_C(0x80049efc), 0x30u },
    { UINT32_C(0x80049f8c), 0x20u },
    { UINT32_C(0x8004a73c), 0x78u },
    { UINT32_C(0x8004a92c), 0x68u },
};

static const XgRenderProtectedCodeRange world_effects_code_ranges[] = {
    { UINT32_C(0x80071a90), 0x20u },
    { UINT32_C(0x8008901c), 0x10cu },
    { UINT32_C(0x80089748), 0x530u },
    { UINT32_C(0x80089c78), 0x650u },
    { UINT32_C(0x80093534), 0xa8u },
    { UINT32_C(0x80049dcc), 0x124u },
    { UINT32_C(0x8004b18c), 0x198u },
    { UINT32_C(0x8009aff0), 0x50u },
    { UINT32_C(0x8009b040), 0x140u },
};

static const XgRenderProtectedCodeRange model_ft4_raw_code_ranges[] = {
    { UINT32_C(0x800257b0), 0x2cu },
    { UINT32_C(0x8002c700), 0x4ea0u },
    { UINT32_C(0x80043a1c), 0x54u },
    { UINT32_C(0x80043c24), 0x50u },
    { UINT32_C(0x8004a19c), 0x104u },
    { UINT32_C(0x8004a7bc), 0x7cu },
};

static const XgRenderProtectedCodeRange sprite_ft4_code_ranges[] = {
    { UINT32_C(0x8001e148), 0x150u },
    { UINT32_C(0x8001e298), 0x060u },
    { UINT32_C(0x8001e3d8), 0x5e4u },
    { UINT32_C(0x8001e9bc), 0x4acu },
    { UINT32_C(0x8001f6b0), 0x0a0u },
    { UINT32_C(0x80024ff4), 0x050u },
    { UINT32_C(0x8002675c), 0x274u },
    { UINT32_C(0x8003f738), 0x178u },
    { UINT32_C(0x8004974c), 0x124u },
    { UINT32_C(0x8004987c), 0x110u },
    { UINT32_C(0x80049cec), 0x050u },
    { UINT32_C(0x80049efc), 0x030u },
    { UINT32_C(0x80049f8c), 0x020u },
    { UINT32_C(0x8004a73c), 0x078u },
    { UINT32_C(0x8004a7bc), 0x07cu },
};

static const XgRenderProtectedCodeRange model_ft4_dispatch_data_ranges[] = {
    { UINT32_C(0x8004fe50), 0x2a8u },
};

static const XgRenderProtectedCodeRange shared_trig_data_ranges[] = {
    { UINT32_C(0x800523f0), 0x4000u },
};

#define XG_RENDER_PROTECTED_CODE_PAGE_COUNT 512u
static uint32_t protected_code_page_masks[
    XG_RENDER_PROTECTED_CODE_PAGE_COUNT];
static bool protected_code_page_masks_ready;

static void protected_code_page_mask_add_range(
        uint32_t start, uint32_t size, uint32_t class_mask) {
    const uint64_t begin = (start & UINT32_C(0x1fffffff)) >> 12u;
    uint64_t end;

    if (size == 0u || begin >= XG_RENDER_PROTECTED_CODE_PAGE_COUNT)
        return;
    end = ((uint64_t)(start & UINT32_C(0x1fffffff)) + size - 1u) >> 12u;
    for (uint64_t page = begin;
         page <= end && page < XG_RENDER_PROTECTED_CODE_PAGE_COUNT;
         ++page)
        protected_code_page_masks[page] |= class_mask;
}

static void protected_code_page_mask_add(
        const XgRenderProtectedCodeRange ranges[], uint32_t range_count,
        uint32_t class_mask) {
    for (uint32_t index = 0u; index < range_count; ++index) {
        protected_code_page_mask_add_range(
            ranges[index].start, ranges[index].size, class_mask);
    }
}

static void protected_code_page_mask_add_runtime_variant(
        const XgRenderRuntimeVariantDescriptor *descriptor) {
    const uint32_t class_mask =
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_DESCRIPTOR;

    protected_code_page_mask_add_range(
        descriptor->activation_window_start,
        descriptor->activation_window_size, class_mask);
    protected_code_page_mask_add_range(
        descriptor->physical_producer_entry, 4u, class_mask);
    protected_code_page_mask_add_range(
        descriptor->capture_window_start,
        descriptor->capture_window_size, class_mask);
    protected_code_page_mask_add_range(
        descriptor->physical_return_site, 4u, class_mask);
    protected_code_page_mask_add_range(
        descriptor->model_dispatch_window_start,
        descriptor->model_dispatch_instruction_count * 4u, class_mask);
    for (uint32_t site_index = 0u;
         site_index < descriptor->source_site_count; ++site_index)
        protected_code_page_mask_add_range(
            descriptor->source_sites[site_index].pc, 4u, class_mask);
    for (uint32_t cutover_index = 0u;
         cutover_index < descriptor->cutover_count; ++cutover_index)
        protected_code_page_mask_add_range(
            descriptor->cutovers[cutover_index].code_range_start,
            descriptor->cutovers[cutover_index].code_range_size, class_mask);
}

static void protected_code_page_masks_init(void) {
    if (protected_code_page_masks_ready) return;
    memset(protected_code_page_masks, 0, sizeof(protected_code_page_masks));
    protected_code_page_mask_add(
        zoom_code_ranges,
        (uint32_t)(sizeof(zoom_code_ranges) / sizeof(zoom_code_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_ZOOM);
    protected_code_page_mask_add(
        projected_effect_code_ranges,
        (uint32_t)(sizeof(projected_effect_code_ranges) /
                   sizeof(projected_effect_code_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_PROJECTED);
    protected_code_page_mask_add(
        world_sky_code_ranges,
        (uint32_t)(sizeof(world_sky_code_ranges) /
                   sizeof(world_sky_code_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_SKY);
    protected_code_page_mask_add(
        world_horizon_code_ranges,
        (uint32_t)(sizeof(world_horizon_code_ranges) /
                   sizeof(world_horizon_code_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON);
    protected_code_page_mask_add(
        world_effects_code_ranges,
        (uint32_t)(sizeof(world_effects_code_ranges) /
                   sizeof(world_effects_code_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS);
    protected_code_page_mask_add(
        model_ft4_raw_code_ranges,
        (uint32_t)(sizeof(model_ft4_raw_code_ranges) /
                   sizeof(model_ft4_raw_code_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_FT4);
    protected_code_page_mask_add(
        sprite_ft4_code_ranges,
        (uint32_t)(sizeof(sprite_ft4_code_ranges) /
                   sizeof(sprite_ft4_code_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4);
    protected_code_page_mask_add(
        model_ft4_dispatch_data_ranges,
        (uint32_t)(sizeof(model_ft4_dispatch_data_ranges) /
                   sizeof(model_ft4_dispatch_data_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA);
    protected_code_page_mask_add(
        shared_trig_data_ranges,
        (uint32_t)(sizeof(shared_trig_data_ranges) /
                   sizeof(shared_trig_data_ranges[0])),
        UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);
    for (uint32_t descriptor_index = 0u;
         descriptor_index < xg_render_runtime_variant_descriptor_count;
         ++descriptor_index) {
        const XgRenderRuntimeVariantDescriptor *descriptor =
            &xg_render_runtime_variant_descriptors[descriptor_index];
        if (descriptor_is_valid(descriptor))
            protected_code_page_mask_add_runtime_variant(descriptor);
    }
    protected_code_page_masks_ready = true;
}

static uint32_t protected_code_page_mask_for_write(
        uint32_t write_address, uint32_t write_size) {
    const uint64_t begin =
        (write_address & UINT32_C(0x1fffffff)) >> 12u;
    const uint64_t end =
        ((write_address & UINT32_C(0x1fffffff)) + write_size - 1u) >> 12u;
    uint32_t mask = 0u;

    if (write_size == 0u || begin >= XG_RENDER_PROTECTED_CODE_PAGE_COUNT)
        return 0u;
    protected_code_page_masks_init();
    for (uint64_t page = begin;
         page <= end && page < XG_RENDER_PROTECTED_CODE_PAGE_COUNT;
         ++page)
        mask |= protected_code_page_masks[page];
    return mask;
}

static bool code_write_overlaps_ranges(
        const XgRenderProtectedCodeRange ranges[], uint32_t range_count,
        uint32_t write_address, uint32_t write_size) {
    const uint64_t write_begin = write_address & UINT32_C(0x1fffffff);
    const uint64_t write_end = write_begin + write_size;
    uint32_t index;

    if (write_size == 0u) return false;
    for (index = 0u; index < range_count; ++index) {
        const uint64_t range_begin =
            ranges[index].start & UINT32_C(0x1fffffff);
        const uint64_t range_end = range_begin + ranges[index].size;
        if (ranges[index].size != 0u && range_begin < write_end &&
            write_begin < range_end)
            return true;
    }
    return false;
}

static bool zoom_code_write_overlaps(uint32_t write_address,
                                     uint32_t write_size) {
    return code_write_overlaps_ranges(
        zoom_code_ranges,
        (uint32_t)(sizeof(zoom_code_ranges) / sizeof(zoom_code_ranges[0])),
        write_address, write_size);
}

static bool projected_effect_code_write_overlaps(uint32_t write_address,
                                                  uint32_t write_size) {
    return code_write_overlaps_ranges(
        projected_effect_code_ranges,
        (uint32_t)(sizeof(projected_effect_code_ranges) /
                   sizeof(projected_effect_code_ranges[0])),
        write_address, write_size);
}

static bool world_sky_code_write_overlaps(uint32_t write_address,
                                          uint32_t write_size) {
    return code_write_overlaps_ranges(
        world_sky_code_ranges,
        (uint32_t)(sizeof(world_sky_code_ranges) /
                   sizeof(world_sky_code_ranges[0])),
        write_address, write_size);
}

static bool world_horizon_code_write_overlaps(uint32_t write_address,
                                               uint32_t write_size) {
    return code_write_overlaps_ranges(
        world_horizon_code_ranges,
        (uint32_t)(sizeof(world_horizon_code_ranges) /
                   sizeof(world_horizon_code_ranges[0])),
        write_address, write_size);
}

static bool world_effects_code_write_overlaps(uint32_t write_address,
                                               uint32_t write_size) {
    return code_write_overlaps_ranges(
        world_effects_code_ranges,
        (uint32_t)(sizeof(world_effects_code_ranges) /
                   sizeof(world_effects_code_ranges[0])),
        write_address, write_size);
}

static bool model_ft4_raw_code_write_overlaps(uint32_t write_address,
                                               uint32_t write_size) {
    return code_write_overlaps_ranges(
        model_ft4_raw_code_ranges,
        (uint32_t)(sizeof(model_ft4_raw_code_ranges) /
                   sizeof(model_ft4_raw_code_ranges[0])),
        write_address, write_size);
}

bool xg_render_runtime_variant_model_ft4_code_write_overlaps(
    uint32_t write_address, uint32_t write_size) {
    return model_ft4_raw_code_write_overlaps(write_address, write_size);
}

static bool sprite_ft4_code_write_overlaps(uint32_t write_address,
                                            uint32_t write_size) {
    return code_write_overlaps_ranges(
        sprite_ft4_code_ranges,
        (uint32_t)(sizeof(sprite_ft4_code_ranges) /
                   sizeof(sprite_ft4_code_ranges[0])),
        write_address, write_size);
}

static bool model_ft4_dispatch_data_write_overlaps(uint32_t write_address,
                                                    uint32_t write_size) {
    return code_write_overlaps_ranges(
        model_ft4_dispatch_data_ranges,
        (uint32_t)(sizeof(model_ft4_dispatch_data_ranges) /
                   sizeof(model_ft4_dispatch_data_ranges[0])),
        write_address, write_size);
}

static bool shared_trig_data_write_overlaps(uint32_t write_address,
                                            uint32_t write_size) {
    return code_write_overlaps_ranges(
        shared_trig_data_ranges,
        (uint32_t)(sizeof(shared_trig_data_ranges) /
                   sizeof(shared_trig_data_ranges[0])),
        write_address, write_size);
}

bool xg_render_runtime_variant_sprite_ft4_code_write_overlaps(
        uint32_t write_address, uint32_t write_size) {
    return sprite_ft4_code_write_overlaps(write_address, write_size);
}

void xg_render_runtime_variant_register_code_watches(
    void (*set_range)(uint32_t physical_address, uint32_t size)) {
    uint32_t index;

    if (set_range == NULL) return;
    protected_code_page_masks_init();
    for (uint32_t descriptor_index = 0u;
         descriptor_index < xg_render_runtime_variant_descriptor_count;
         ++descriptor_index) {
        const XgRenderRuntimeVariantDescriptor *descriptor =
            &xg_render_runtime_variant_descriptors[descriptor_index];
        if (!descriptor_is_valid(descriptor)) continue;
        set_range(descriptor->activation_window_start & UINT32_C(0x1fffffff),
                  descriptor->activation_window_size);
        set_range(descriptor->capture_window_start & UINT32_C(0x1fffffff),
                  descriptor->capture_window_size);
        set_range(descriptor->model_dispatch_window_start &
                      UINT32_C(0x1fffffff),
                  descriptor->model_dispatch_instruction_count * 4u);
        for (uint32_t site_index = 0u;
             site_index < descriptor->source_site_count; ++site_index)
            set_range(descriptor->source_sites[site_index].pc &
                          UINT32_C(0x1fffffff),
                      4u);
        for (uint32_t cutover_index = 0u;
             cutover_index < descriptor->cutover_count; ++cutover_index)
            set_range(descriptor->cutovers[cutover_index].code_range_start &
                          UINT32_C(0x1fffffff),
                      descriptor->cutovers[cutover_index].code_range_size);
    }
    for (index = 0u;
         index < sizeof(zoom_code_ranges) / sizeof(zoom_code_ranges[0]); ++index)
        set_range(zoom_code_ranges[index].start & UINT32_C(0x1fffffff),
                  zoom_code_ranges[index].size);
    for (index = 0u;
         index < sizeof(projected_effect_code_ranges) /
                     sizeof(projected_effect_code_ranges[0]); ++index)
        set_range(projected_effect_code_ranges[index].start &
                      UINT32_C(0x1fffffff),
                   projected_effect_code_ranges[index].size);
    for (index = 0u;
         index < sizeof(world_sky_code_ranges) /
                     sizeof(world_sky_code_ranges[0]); ++index)
        set_range(world_sky_code_ranges[index].start &
                       UINT32_C(0x1fffffff),
                  world_sky_code_ranges[index].size);
    for (index = 0u;
         index < sizeof(world_horizon_code_ranges) /
                     sizeof(world_horizon_code_ranges[0]); ++index)
        set_range(world_horizon_code_ranges[index].start &
                      UINT32_C(0x1fffffff),
                   world_horizon_code_ranges[index].size);
    for (index = 0u;
         index < sizeof(world_effects_code_ranges) /
                     sizeof(world_effects_code_ranges[0]); ++index)
        set_range(world_effects_code_ranges[index].start &
                      UINT32_C(0x1fffffff),
                  world_effects_code_ranges[index].size);
    for (index = 0u;
         index < sizeof(model_ft4_raw_code_ranges) /
                     sizeof(model_ft4_raw_code_ranges[0]); ++index)
        set_range(model_ft4_raw_code_ranges[index].start &
                      UINT32_C(0x1fffffff),
                  model_ft4_raw_code_ranges[index].size);
    for (index = 0u;
         index < sizeof(sprite_ft4_code_ranges) /
                      sizeof(sprite_ft4_code_ranges[0]); ++index)
        set_range(sprite_ft4_code_ranges[index].start &
                       UINT32_C(0x1fffffff),
                  sprite_ft4_code_ranges[index].size);
    for (index = 0u;
         index < sizeof(model_ft4_dispatch_data_ranges) /
                     sizeof(model_ft4_dispatch_data_ranges[0]); ++index)
        set_range(model_ft4_dispatch_data_ranges[index].start &
                      UINT32_C(0x1fffffff),
                  model_ft4_dispatch_data_ranges[index].size);
    for (index = 0u;
         index < sizeof(shared_trig_data_ranges) /
                     sizeof(shared_trig_data_ranges[0]); ++index)
        set_range(shared_trig_data_ranges[index].start &
                      UINT32_C(0x1fffffff),
                  shared_trig_data_ranges[index].size);
}

uint32_t xg_render_runtime_variant_code_write_overlap_mask(
    uint32_t write_address, uint32_t write_size) {
    uint32_t mask = 0u;
    const uint32_t page_mask =
        protected_code_page_mask_for_write(write_address, write_size);

    if (page_mask & (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_DESCRIPTOR)) {
        for (uint32_t index = 0u;
             index < xg_render_runtime_variant_descriptor_count; ++index) {
            const XgRenderRuntimeVariantDescriptor *descriptor =
                state.descriptor != NULL ? state.descriptor :
                    &xg_render_runtime_variant_descriptors[index];
            const bool descriptor_valid = descriptor_is_valid(descriptor);
            bool descriptor_overlap = descriptor_valid &&
                (normalized_ranges_overlap(descriptor->activation_window_start,
                                           descriptor->activation_window_size,
                                           write_address, write_size) ||
                 normalized_ranges_overlap(
                     descriptor->physical_producer_entry, 4u,
                     write_address, write_size) ||
                 normalized_ranges_overlap(descriptor->capture_window_start,
                                           descriptor->capture_window_size,
                                           write_address, write_size) ||
                 normalized_ranges_overlap(
                     descriptor->model_dispatch_window_start,
                     descriptor->model_dispatch_instruction_count * 4u,
                     write_address, write_size) ||
                 normalized_ranges_overlap(descriptor->physical_return_site,
                                           4u, write_address, write_size));
            if (descriptor_overlap) {
                mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_DESCRIPTOR;
                break;
            }
            if (descriptor_valid) {
                for (uint32_t site_index = 0u;
                     site_index < descriptor->source_site_count; ++site_index) {
                    if (normalized_ranges_overlap(
                            descriptor->source_sites[site_index].pc, 4u,
                            write_address, write_size)) {
                        descriptor_overlap = true;
                        break;
                    }
                }
                for (uint32_t cutover_index = 0u;
                     !descriptor_overlap &&
                     cutover_index < descriptor->cutover_count; ++cutover_index) {
                    const XgRenderRuntimeVariantCutover *cutover =
                        &descriptor->cutovers[cutover_index];
                    if (normalized_ranges_overlap(
                            cutover->code_range_start, cutover->code_range_size,
                            write_address, write_size))
                        descriptor_overlap = true;
                }
            }
            if (descriptor_overlap) {
                mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_DESCRIPTOR;
                break;
            }
            if (state.descriptor != NULL) break;
        }
    }
    if ((page_mask & (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_ZOOM)) &&
        zoom_code_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_ZOOM;
    if ((page_mask & (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_PROJECTED)) &&
        projected_effect_code_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_PROJECTED;
    if ((page_mask & (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_SKY)) &&
        world_sky_code_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_SKY;
    if ((page_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON)) &&
        world_horizon_code_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_HORIZON;
    if ((page_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS)) &&
        world_effects_code_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_WORLD_EFFECTS;
    if ((page_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_FT4)) &&
        model_ft4_raw_code_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_FT4;
    if ((page_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4)) &&
        sprite_ft4_code_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SPRITE_FT4;
    if ((page_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA)) &&
        model_ft4_dispatch_data_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) <<
            PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA;
    if ((page_mask &
         (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA)) &&
        shared_trig_data_write_overlaps(write_address, write_size))
        mask |= UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA;
    return mask;
}

bool xg_render_runtime_variant_active_code_write_overlaps(
    uint32_t write_address, uint32_t write_size) {
    const uint32_t data_mask =
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_MODEL_DISPATCH_DATA) |
        (UINT32_C(1) << PSX_XG_RENDER_CODE_WRITE_SHARED_TRIG_DATA);

    return (xg_render_runtime_variant_code_write_overlap_mask(
                write_address, write_size) & ~data_mask) != 0u;
}

XgRenderRuntimeVariantEvent xg_render_runtime_variant_observe(
    uint32_t hook, uint32_t pc, uint32_t instruction_word,
    uint32_t delay_slot_word, uint32_t return_address,
    uint64_t scene_generation) {
    const XgRenderRuntimeVariantDescriptor *descriptor = state.descriptor;
    bool activation_site_seen = false;

    if (descriptor != NULL) {
        if (state.scene_generation != scene_generation) {
            xg_render_runtime_variant_reset();
            return XG_RENDER_RUNTIME_VARIANT_REJECT;
        }
        if (state.phase == XG_RENDER_RUNTIME_VARIANT_EXPECT_ENTRY &&
            hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY &&
            physical_address_equals(pc, descriptor->physical_producer_entry) &&
            physical_address_equals(return_address,
                                    descriptor->activation_site + 8u)) {
            state.phase = XG_RENDER_RUNTIME_VARIANT_EXPECT_CAPTURE;
            return XG_RENDER_RUNTIME_VARIANT_ENTRY;
        }
        if (state.phase == XG_RENDER_RUNTIME_VARIANT_EXPECT_CAPTURE) {
            if (physical_address_equals(pc, descriptor->capture_site)) {
                if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY)
                    return XG_RENDER_RUNTIME_VARIANT_CONSUMED;
                if (hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE &&
                    jal_matches(pc, instruction_word,
                                descriptor->capture_jal_target,
                                delay_slot_word,
                                descriptor->capture_delay_instruction) &&
                    physical_address_equals(return_address,
                                            descriptor->physical_return_site)) {
                    state.phase = XG_RENDER_RUNTIME_VARIANT_EXPECT_RETURN;
                    return XG_RENDER_RUNTIME_VARIANT_CAPTURE;
                }
                xg_render_runtime_variant_reset();
                return XG_RENDER_RUNTIME_VARIANT_REJECT;
            }
            if ((hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY &&
                 physical_address_equals(pc,
                                         descriptor->physical_producer_entry)) ||
                (hook == PSX_XG_RENDER_AUTH_HOOK_RETURN &&
                 physical_address_equals(pc, descriptor->physical_return_site))) {
                xg_render_runtime_variant_reset();
                return XG_RENDER_RUNTIME_VARIANT_REJECT;
            }
            if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY ||
                hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE ||
                hook == PSX_XG_RENDER_AUTH_HOOK_RETURN)
                return XG_RENDER_RUNTIME_VARIANT_CONSUMED;
        }
        if (state.phase == XG_RENDER_RUNTIME_VARIANT_EXPECT_RETURN &&
            (hook == PSX_XG_RENDER_AUTH_HOOK_RETURN ||
             hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY) &&
            physical_address_equals(pc, descriptor->physical_return_site) &&
            physical_address_equals(return_address,
                                    descriptor->physical_return_site)) {
            return XG_RENDER_RUNTIME_VARIANT_RETURN;
        }
        if (state.phase == XG_RENDER_RUNTIME_VARIANT_EXPECT_RETURN &&
            (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY ||
             hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE ||
             hook == PSX_XG_RENDER_AUTH_HOOK_RETURN) &&
            !range_contains(descriptor->artifact_range_start,
                            descriptor->artifact_range_size, pc, 1u))
            return XG_RENDER_RUNTIME_VARIANT_CONSUMED;
        xg_render_runtime_variant_reset();
        return XG_RENDER_RUNTIME_VARIANT_REJECT;
    }
    if (hook != PSX_XG_RENDER_AUTH_HOOK_CAPTURE) return XG_RENDER_RUNTIME_VARIANT_IGNORE;
    descriptor = NULL;
    for (uint32_t index = 0u;
         index < xg_render_runtime_variant_descriptor_count; ++index) {
        const XgRenderRuntimeVariantDescriptor *candidate =
            &xg_render_runtime_variant_descriptors[index];
        if (!descriptor_is_valid(candidate) || !physical_address_equals(
                pc, candidate->activation_site))
            continue;
        activation_site_seen = true;
        if (!jal_matches(pc, instruction_word,
                         candidate->activation_jal_target,
                         delay_slot_word,
                         candidate->activation_delay_instruction))
            continue;
        descriptor = candidate;
        break;
    }
    if (descriptor == NULL) return activation_site_seen
        ? XG_RENDER_RUNTIME_VARIANT_REJECT
        : XG_RENDER_RUNTIME_VARIANT_IGNORE;
    state = (XgRenderRuntimeVariantState){
        descriptor, scene_generation, XG_RENDER_RUNTIME_VARIANT_EXPECT_ENTRY,
    };
    return XG_RENDER_RUNTIME_VARIANT_ACTIVATED;
}

bool xg_render_runtime_variant_event_is_exact(
    XgRenderRuntimeVariantEvent event, uint32_t pc) {
    const XgRenderRuntimeVariantDescriptor *descriptor = state.descriptor;

    if (descriptor == NULL) return false;
    switch (event) {
    case XG_RENDER_RUNTIME_VARIANT_ACTIVATED:
        return pc == descriptor->activation_site;
    case XG_RENDER_RUNTIME_VARIANT_ENTRY:
        return pc == descriptor->physical_producer_entry;
    case XG_RENDER_RUNTIME_VARIANT_CAPTURE:
        return pc == descriptor->capture_site;
    case XG_RENDER_RUNTIME_VARIANT_RETURN:
        return pc == descriptor->physical_return_site;
    default:
        return false;
    }
}
