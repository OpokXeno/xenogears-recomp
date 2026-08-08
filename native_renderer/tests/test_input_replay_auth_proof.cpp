#include "game_identity.h"
#include "guest_render_bridge.h"
#include "cpu_state.h"
#include "input_replay.h"
#include "xg_render_auth.h"
#include "xg_render_auth_runtime.h"
#include "xg_render_manifest_generated.h"
#include "xg_render_static_auth.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {
constexpr uint32_t kProducerEntry = 0x80075b44u;
constexpr uint32_t kCallerSite = 0x800781bcu;
constexpr uint32_t kStaticCallee = 0x8004b54cu;
constexpr uint32_t kReturnSite = 0x800781c4u;
constexpr uint32_t kJalInstruction = 0x0c012d53u;
constexpr uint32_t kFieldRangeStart = 0x8006f000u;
constexpr uint32_t kFieldRangeSize = 282624u;
constexpr uint32_t kCandidateRangeSize = kReturnSite - kProducerEntry;
constexpr uint32_t kField5ActivationSite = 0x80075414u;
constexpr uint32_t kField5ProducerEntry = 0x800764b4u;
constexpr uint32_t kField5CaptureSite = 0x80075694u;
constexpr uint32_t kField5ActivationJal = 0x0c01d92du;
constexpr uint32_t kField5ActivationDelay = 0x248400ccu;
constexpr uint32_t kField5CaptureDelay = 0x34040001u;

PsxGameIdentity runtime_identity{};

uint32_t hook_return_address(uint32_t hook, uint32_t pc) {
    if (hook == PSX_XG_RENDER_AUTH_HOOK_ENTRY &&
        (pc & 0x1fffffffu) == (kField5ProducerEntry & 0x1fffffffu))
        return kField5ActivationSite + 8u;
    return hook == PSX_XG_RENDER_AUTH_HOOK_CAPTURE ? pc + 8u : pc;
}

void cold_hook(uint32_t hook, uint32_t pc, uint32_t instruction_word,
               uint32_t delay_slot_word) {
    CPUState cpu{};

    cpu.gpr[31] = hook_return_address(hook, pc);
    psx_xg_render_auth_cold_hook(&cpu, hook, pc, instruction_word,
                                 delay_slot_word);
}

void warm_hook(CPUState* cpu, uint32_t hook, uint32_t pc,
               uint32_t instruction_word, uint32_t delay_slot_word) {
    CPUState synthetic_cpu{};

    if (cpu == nullptr) {
        synthetic_cpu.gpr[31] = hook_return_address(hook, pc);
        cpu = &synthetic_cpu;
    }
    psx_xg_render_auth_warm_hook(cpu, hook, pc, instruction_word,
                                 delay_slot_word);
}

#define psx_xg_render_auth_cold_hook cold_hook
#define psx_xg_render_auth_warm_hook warm_hook

void write_trace(const std::string& path) {
    std::ofstream output(path);

    output << "schema = \"xenogears.native-render-replay/v2\"\n"
           << "complete = true\n"
           << "vblank_budget = 1\n"
           << "record_stop_field = 5\n"
           << "record_stable_vblanks = 4\n"
           << "[checkpoint]\nkind = \"u16\"\naddress = \"0x8006F94E\"\nequals = 5\n"
           << "[[vblank]]\nrepeat = 1\n";
    for (int slot = 1; slot <= 2; ++slot) {
        output << "p" << slot << "_connected = "
               << (slot == 1 ? "true" : "false") << "\n"
               << "p" << slot << "_mode = \"digital\"\n"
               << "p" << slot << "_buttons = []\n"
               << "p" << slot << "_left_x = 0\n"
               << "p" << slot << "_left_y = 0\n"
               << "p" << slot << "_right_x = 0\n"
               << "p" << slot << "_right_y = 0\n"
               << "p" << slot << "_trigger_left = 0\n"
               << "p" << slot << "_trigger_right = 0\n";
    }
}

