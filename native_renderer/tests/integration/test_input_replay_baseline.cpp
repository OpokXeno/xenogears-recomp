#include "game_identity.h"
#include "input_replay.h"
#include "native_render_baseline.h"
#include "xg_native_render_baseline.h"
#include "xg_render_auth_runtime_diagnostics.h"

#include <SDL.h>

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

extern "C" bool psx_xg_render_auth_authenticated_producer_entry(
    uint32_t *out_producer_entry) {
    if (out_producer_entry == nullptr) return false;
    *out_producer_entry = UINT32_C(0x800764b4);
    return true;
}

namespace {
std::array<uint8_t, 2u * 1024u * 1024u> ram{};
PsxGameIdentity runtime_identity{};
uint64_t runtime_observations = 0;
bool runtime_unsupported_display = false;
GuestRenderBridgeSnapshot runtime_bridge{};
GuestRenderCompletedState runtime_completed{};

void write_u32_le(uint32_t offset, uint32_t value) {
    ram[offset] = static_cast<uint8_t>(value);
    ram[offset + 1u] = static_cast<uint8_t>(value >> 8u);
    ram[offset + 2u] = static_cast<uint8_t>(value >> 16u);
    ram[offset + 3u] = static_cast<uint8_t>(value >> 24u);
}

void seed_ram() {
    for (size_t index = 0; index < ram.size(); ++index)
        ram[index] = static_cast<uint8_t>(index * 13u + 7u);
    write_u32_le(0x000afb0cu, 1u);
    write_u32_le(0x000afb10u, 0x80100000u);
}

void write_v3_trace(const std::string& path, bool baseline, uint16_t field,
                    uint64_t budget) {
    std::ofstream trace(path);
    trace << "schema = \"xenogears.native-render-replay/v3\"\n"
          << "complete = true\n";
    if (baseline) trace << "baseline = true\n";
    trace << "vblank_budget = " << budget << "\n"
          << "record_stop_field = " << field << "\n"
          << "record_stable_vblanks = 4\n"
          << "[checkpoint]\nkind = \"u16\"\naddress = \"0x8006F94E\"\n"
          << "equals = " << field << "\n"
          << "[[vblank]]\nrepeat = " << budget << "\n";
    for (int slot = 1; slot <= 2; ++slot)
        trace << "p" << slot << "_connected = " << (slot == 1 ? "true" : "false") << "\n"
              << "p" << slot << "_mode = \"digital\"\n"
              << "p" << slot << "_buttons = []\n"
              << "p" << slot << "_left_x = 0\n"
              << "p" << slot << "_left_y = 0\n"
              << "p" << slot << "_right_x = 0\n"
              << "p" << slot << "_right_y = 0\n"
              << "p" << slot << "_trigger_left = 0\n"
              << "p" << slot << "_trigger_right = 0\n";
}

std::string digest(uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << value;
    return output.str();
}

std::string expected_baseline(const NativeRenderBaselineSnapshot& value,
                               bool requested) {
    std::ostringstream output;
    output << "{\"requested\":" << (requested ? "true" : "false")
           << ",\"schema_version\":" << value.schema_version
           << ",\"enabled\":" << (value.enabled ? "true" : "false")
           << ",\"complete\":" << (value.complete ? "true" : "false")
           << ",\"overflow\":" << (value.overflow ? "true" : "false")
           << ",\"invalid_ot\":" << (value.invalid_ot ? "true" : "false")
           << ",\"cyclic_ot\":" << (value.cyclic_ot ? "true" : "false")
           << ",\"reason\":" << static_cast<unsigned>(value.incomplete_reason)
           << ",\"field_completeness_mask\":" << value.field_completeness_mask
           << ",\"required_field_mask\":" << value.required_field_mask
           << ",\"visual_scene_epoch\":" << value.visual_state_id.scene_epoch
           << ",\"visual_state_sequence\":" << value.visual_state_id.state_sequence
           << ",\"requested_render_mode\":" << static_cast<unsigned>(value.requested_render_mode)
           << ",\"effective_render_mode\":" << static_cast<unsigned>(value.effective_render_mode)
           << ",\"fallback_reason\":" << static_cast<unsigned>(value.fallback_reason)
           << ",\"fallback_count\":" << value.fallback_count
           << ",\"producer_count\":" << value.producer_count
           << ",\"producer_binding_count\":" << value.producer_binding_count
           << ",\"interpreter_calls\":" << value.interpreter_calls
           << ",\"native_calls\":" << value.native_calls
           << ",\"gte_total_count\":" << value.gte_total_count
           << ",\"gte_inside_producer_count\":" << value.gte_inside_producer_count
           << ",\"gte_outside_producer_count\":" << value.gte_outside_producer_count
           << ",\"gte_tier_counts\":[" << value.gte_tier_counts[0]
           << "," << value.gte_tier_counts[1]
           << "," << value.gte_tier_counts[2]
           << "," << value.gte_tier_counts[3] << "]"
           << ",\"gte_overflow_reason\":" << static_cast<unsigned>(value.gte_overflow_reason)
           << ",\"gte_blocked\":" << (value.gte_blocked ? "true" : "false")
           << ",\"ot_lists\":" << value.ot_lists
           << ",\"ot_nodes\":" << value.ot_nodes
           << ",\"ot_words\":" << value.ot_words
           << ",\"ot_digest\":\"" << digest(value.ot_digest) << "\""
           << ",\"topology_digest\":\"" << digest(value.topology_digest) << "\""
           << ",\"material_samples\":" << value.material_samples
           << ",\"material_digest\":\"" << digest(value.material_digest) << "\""
           << ",\"gp0_writes\":" << value.gp0_writes
           << ",\"gp1_writes\":" << value.gp1_writes
           << ",\"vram_mutations\":" << value.vram_mutations
           << ",\"global_vram_mutation_serial\":" << value.global_vram_mutation_serial
           << ",\"global_vram_serial_overflowed\":"
           << (value.global_vram_serial_overflowed ? "true" : "false")
           << ",\"vram_digest\":\"" << digest(value.vram_digest) << "\""
           << ",\"gpu_digest\":\"" << digest(value.gpu_digest) << "\""
           << ",\"display_samples\":" << value.display_samples
           << ",\"display15_digest\":\"" << digest(value.display15_digest) << "\""
           << ",\"display_digest\":\"" << digest(value.display_digest) << "\""
           << ",\"host_framebuffer_samples\":" << value.host_framebuffer_samples
           << ",\"host_framebuffer_digest\":\"" << digest(value.host_framebuffer_digest) << "\""
           << ",\"vblank_delta\":" << value.vblank_delta
           << ",\"guest_cycle_delta\":" << value.guest_cycle_delta
           << ",\"cycles_per_vblank\":" << value.cycles_per_vblank
           << ",\"cycle_digest\":\"" << digest(value.cycle_digest) << "\""
           << ",\"audio_frames\":" << value.audio_frames
           << ",\"audio_events\":" << value.audio_events
           << ",\"audio_digest\":\"" << digest(value.audio_digest) << "\""
           << ",\"game_digest\":\"" << digest(value.game_digest) << "\""
           << ",\"camera_actor_digest\":\"" << digest(value.camera_actor_digest) << "\""
           << ",\"normalized_digest\":\"" << digest(value.normalized_digest) << "\"}";
    return output.str();
}
}

