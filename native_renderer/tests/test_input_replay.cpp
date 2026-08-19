#include "input_replay.h"

#include <SDL.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

int main() {
    const std::string trace_path = "input_replay_test.toml";
    {
        std::ofstream trace(trace_path);
        trace << "schema = \"xenogears.native-render-replay/v1\"\n";
        trace << "vblank_budget = 2\n";
        trace << "[checkpoint]\n";
        trace << "kind = \"u16\"\n";
        trace << "address = \"0x8006F94E\"\n";
        trace << "equals = 5\n";
        trace << "[[vblank]]\n";
        trace << "repeat = 1\n";
        trace << "p1_buttons = [\"a\"]\n";
        trace << "p1_left_x = -1\n";
        trace << "p2_buttons = [\"start\"]\n";
        trace << "[[vblank]]\n";
        trace << "repeat = 1\n";
    }
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    std::string error;
    assert(input_replay::load(trace_path.c_str(), &error));
    SDL_GameController* players[2] = {nullptr, nullptr};
    assert(input_replay::attach(players, &error));
    assert(players[0] != nullptr);
    assert(players[1] == nullptr);
    input_replay::note_guest_vblank();
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_A) == 1);
    assert(SDL_GameControllerGetAxis(players[0], SDL_CONTROLLER_AXIS_LEFTX) == -1);
    input_replay::note_capture(0);
    input_replay::note_mapping();
    input_replay::note_sio();
    input_replay::Snapshot snapshot{0x80119728u, 1u, 1u, 0u, 5u, 5u, 7u, true};
    input_replay::note_snapshot(snapshot);
    input_replay::SioReceipt receipt{1u, 0u, 0x41u, 0x5Au, 0xDFu, 0xFFu, false};
    input_replay::note_sio_receipt(receipt);
    input_replay::note_sio_receipt({2u, 0u, 0x41u, 0x5Au, 0xF7u, 0xFFu, false});
    input_replay::note_sio_receipt({3u, 0u, 0x41u, 0x5Au, 0xFFu, 0xBFu, false});
    input_replay::note_sio_receipt({4u, 0u, 0x41u, 0x5Au, 0xFFu, 0xBCu, false});
    input_replay::note_loader_state({1, 1, 2048, 1, 6, 28, 1, 42, 9, 1});
    input_replay::note_capture(0);
    input_replay::note_mapping();
    input_replay::note_sio();
    assert(input_replay::latch_vblank());
    assert(!input_replay::latch_vblank());
    const input_replay::Counters counters = input_replay::counters();
    assert(counters.vblank_latches == 2);
    assert(counters.guest_vblank_callbacks == 1);
    assert(counters.trace_state_latches == 2);
    assert(counters.provider_updates == 2);
    assert(counters.capture_samples == 2);
    assert(counters.mapping_reads == 2);
    assert(counters.sio_applies == 2);
    assert(input_replay::stop_reason() == input_replay::StopReason::CheckpointNotReached);
    assert(input_replay::write_evidence("input_replay_evidence.json", 0, "opengl"));
    std::ifstream evidence("input_replay_evidence.json");
    std::string evidence_text((std::istreambuf_iterator<char>(evidence)), {});
    assert(evidence_text.find("\"valid\":true") != std::string::npos);
    assert(evidence_text.find("\"id\":5") != std::string::npos);
    assert(evidence_text.find("\"id\":65") != std::string::npos);
    assert(evidence_text.find("\"start_count\":1") != std::string::npos);
    assert(evidence_text.find("\"cross_count\":2") != std::string::npos);
    assert(evidence_text.find("\"cross_first\":1") != std::string::npos);
    assert(evidence_text.find("\"loader\"") != std::string::npos);
    assert(evidence_text.find("\"cd\"") != std::string::npos);
    assert(evidence_text.find("\"registered\":42") != std::string::npos);
    assert(evidence_text.find("\"pending_cmd\":6") != std::string::npos);
    assert(evidence_text.find("\"transitions\"") != std::string::npos);
    assert(evidence_text.find("\"requested_timing_mode\":\"original\"") !=
           std::string::npos);
    assert(evidence_text.find("\"effective_timing_mode\":\"original\"") !=
           std::string::npos);
    assert(evidence_text.find("\"requested_render_mode\":\"original\"") !=
           std::string::npos);
    assert(evidence_text.find("\"effective_render_mode\":\"original\"") !=
           std::string::npos);
    assert(evidence_text.find("\"cumulative_fallback_count\":0") !=
           std::string::npos);
    assert(evidence_text.find("\"scene_fallback_count_baseline\":0") !=
           std::string::npos);
    assert(evidence_text.find("\"scene_fallback_count_delta\":0") !=
           std::string::npos);
    assert(evidence_text.find("\"scene_fallback_reason\":\"none\"") !=
           std::string::npos);
    assert(evidence_text.find("\"last_fallback_reason\":\"none\"") !=
           std::string::npos);
    assert(evidence_text.find("\"fallback_count_overflowed\":false") !=
           std::string::npos);
    assert(evidence_text.find("\"fallback_reason\":0") != std::string::npos);
    assert(evidence_text.find("\"fallback_count\":0") != std::string::npos);
    input_replay::detach(players);

    const std::string policy_path = "input_replay_policy_test.toml";
    {
        std::ofstream policy(policy_path);
        policy << "schema = \"xenogears.native-render-replay/v1\"\n";
        policy << "vblank_budget = 32\n";
        policy << "[checkpoint]\nkind = \"u16\"\naddress = \"0x8006F94E\"\nequals = 5\n";
        policy << "[[action]]\nmin_polls = 2\np1_buttons = [\"start\"]\n";
        policy << "[[action]]\nmin_polls = 3\n";
        policy << "[[action]]\nmin_polls = 2\np1_buttons = [\"a\"]\n";
        policy << "until_request = 1\n";
        policy << "[[action]]\nmin_polls = 1\n";
    }
    assert(input_replay::load(policy_path.c_str(), &error));
    assert(input_replay::attach(players, &error));
    input_replay::note_media_state({true, true, 7u});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_START) == 0);
    input_replay::note_media_state({false, false, 7u});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_START) == 1);
    input_replay::note_sio_receipt({1u, 0u, 0x41u, 0x5Au, 0xF7u, 0xFFu, false});
    input_replay::note_sio_receipt({2u, 0u, 0x41u, 0x5Au, 0xFFu, 0xFFu, false});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_START) == 1);
    input_replay::note_sio_receipt({3u, 0u, 0x41u, 0x5Au, 0xF7u, 0xFFu, false});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_START) == 0);
    input_replay::note_sio_receipt({4u, 0u, 0x41u, 0x5Au, 0xFFu, 0xFFu, false});
    input_replay::note_sio_receipt({5u, 0u, 0x41u, 0x5Au, 0xFFu, 0xFFu, false});
    input_replay::note_sio_receipt({6u, 0u, 0x41u, 0x5Au, 0xFFu, 0xFFu, true});
    input_replay::note_sio_receipt({7u, 0u, 0x41u, 0x5Au, 0xFFu, 0xFFu, false});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_A) == 1);
    input_replay::note_sio_receipt({8u, 0u, 0x41u, 0x5Au, 0xFFu, 0xBFu, false});
    input_replay::note_sio_receipt({9u, 0u, 0x41u, 0x5Au, 0xFFu, 0xBFu, false});
    input_replay::note_snapshot({0u, 2u, 0u, 0u, 0u, 0u, 0u, false});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_A) == 1);
    input_replay::note_snapshot({0u, 1u, 0u, 0u, 0u, 0u, 0u, false});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_A) == 0);
    input_replay::detach(players);

    const std::string lifecycle_path = "input_replay_lifecycle_test.toml";
    {
        std::ofstream policy(lifecycle_path);
        policy << "schema = \"xenogears.native-render-replay/v1\"\n";
        policy << "vblank_budget = 64\n";
        policy << "[checkpoint]\nkind = \"u16\"\naddress = \"0x8006F94E\"\nequals = 5\n";
        policy << "[[action]]\nmin_polls = 1\np1_buttons = [\"a\"]\nuntil_request = 1\n";
        policy << "[[action]]\nmin_polls = 1\n";
        policy << "[[action]]\nmin_polls = 1\np1_buttons = [\"a\"]\nafter_lifecycle = true\nuntil_change = true\n";
        policy << "[[action]]\nmin_polls = 1\n";
    }
    assert(input_replay::load(lifecycle_path.c_str(), &error));
    assert(input_replay::attach(players, &error));
    input_replay::note_media_state({true, true, 7u});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_A) == 1);
    input_replay::note_sio_receipt({1u, 0u, 0x41u, 0x5Au, 0xFFu, 0xBFu, false});
    input_replay::note_snapshot({0u, 1u, UINT32_MAX, 0u, 490u, 490u, 0u, false});
    input_replay::note_snapshot({0u, 1u, 1u, 0u, 490u, 490u, 0u, false});
    input_replay::note_snapshot({0u, 0u, UINT32_MAX, 0u, 490u, 490u, 0u, false});
    input_replay::note_snapshot({0u, 0u, UINT32_MAX, 0u, 490u, 490u, 0u, true});
    input_replay::note_sio_receipt({2u, 0u, 0x41u, 0x5Au, 0xFFu, 0xFFu, false});
    for (uint64_t poll = 3; poll < 15; ++poll)
        input_replay::note_sio_receipt({poll, 0u, 0x41u, 0x5Au, 0xFFu, 0xFFu, false});
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_A) == 1);
    input_replay::detach(players);

    const std::string close_path = "input_replay_close_test.toml";
    const std::string close_evidence_path = "input_replay_close_evidence.json";
    {
        std::ofstream close_trace(close_path);
        close_trace << "schema = \"xenogears.native-render-replay/v3\"\n"
                    << "complete = true\nvblank_budget = 2\nrecord_on_close = true\n";
        for (int index = 0; index < 2; ++index) {
            close_trace << "[[vblank]]\nrepeat = 1\n"
                        << "p1_connected = true\np1_mode = \"digital\"\np1_buttons = []\n"
                        << "p1_left_x = 0\np1_left_y = 0\np1_right_x = 0\np1_right_y = 0\n"
                        << "p1_trigger_left = 0\np1_trigger_right = 0\n"
                        << "p2_connected = false\np2_mode = \"digital\"\np2_buttons = []\n"
                        << "p2_left_x = 0\np2_left_y = 0\np2_right_x = 0\np2_right_y = 0\n"
                        << "p2_trigger_left = 0\np2_trigger_right = 0\n";
        }
    }
    assert(input_replay::load(close_path.c_str(), &error));
    uint32_t checkpoint_address = 0;
    uint16_t checkpoint_expected = 0;
    assert(!input_replay::checkpoint(&checkpoint_address, &checkpoint_expected));
    assert(input_replay::attach(players, &error));
    assert(input_replay::latch_vblank());
    assert(input_replay::latch_vblank());
    assert(!input_replay::latch_vblank());
    assert(input_replay::stop_reason() == input_replay::StopReason::TraceComplete);
    assert(input_replay::write_evidence(close_evidence_path.c_str(), 0u, "opengl"));
    std::ifstream close_evidence_file(close_evidence_path);
    const std::string close_evidence(
        (std::istreambuf_iterator<char>(close_evidence_file)), {});
    assert(close_evidence.find("\"status\":\"PASS\"") != std::string::npos);
    assert(close_evidence.find("\"completion\":\"trace_complete\"") !=
        std::string::npos);
    assert(close_evidence.find("\"checkpoint\":null") != std::string::npos);
    input_replay::detach(players);
    SDL_Quit();
    std::remove(trace_path.c_str());
    std::remove(policy_path.c_str());
    std::remove(lifecycle_path.c_str());
    std::remove(close_path.c_str());
    std::remove(close_evidence_path.c_str());
    std::remove("input_replay_evidence.json");
    return 0;
}