std::string read_file(const std::string& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string diagnostic(const std::string& evidence) {
    const size_t begin = evidence.find("\"diagnostic\":");
    const size_t instrumentation = evidence.find(",\"instrumentation\":", begin);
    assert(begin != std::string::npos);
    assert(instrumentation != std::string::npos);
    return evidence.substr(begin + std::strlen("\"diagnostic\":"),
                           instrumentation - begin - std::strlen("\"diagnostic\":")) + "}";
}

std::string instrumentation(const std::string& evidence) {
    const size_t begin = evidence.find("\"instrumentation\":");
    assert(begin != std::string::npos);
    const size_t end = evidence.find('}', begin);
    assert(end != std::string::npos);
    return evidence.substr(begin + std::strlen("\"instrumentation\":"),
                           end - begin - std::strlen("\"instrumentation\":" ) + 1u);
}

void assert_auth_proof_privacy(const std::string& evidence) {
    const std::string privacy =
        "\"privacy\":{\"metadata_only\":true,\"raw_instruction_words\":false,"
        "\"raw_delay_slot_words\":false,\"identities_or_digests\":false,"
        "\"private_paths\":false,\"disc_cards_cache_hashes\":false,"
        "\"input_states\":false,\"packets\":false,"
        "\"child_runtime_json\":false}";
    const std::array<const char*, 12> prohibited_keys = {
        "\"instruction_word\":", "\"delay_slot_word\":",
        "\"static_game_identity\":", "\"runtime_game_identity\":",
        "\"field_image_identity\":", "\"manifest_identity\":",
        "\"identity_digest\":", "\"cache_digest\":",
        "\"private_path\":", "\"disc_path\":", "\"payload\":",
        "\"authorization_handle\":",
    };

    assert(evidence.find(privacy) != std::string::npos);
    for (const char* key : prohibited_keys)
        assert(evidence.find(key) == std::string::npos);
}

PsxXgRenderAuthCandidate matching_candidate() {
    PsxXgRenderAuthCandidate candidate = {
        kProducerEntry, kProducerEntry, kCandidateRangeSize, kProducerEntry,
    };

    std::copy(std::begin(xg_render_game_identity), std::end(xg_render_game_identity),
              std::begin(candidate.identity.game_sha256));
    std::copy(std::begin(xg_render_manifest_identity),
              std::end(xg_render_manifest_identity),
              std::begin(candidate.identity.manifest_sha256));
    candidate.pair_id = UINT64_C(0x1020304050607080);
    candidate.artifact_base = kFieldRangeStart;
    candidate.artifact_size = kFieldRangeSize;
    candidate.artifact_crc32 = 0x11223344u;
    candidate.authority_provenance = true;
    candidate.pair_bound = true;
    return candidate;
}

void complete_cold_runtime_trace() {
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kProducerEntry, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 kCallerSite, kJalInstruction, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 kReturnSite, 0u, 0u);
}

void complete_warm_runtime_trace() {
    psx_xg_render_auth_warm_hook(nullptr, PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kProducerEntry, 0u, 0u);
    psx_xg_render_auth_warm_hook(nullptr, PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 kCallerSite, kJalInstruction, 0u);
    psx_xg_render_auth_warm_hook(nullptr, PSX_XG_RENDER_AUTH_HOOK_RETURN,
                                 kReturnSite, 0u, 0u);
}

void assert_original_render_mode() {
    GuestRenderBridgeSnapshot bridge{};

    assert(guest_render_bridge_snapshot(&bridge) == GUEST_RENDER_OK);
    assert(bridge.modes.effective_render_mode == GUEST_RENDER_RENDER_ORIGINAL);
}
}

extern "C" {
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
    3u, 4u, 0x11223344u, 0x55667788u, kFieldRangeStart, kFieldRangeSize,
    kProducerEntry, kCallerSite, kStaticCallee, kReturnSite,
    0x800781b4u, 16u,
    {0x50u, 0x51u, 0x52u, 0x53u, 0x54u, 0x55u, 0x56u, 0x57u,
     0x58u, 0x59u, 0x5au, 0x5bu, 0x5cu, 0x5du, 0x5eu, 0x5fu,
     0x60u, 0x61u, 0x62u, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u,
     0x68u, 0x69u, 0x6au, 0x6bu, 0x6cu, 0x6du, 0x6eu, 0x6fu},
    3u, kStaticCallee, 1u, 1u,
};
const XgRenderManifestRecord xg_render_manifest_records[] = {
    {3u, "producer", 3u, kProducerEntry, 0u,
     {0x70u, 0x71u, 0x72u, 0x73u, 0x74u, 0x75u, 0x76u, 0x77u,
      0x78u, 0x79u, 0x7au, 0x7bu, 0x7cu, 0x7du, 0x7eu, 0x7fu,
      0x80u, 0x81u, 0x82u, 0x83u, 0x84u, 0x85u, 0x86u, 0x87u,
      0x88u, 0x89u, 0x8au, 0x8bu, 0x8cu, 0x8du, 0x8eu, 0x8fu},
     {0}, "field-double-buffer", "field-ot"},
    {4u, "site", 4u, kCallerSite, kStaticCallee,
     {0},
     {0x50u, 0x51u, 0x52u, 0x53u, 0x54u, 0x55u, 0x56u, 0x57u,
      0x58u, 0x59u, 0x5au, 0x5bu, 0x5cu, 0x5du, 0x5eu, 0x5fu,
      0x60u, 0x61u, 0x62u, 0x63u, 0x64u, 0x65u, 0x66u, 0x67u,
      0x68u, 0x69u, 0x6au, 0x6bu, 0x6cu, 0x6du, 0x6eu, 0x6fu},
     "field-double-buffer", "field-ot"},
};
const uint32_t xg_render_manifest_record_count = 2u;