extern "C" {
extern const uint8_t xg_render_game_identity[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
extern const uint8_t xg_render_manifest_identity[32] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
};

const PsxGameIdentity *psx_game_identity_runtime(void) {
    return &runtime_identity;
}

uint8_t *memory_get_ram_ptr(void) { return ram.data(); }
void native_render_baseline_runtime_reset(void) { runtime_observations = 0; }
void native_render_baseline_runtime_arm(void) { runtime_observations = 0; }
NativeRenderBaselineReason native_render_baseline_runtime_observe(
        NativeRenderBaselineSnapshot *snapshot) {
    ++runtime_observations;
    ++snapshot->vblank_delta;
    snapshot->guest_cycle_delta += 100u;
    ++snapshot->gp0_writes;
    ++snapshot->gp1_writes;
    ++snapshot->vram_mutations;
    snapshot->global_vram_mutation_serial = snapshot->vram_mutations;
    snapshot->vram_digest += 3u;
    snapshot->gpu_digest += 5u;
    ++snapshot->display_samples;
    snapshot->display15_digest += 7u;
    snapshot->display_digest += 11u;
    snapshot->audio_frames += 2u;
    ++snapshot->audio_events;
    if (runtime_unsupported_display)
        return NATIVE_RENDER_BASELINE_UNSUPPORTED_DISPLAY;
    snapshot->field_completeness_mask |=
        NATIVE_RENDER_BASELINE_FIELD_VRAM_SERIAL |
        NATIVE_RENDER_BASELINE_FIELD_VRAM_DIGEST |
        NATIVE_RENDER_BASELINE_FIELD_DISPLAY15_DIGEST;
    return NATIVE_RENDER_BASELINE_COMPLETE;
}

GuestRenderStatus guest_render_bridge_snapshot(GuestRenderBridgeSnapshot *out) {
    if (!out) return GUEST_RENDER_INVALID_ARGUMENT;
    *out = runtime_bridge;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_present(GuestRenderCompletedState *out) {
    if (!out) return GUEST_RENDER_INVALID_ARGUMENT;
    *out = runtime_completed;
    return GUEST_RENDER_OK;
}

GuestRenderStatus guest_render_bridge_last_completed(
        GuestRenderBridgeSnapshot *out_snapshot,
        GuestRenderCompletedState *out_completed) {
    if (!out_snapshot || !out_completed) return GUEST_RENDER_INVALID_ARGUMENT;
    *out_snapshot = runtime_bridge;
    *out_completed = runtime_completed;
    return GUEST_RENDER_OK;
}

GteAttributionResult gte_attribution_snapshot(
        GteAttributionSnapshot *out_snapshot,
        GteAttributionContextCounter *context_counters,
        size_t context_capacity,
        GteAttributionSiteCounter *site_counters,
        size_t site_capacity) {
    (void)context_counters;
    (void)context_capacity;
    (void)site_counters;
    (void)site_capacity;
    if (!out_snapshot) return GTE_ATTRIBUTION_INVALID_ARGUMENT;
    *out_snapshot = {};
    return GTE_ATTRIBUTION_OK;
}

void gte_attribution_set_enabled(bool enabled) { (void)enabled; }
void gte_attribution_reset(void) {}
}

int main() {
    const std::string task5 = "input_replay_baseline_task5.toml";
    const std::string other_field = "input_replay_baseline_other_field.toml";
    const std::string v2 = "input_replay_baseline_v2.toml";
    const std::string evidence_path = "input_replay_baseline_evidence.json";
    std::string error;
    NativeRenderBaselineConfig config{};
    NativeRenderBaselineSnapshot snapshot{};

    std::copy(std::begin(xg_render_game_identity), std::end(xg_render_game_identity),
              runtime_identity.game_sha256);
    std::copy(std::begin(xg_render_manifest_identity), std::end(xg_render_manifest_identity),
              runtime_identity.manifest_sha256);
    seed_ram();
    runtime_bridge.modes.requested_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    runtime_bridge.modes.effective_render_mode = GUEST_RENDER_RENDER_ORIGINAL;
    runtime_bridge.slot_count = 1u;
    runtime_bridge.binding_count = 1u;
    runtime_completed.id.scene_epoch = 1u;
    runtime_completed.id.state_sequence = 1u;
    runtime_completed.slot_count = 1u;
    runtime_completed.binding_count = 1u;

    write_v3_trace(other_field, true, 6u, 3508u);
    assert(input_replay::load(other_field.c_str(), &error));
    write_v3_trace(v2, true, 5u, 3508u);
    {
        std::fstream trace(v2, std::ios::in | std::ios::out);
        std::string text((std::istreambuf_iterator<char>(trace)), {});
        const size_t version = text.find("/v3");
        text.replace(version, 3u, "/v2");
        trace.seekp(0);
        trace << text;
    }
    assert(!input_replay::load(v2.c_str(), &error));

    write_v3_trace(task5, true, 5u, 4528u);
    runtime_identity.game_sha256[0] ^= 1u;
    assert(input_replay::load(task5.c_str(), &error));
    native_render_baseline_snapshot(&snapshot);
    assert(!snapshot.enabled);
    assert(snapshot.incomplete_reason == NATIVE_RENDER_BASELINE_DISABLED);
    input_replay::note_snapshot({1u, 0u, UINT32_MAX, 0u, 5u, 5u, 0u, true});
    native_render_baseline_snapshot(&snapshot);
    assert(snapshot.incomplete_reason == NATIVE_RENDER_BASELINE_INVALID_CONFIG);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    {
        std::ifstream mismatch_evidence(evidence_path);
        const std::string text((std::istreambuf_iterator<char>(mismatch_evidence)), {});
        assert(text.find("\"status\":\"FAIL\"") != std::string::npos);
        assert(text.find("\"requested\":true,\"schema_version\":2,\"enabled\":false") !=
               std::string::npos);
        assert(text.find("\"reason\":2") != std::string::npos);
    }
    runtime_identity.game_sha256[0] ^= 1u;

    assert(input_replay::load(task5.c_str(), &error));
    native_render_baseline_snapshot(&snapshot);
    assert(!snapshot.enabled);
    assert(snapshot.incomplete_reason == NATIVE_RENDER_BASELINE_DISABLED);
    input_replay::note_snapshot({1u, 0u, UINT32_MAX, 0u, 5u, 5u, 0u, false});
    native_render_baseline_snapshot(&snapshot);
    assert(!snapshot.enabled);
    native_render_baseline_observe_vblank();
    assert(runtime_observations == 0u);
    input_replay::note_snapshot({1u, 0u, UINT32_MAX, 0u, 5u, 5u, 0u, true});
    native_render_baseline_snapshot(&snapshot);
    assert(snapshot.enabled);
    assert(snapshot.camera_actor_digest != 0u);
    for (uint16_t progress = 0u; progress < 17u; ++progress) {
        input_replay::note_snapshot(
            {1u, 0u, UINT32_MAX, 0u, 5u, 5u, progress, true});
        input_replay::note_loader_state(
            {1, 0, 0, 0, 0u, 0u, static_cast<int>(progress & 1u), 0, 0, 0});
    }
    assert(!input_replay::finished());

    const XgNativeRenderBaselineResult configured =
        xg_native_render_baseline_configure(&runtime_identity, 4528u, &config);
    assert(configured.success);
    config.authenticated_producer_address = UINT32_C(0x800764b4);
    native_render_baseline_note_execution(config.authenticated_producer_address,
                                          NATIVE_RENDER_BASELINE_INTERPRETER);
    native_render_baseline_ot_begin(UINT32_C(0x1000));
    {
        const NativeRenderBaselineOtNode node = {
            UINT32_C(0x1000), UINT32_C(0x00ffffff), 3u, 0u,
        };
        native_render_baseline_ot_node(&node);
    }
    native_render_baseline_ot_end(NATIVE_RENDER_BASELINE_OT_VALID);
    {
        NativeRenderBaselineMaterialObservation observation{};
        observation.material.texture_depth = GPU_RENDER_TEXTURE_4_BIT;
        observation.material.blend_mode = GPU_RENDER_BLEND_AVERAGE;
        observation.material.shading = GPU_RENDER_SHADING_FLAT;
        observation.material.draw_area_right = 1023u;
        observation.material.draw_area_bottom = 511u;
        observation.provenance = NATIVE_RENDER_BASELINE_MATERIAL_OT;
        observation.command_address = UINT32_C(0x1004);
        observation.source_word_ordinal = UINT64_C(0x401);
        observation.container_ordinal = UINT64_C(0x400);
        observation.word_count = 3u;
        native_render_baseline_note_material(&observation);
    }
    native_render_baseline_note_host_framebuffer_digest(
        UINT64_C(0x1122334455667788));
    for (uint64_t index = 0; index < 600u; ++index) {
        input_replay::note_guest_vblank();
        native_render_baseline_observe_vblank();
    }
    assert(runtime_observations == 600u);
    input_replay::note_snapshot({1u, 0u, UINT32_MAX, 0u, 5u, 5u, 0u, true});
    input_replay::note_guest_vblank();
    native_render_baseline_observe_vblank();
    assert(runtime_observations == 600u);
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    native_render_baseline_snapshot(&snapshot);
    assert(snapshot.complete);

    std::ifstream evidence(evidence_path);
    const std::string evidence_text((std::istreambuf_iterator<char>(evidence)), {});
    const std::string baseline = expected_baseline(snapshot, true);
    assert(evidence_text.find("\"baseline\":" + baseline) !=
           std::string::npos);

    assert(input_replay::load(task5.c_str(), &error));
    input_replay::note_snapshot({1u, 0u, UINT32_MAX, 0u, 5u, 5u, 0u, true});
    runtime_unsupported_display = true;
    native_render_baseline_observe_vblank();
    runtime_unsupported_display = false;
    assert(input_replay::write_evidence(evidence_path.c_str(), 5u, "opengl"));
    native_render_baseline_snapshot(&snapshot);
    assert(!snapshot.complete);
    assert(snapshot.incomplete_reason == NATIVE_RENDER_BASELINE_UNSUPPORTED_DISPLAY);

    write_v3_trace(task5, false, 5u, 2u);
    assert(input_replay::load(task5.c_str(), &error));
    native_render_baseline_snapshot(&snapshot);
    assert(!snapshot.enabled);
    assert(snapshot.incomplete_reason == NATIVE_RENDER_BASELINE_DISABLED);

    std::remove(task5.c_str());
    std::remove(other_field.c_str());
    std::remove(v2.c_str());
    std::remove(evidence_path.c_str());
    SDL_Quit();
    return 0;
}