int psx_game_identity_equal(const PsxGameIdentity* left,
                            const PsxGameIdentity* right) {
    return left != nullptr && right != nullptr &&
           std::memcmp(left, right, sizeof(*left)) == 0;
}

const PsxGameIdentity* psx_game_identity_runtime(void) {
    return &runtime_identity;
}

int psx_game_identity_bind_static(const PsxGameIdentity* identity) {
    return psx_game_identity_equal(identity, &runtime_identity);
}

int psx_game_identity_gate(const PsxGameIdentity* identity) {
    return psx_game_identity_equal(identity, &runtime_identity);
}

uint64_t gpu_render_vram_mutation_serial(void) { return 0u; }
bool gpu_render_vram_mutation_overflowed(void) { return false; }
}

int main() {
    const std::string trace_path = "input_replay_auth_proof_test.toml";
    const std::string evidence_path = "input_replay_auth_proof_evidence.json";
    const std::string record_path = "input_replay_auth_record.toml";
    const std::string record_evidence_path =
        "input_replay_auth_record_evidence.json";
    SDL_GameController* players[2] = {nullptr, nullptr};
    XgRenderAuth* auth = nullptr;
    XgRenderAuthSnapshot live_auth{};
    PsxXgRenderAuthCompletedProofReceipt warm_receipt{};
    std::string error;

    guest_render_bridge_test_reset();
    std::copy(std::begin(xg_render_game_identity), std::end(xg_render_game_identity),
              runtime_identity.game_sha256);
    std::copy(std::begin(xg_render_manifest_identity), std::end(xg_render_manifest_identity),
              runtime_identity.manifest_sha256);
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    std::remove(record_path.c_str());
    std::remove(record_evidence_path.c_str());
    assert(input_replay::record_begin_until_close(
        record_path.c_str(), 5u, 2u, &error));
    input_replay::record_note_guest_vblank();
    assert(input_replay::record_snapshot(
        input_replay::HostInputSnapshot{}, &error));
    assert(input_replay::record_close(&error));
    assert(input_replay::write_record_evidence(
        record_evidence_path.c_str(), &error));
    const std::string record_evidence = read_file(record_evidence_path);
    assert(record_evidence.find(
        "\"schema\":\"xenogears.native-render-session-evidence/v2\"") !=
        std::string::npos);
    assert(record_evidence.find("\"model_ft4\"") != std::string::npos);
    assert(record_evidence.find("\"sprite_ft4\"") != std::string::npos);
    std::remove(record_path.c_str());
    std::remove(record_evidence_path.c_str());
    write_trace(trace_path);
    assert(input_replay::load(trace_path.c_str(), &error));
    assert(input_replay::attach(players, &error));
    assert(input_replay::latch_vblank());
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kFieldRangeStart, 0u, 0u);
    PsxXgRenderAuthInstrumentation instrumentation_before{};
    PsxXgRenderAuthInstrumentation instrumentation_after{};
    psx_xg_render_auth_instrumentation_snapshot(&instrumentation_before);
    input_replay::note_snapshot({1u, 0u, UINT32_MAX, 0u, 5u, 5u, 0u, true});
    psx_xg_render_auth_instrumentation_snapshot(&instrumentation_after);
    assert(instrumentation_after.scene_boundary_count ==
           instrumentation_before.scene_boundary_count);
    input_replay::observe_checkpoint(5u);
    assert(input_replay::stop_reason() == input_replay::StopReason::CheckpointReached);

    psx_xg_render_auth_scene_boundary();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string blocked_static_only = read_file(evidence_path);
    assert(blocked_static_only.find("\"world_effects\"") != std::string::npos);
    assert(blocked_static_only.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(blocked_static_only.find(
        "\"static\":{\"accepted\":true,\"provenance\":{\"source\":\"manifest-overlay\",\"image\":\"field-image\",\"producer_entry\":" +
        std::to_string(kProducerEntry) + ",\"range_start\":" +
        std::to_string(kFieldRangeStart) + ",\"range_size\":" +
        std::to_string(kFieldRangeSize) +
        ",\"manifest_bound\":true,\"range_bound\":true,\"candidate\":{\"matched\":false,\"dispatched\":false}}}" ) != std::string::npos);
    assert(blocked_static_only.find("\"runtime\":{\"accepted\":false,\"tier\":\"none\"") != std::string::npos);
    assert(blocked_static_only.find("\"tuple\":") == std::string::npos);
    assert(blocked_static_only.find("\"trace\":") == std::string::npos);
    assert(diagnostic(blocked_static_only) ==
           "{\"available\":true,\"producer_begin_count\":0,\"hook_count\":0,\"trace_event_count\":0,\"trace_overflowed\":false,\"accepted_entry\":false,\"accepted_capture\":false,\"accepted_return\":false,\"rejected_event_count\":0,\"reset_since_trace_start\":false,\"scene_aborted\":false,\"reject_reason\":\"none\",\"rejection_source\":\"none\",\"rejection_hook\":\"none\",\"rejection_guest_pc\":0}");

    psx_xg_render_auth_scene_boundary();
    complete_cold_runtime_trace();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string cold_observed = read_file(evidence_path);
    const std::string tuple = "\"tuple\":{\"producer_entry\":" +
        std::to_string(kProducerEntry) + ",\"capture_site\":" +
        std::to_string(kCallerSite) + ",\"static_callee\":" +
        std::to_string(kStaticCallee) + ",\"return_site\":" +
        std::to_string(kReturnSite) + "}";
    const std::string static_provenance =
        "\"static\":{\"accepted\":true,\"provenance\":{\"source\":\"manifest-overlay\",\"image\":\"field-image\",\"producer_entry\":" +
        std::to_string(kProducerEntry) + ",\"range_start\":" +
        std::to_string(kFieldRangeStart) + ",\"range_size\":" +
        std::to_string(kFieldRangeSize) +
        ",\"manifest_bound\":true,\"range_bound\":true,\"candidate\":{\"matched\":false,\"dispatched\":false}}}";
    assert(cold_observed.find("\"schema\":\"xenogears.native-render-auth-proof/v4\"") != std::string::npos);
    assert(cold_observed.find("\"status\":\"OBSERVED\"") != std::string::npos);
    assert(cold_observed.find(static_provenance) != std::string::npos);
    assert(cold_observed.find("\"tier\":\"cold\"") != std::string::npos);
    assert(cold_observed.find(tuple) != std::string::npos);
    assert(cold_observed.find("\"trace\":{\"entry_sequence\":") != std::string::npos);
    assert(cold_observed.find("\"scene_epoch\":") != std::string::npos);
    assert(cold_observed.find("\"state_sequence\":") != std::string::npos);
    assert(cold_observed.find("\"field_binding\":{\"checkpoint_field_id\":5,\"checkpoint_seen\":true,\"checkpoint_seen_vblank\":1,\"evidence_vblank\":1,\"context_valid\":true,\"context_field_id\":5}") != std::string::npos);
    assert(cold_observed.find("\"static\":{\"accepted\":true,\"tuple\":") == std::string::npos);
    assert_auth_proof_privacy(cold_observed);
    assert(instrumentation(cold_observed).find("\"revision\":1,") != std::string::npos);
    assert(instrumentation(cold_observed).find(
        "\"cold_hook_ingress_count\":3,") != std::string::npos);
    for (const char* field : {
             "\"cold_hook_ingress_count\":", "\"activation_physical_count\":",
             "\"activation_exact_count\":",
             "\"entry_physical_count\":", "\"entry_exact_count\":",
             "\"capture_physical_count\":", "\"capture_exact_count\":",
             "\"return_physical_count\":", "\"return_exact_count\":",
             "\"last_progress_sequence\":", "\"last_reset_sequence\":",
             "\"last_publish_sequence\":", "\"scene_boundary_count\":",
             "\"disarm_count\":", "\"completed_proof_publication_count\":",
         })
        assert(instrumentation(cold_observed).find(field) != std::string::npos);
    assert(diagnostic(cold_observed) ==
           "{\"available\":true,\"producer_begin_count\":1,\"hook_count\":3,\"trace_event_count\":3,\"trace_overflowed\":false,\"accepted_entry\":true,\"accepted_capture\":true,\"accepted_return\":true,\"rejected_event_count\":0,\"reset_since_trace_start\":false,\"scene_aborted\":false,\"reject_reason\":\"none\",\"rejection_source\":\"none\",\"rejection_hook\":\"none\",\"rejection_guest_pc\":0}");

    psx_xg_render_auth_scene_boundary();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string reset_rearmed = read_file(evidence_path);
    assert(reset_rearmed.find("\"status\":\"OBSERVED\"") != std::string::npos);
    assert(reset_rearmed.find("\"runtime\":{\"accepted\":true,\"tier\":\"cold\",\"reject_reason\":\"none\",\"scene_aborted\":false,\"ir_usable\":true,\"native_permitted\":true") != std::string::npos);
    assert(reset_rearmed.find(tuple) != std::string::npos);
    assert(xg_render_auth_process_owner(&auth) == XG_RENDER_AUTH_OK);
    assert(xg_render_auth_snapshot(auth, &live_auth) == XG_RENDER_AUTH_OK);
    assert(!live_auth.native_use_permitted);
    assert(diagnostic(reset_rearmed) ==
           "{\"available\":true,\"producer_begin_count\":1,\"hook_count\":3,\"trace_event_count\":3,\"trace_overflowed\":false,\"accepted_entry\":true,\"accepted_capture\":true,\"accepted_return\":true,\"rejected_event_count\":0,\"reset_since_trace_start\":true,\"scene_aborted\":false,\"reject_reason\":\"none\",\"rejection_source\":\"none\",\"rejection_hook\":\"none\",\"rejection_guest_pc\":0}");
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kProducerEntry, 0u, 0u);
    psx_xg_render_auth_loader_mismatch(kProducerEntry);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string rejected_runtime = read_file(evidence_path);
    assert(rejected_runtime.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(rejected_runtime.find("\"runtime\":{\"accepted\":false,\"tier\":\"cold\",\"reject_reason\":\"validation_mismatch\",\"scene_aborted\":true,\"ir_usable\":false,\"native_permitted\":false") != std::string::npos);
    assert(rejected_runtime.find("\"tuple\":") == std::string::npos);
    assert(rejected_runtime.find("\"trace\":") == std::string::npos);
    assert(diagnostic(rejected_runtime) ==
           "{\"available\":true,\"producer_begin_count\":1,\"hook_count\":3,\"trace_event_count\":5,\"trace_overflowed\":false,\"accepted_entry\":true,\"accepted_capture\":true,\"accepted_return\":true,\"rejected_event_count\":1,\"reset_since_trace_start\":true,\"scene_aborted\":true,\"reject_reason\":\"validation_mismatch\",\"rejection_source\":\"loader_mismatch\",\"rejection_hook\":\"none\",\"rejection_guest_pc\":2147965764}");

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 kField5ActivationSite, kField5ActivationJal,
                                 kField5ActivationDelay);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kField5ProducerEntry, 0u, 0u);
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_CAPTURE,
                                 kField5CaptureSite, 0u,
                                 kField5CaptureDelay);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string rejected_variant = read_file(evidence_path);
    assert(rejected_variant.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(diagnostic(rejected_variant) ==
           "{\"available\":true,\"producer_begin_count\":1,\"hook_count\":3,\"trace_event_count\":7,\"trace_overflowed\":false,\"accepted_entry\":true,\"accepted_capture\":true,\"accepted_return\":true,\"rejected_event_count\":2,\"reset_since_trace_start\":true,\"scene_aborted\":true,\"reject_reason\":\"validation_mismatch\",\"rejection_source\":\"loader_mismatch\",\"rejection_hook\":\"none\",\"rejection_guest_pc\":2147965764}");

    psx_xg_render_auth_scene_boundary();
    complete_cold_runtime_trace();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string newer_cold = read_file(evidence_path);
    assert(newer_cold.find("\"status\":\"OBSERVED\"") != std::string::npos);
    assert(newer_cold.find("\"tier\":\"cold\"") != std::string::npos);
    assert(newer_cold.find("\"candidate\":{\"matched\":false,\"dispatched\":false}") != std::string::npos);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kProducerEntry, 0u, 0u);
    psx_xg_render_auth_loader_mismatch(kProducerEntry);

    psx_xg_render_auth_scene_boundary();
    const PsxXgRenderAuthCandidate candidate = matching_candidate();
    psx_xg_render_auth_note_candidate_dispatch(&candidate);
    complete_warm_runtime_trace();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string warm_observed = read_file(evidence_path);
    assert(warm_observed.find("\"status\":\"OBSERVED\"") != std::string::npos);
    assert(warm_observed.find("\"tier\":\"warm\"") != std::string::npos);
    assert(warm_observed.find("\"candidate\":{\"matched\":true,\"dispatched\":true}") != std::string::npos);

    psx_xg_render_auth_completed_proof_snapshot(&warm_receipt);
    assert(warm_receipt.available);
    assert(!warm_receipt.blocked);
    assert(warm_receipt.tier == XG_RENDER_AUTH_TIER_WARM_NATIVE);
    assert(warm_receipt.candidate_matched);
    assert(warm_receipt.candidate_dispatched);
    for (size_t index = 0u; index <= xg_render_auth_trace_capacity(); ++index) {
        psx_xg_render_auth_scene_boundary();
        psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                     kProducerEntry, 0u, 0u);
    }
    psx_xg_render_auth_scene_boundary();
    assert(xg_render_auth_snapshot(auth, &live_auth) == XG_RENDER_AUTH_OK);
    assert(!live_auth.native_use_permitted);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string warm_retained = read_file(evidence_path);
    const std::string retained_trace =
        "\"trace\":{\"entry_sequence\":" +
        std::to_string(warm_receipt.entry_event_sequence) +
        ",\"capture_sequence\":" +
        std::to_string(warm_receipt.capture_event_sequence) +
        ",\"return_sequence\":" +
        std::to_string(warm_receipt.return_event_sequence) +
        ",\"scene_epoch\":" +
        std::to_string(warm_receipt.state_id.scene_epoch) +
        ",\"state_sequence\":" +
        std::to_string(warm_receipt.state_id.state_sequence) + "}";
    assert(warm_retained.find("\"status\":\"OBSERVED\"") != std::string::npos);
    assert(warm_retained.find("\"runtime\":{\"accepted\":true,\"tier\":\"warm\",\"reject_reason\":\"none\",\"scene_aborted\":false,\"ir_usable\":true,\"native_permitted\":true") != std::string::npos);
    assert(warm_retained.find(tuple) != std::string::npos);
    assert(warm_retained.find(retained_trace) != std::string::npos);
    assert(warm_retained.find("\"candidate\":{\"matched\":true,\"dispatched\":true}") != std::string::npos);
    assert_auth_proof_privacy(warm_retained);
    assert(diagnostic(warm_retained) ==
           "{\"available\":true,\"producer_begin_count\":1,\"hook_count\":3,\"trace_event_count\":64,\"trace_overflowed\":true,\"accepted_entry\":true,\"accepted_capture\":true,\"accepted_return\":true,\"rejected_event_count\":0,\"reset_since_trace_start\":true,\"scene_aborted\":false,\"reject_reason\":\"none\",\"rejection_source\":\"none\",\"rejection_hook\":\"none\",\"rejection_guest_pc\":0}");

    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kProducerEntry, 0u, 0u);
    psx_xg_render_auth_loader_mismatch(kProducerEntry);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string warm_blocked = read_file(evidence_path);
    assert(warm_blocked.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(warm_blocked.find("\"runtime\":{\"accepted\":false,\"tier\":\"warm\",\"reject_reason\":\"validation_mismatch\",\"scene_aborted\":true,\"ir_usable\":false,\"native_permitted\":false") != std::string::npos);
    assert(warm_blocked.find("\"tuple\":") == std::string::npos);
    assert(warm_blocked.find("\"trace\":") == std::string::npos);
    assert_auth_proof_privacy(warm_blocked);
    assert(diagnostic(warm_blocked) ==
           "{\"available\":true,\"producer_begin_count\":1,\"hook_count\":3,\"trace_event_count\":64,\"trace_overflowed\":true,\"accepted_entry\":true,\"accepted_capture\":true,\"accepted_return\":true,\"rejected_event_count\":1,\"reset_since_trace_start\":true,\"scene_aborted\":true,\"reject_reason\":\"validation_mismatch\",\"rejection_source\":\"loader_mismatch\",\"rejection_hook\":\"none\",\"rejection_guest_pc\":2147965764}");

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_cold_hook(PSX_XG_RENDER_AUTH_HOOK_ENTRY,
                                 kProducerEntry, 0u, 0u);
    psx_xg_render_auth_native_bad_entry(kProducerEntry, kCallerSite);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string first_blocker_retained = read_file(evidence_path);
    assert(first_blocker_retained.find("\"rejection_source\":\"loader_mismatch\"") != std::string::npos);
    assert(first_blocker_retained.find("\"rejection_guest_pc\":2147965764") != std::string::npos);
    assert(first_blocker_retained.find("native_bad_entry") == std::string::npos);

    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_note_candidate_dispatch(&candidate);
    psx_xg_render_auth_scene_boundary();
    complete_warm_runtime_trace();
    assert_original_render_mode();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string stale_source = read_file(evidence_path);
    assert(stale_source.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(stale_source.find("\"candidate\":{\"matched\":false,\"dispatched\":false}") != std::string::npos);

    PsxXgRenderAuthCandidate mismatched_candidate = matching_candidate();
    mismatched_candidate.producer_entry += 4u;
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_note_candidate_dispatch(&mismatched_candidate);
    complete_warm_runtime_trace();
    assert_original_render_mode();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string mismatched_source = read_file(evidence_path);
    assert(mismatched_source.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(mismatched_source.find("\"candidate\":{\"matched\":false,\"dispatched\":false}") != std::string::npos);

    PsxXgRenderAuthCandidate invalid_range = matching_candidate();
    invalid_range.range_size = 8u;
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_note_candidate_dispatch(&invalid_range);
    complete_warm_runtime_trace();
    assert_original_render_mode();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string invalid_candidate = read_file(evidence_path);
    assert(invalid_candidate.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(invalid_candidate.find("\"candidate\":{\"matched\":false,\"dispatched\":false}") != std::string::npos);

    PsxXgRenderAuthCandidate unbound_candidate = matching_candidate();
    unbound_candidate.pair_bound = false;
    psx_xg_render_auth_scene_boundary();
    psx_xg_render_auth_note_candidate_dispatch(&unbound_candidate);
    complete_warm_runtime_trace();
    assert_original_render_mode();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string unbound_source = read_file(evidence_path);
    assert(unbound_source.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(unbound_source.find("\"candidate\":{\"matched\":false,\"dispatched\":false}") != std::string::npos);

    psx_xg_render_auth_scene_boundary();
    complete_warm_runtime_trace();
    assert_original_render_mode();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string no_dispatch = read_file(evidence_path);
    assert(no_dispatch.find("\"status\":\"BLOCKED\"") != std::string::npos);
    assert(no_dispatch.find("\"candidate\":{\"matched\":false,\"dispatched\":false}") != std::string::npos);

    psx_xg_render_auth_scene_boundary();
    input_replay::note_snapshot({1u, 0u, UINT32_MAX, 0u, 4u, 4u, 0u, true});
    complete_cold_runtime_trace();
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string post_checkpoint_context = read_file(evidence_path);
    assert(post_checkpoint_context.find("\"status\":\"OBSERVED\"") != std::string::npos);
    assert(post_checkpoint_context.find(
        "\"field_binding\":{\"checkpoint_field_id\":5,\"checkpoint_seen\":true") !=
        std::string::npos);
    assert(post_checkpoint_context.find("\"context_field_id\":5}") !=
           std::string::npos);
    assert(post_checkpoint_context.find("\"tuple\":") != std::string::npos);
    assert(post_checkpoint_context.find("\"trace\":") != std::string::npos);

    guest_render_bridge_test_reset();
    const GuestRenderSceneConfig native_scene = {
        GUEST_RENDER_TIMING_NATIVE_59_94,
        GUEST_RENDER_RENDER_NATIVE,
    };
    assert(guest_render_bridge_begin_scene(&native_scene) == GUEST_RENDER_OK);
    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_BACKEND_FAILURE);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string fallback_scene = read_file(evidence_path);
    assert(fallback_scene.find("\"requested_timing_mode\":\"native_59_94\"") !=
           std::string::npos);
    assert(fallback_scene.find("\"effective_timing_mode\":\"native_59_94\"") !=
           std::string::npos);
    assert(fallback_scene.find("\"requested_render_mode\":\"native\"") !=
           std::string::npos);
    assert(fallback_scene.find("\"effective_render_mode\":\"original\"") !=
           std::string::npos);
    assert(fallback_scene.find("\"cumulative_fallback_count\":1") !=
           std::string::npos);
    assert(fallback_scene.find("\"scene_fallback_count_baseline\":0") !=
           std::string::npos);
    assert(fallback_scene.find("\"scene_fallback_count_delta\":1") !=
           std::string::npos);
    assert(fallback_scene.find(
        "\"scene_fallback_reason\":\"backend_failure\"") != std::string::npos);
    assert(fallback_scene.find(
        "\"last_fallback_reason\":\"backend_failure\"") != std::string::npos);

    assert(guest_render_bridge_begin_scene(&native_scene) == GUEST_RENDER_OK);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string next_clean_scene = read_file(evidence_path);
    assert(next_clean_scene.find("\"effective_render_mode\":\"native\"") !=
           std::string::npos);
    assert(next_clean_scene.find("\"cumulative_fallback_count\":1") !=
           std::string::npos);
    assert(next_clean_scene.find("\"scene_fallback_count_baseline\":1") !=
           std::string::npos);
    assert(next_clean_scene.find("\"scene_fallback_count_delta\":0") !=
           std::string::npos);
    assert(next_clean_scene.find("\"scene_fallback_reason\":\"none\"") !=
           std::string::npos);
    assert(next_clean_scene.find(
        "\"last_fallback_reason\":\"backend_failure\"") != std::string::npos);
    assert(next_clean_scene.find("\"fallback_reason\":0") != std::string::npos);
    assert(next_clean_scene.find("\"fallback_count\":0") != std::string::npos);
    assert(next_clean_scene.find(
        "\"zoom_template_contract\":{\"generation\":0,"
        "\"producer_store_pc\":0,\"template_count\":5,\"buffer_count\":2,"
        "\"opcode\":0,\"authenticated\":false,\"initializer_begins\":0,"
        "\"initializer_commits\":0,\"initializer_2e\":0,\"rgb_updates\":0,"
        "\"invocations\":0,\"cutover_attempts\":0,\"native_invocations\":0,"
        "\"native_primitives\":0,\"replay_invocations\":0,"
        "\"replay_primitives\":0,\"rejections\":0,"
        "\"last_rejection_blocker\":0}") != std::string::npos);
    assert(next_clean_scene.find(
        "\"model_ft4\":{\"dispatch_begins\":0,"
        "\"dispatch_caller_rejects\":0,\"dispatch_mode_rejects\":0,"
        "\"average_seams\":0,\"farthest_seams\":0,"
        "\"seams_without_context\":0,\"invocations\":0,"
        "\"native_cutovers\":0,"
        "\"native_primitives\":0") != std::string::npos);
    assert(next_clean_scene.find(
        "\"sprite_ft4\":{\"callers\":0,\"native_cutovers\":0,"
        "\"native_primitives\":0") != std::string::npos);
    assert(next_clean_scene.find(
        "\"world_terrain_water\":{\"native_cutovers\":0,"
        "\"native_primitives\":0") != std::string::npos);
    assert(next_clean_scene.find(
        "\"world_entity_shadows\":{\"native_cutovers\":0,"
        "\"native_primitives\":0") != std::string::npos);
    assert(next_clean_scene.find(
        "\"world_decorations\":{\"native_cutovers\":0,"
        "\"native_primitives\":0") != std::string::npos);

    guest_render_bridge_test_set_fallback_count_limit(1u);
    guest_render_bridge_force_original(GUEST_RENDER_FALLBACK_BACKEND_FAILURE);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    const std::string overflowed_fallback = read_file(evidence_path);
    assert(overflowed_fallback.find("\"status\":\"FAIL\"") != std::string::npos);
    assert(overflowed_fallback.find("\"cumulative_fallback_count\":1") !=
           std::string::npos);
    assert(overflowed_fallback.find("\"scene_fallback_count_baseline\":1") !=
           std::string::npos);
    assert(overflowed_fallback.find("\"scene_fallback_count_delta\":0") !=
           std::string::npos);
    assert(overflowed_fallback.find(
        "\"scene_fallback_reason\":\"counter_exhausted\"") !=
        std::string::npos);
    assert(overflowed_fallback.find(
        "\"last_fallback_reason\":\"counter_exhausted\"") !=
        std::string::npos);
    assert(overflowed_fallback.find("\"fallback_count_overflowed\":true") !=
           std::string::npos);

    input_replay::detach(players);
    SDL_Quit();
    std::remove(trace_path.c_str());
    std::remove(evidence_path.c_str());
    return 0;
}
