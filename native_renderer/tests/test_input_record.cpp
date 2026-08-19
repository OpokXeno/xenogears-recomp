#include "input_replay.h"

#include <SDL.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

input_replay::HostInputSnapshot controller_snapshot() {
    input_replay::HostInputSnapshot snapshot{};
    snapshot.pads[0].connected = true;
    snapshot.pads[0].mode = input_replay::PadMode::Analog;
    snapshot.pads[0].buttons[SDL_CONTROLLER_BUTTON_A] = SDL_PRESSED;
    snapshot.pads[0].axes[SDL_CONTROLLER_AXIS_LEFTX] = -12345;
    snapshot.pads[0].axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 32767;
    snapshot.pads[1].connected = true;
    snapshot.pads[1].mode = input_replay::PadMode::Digital;
    snapshot.pads[1].buttons[SDL_CONTROLLER_BUTTON_START] = SDL_PRESSED;
    snapshot.pads[1].axes[SDL_CONTROLLER_AXIS_RIGHTY] = 23456;
    return snapshot;
}

input_replay::Snapshot stable_field_five() {
    return {0x80119728u, 0u, UINT32_MAX, 0u, 5u, 5u, 7u, true};
}

input_replay::Snapshot transient_active_field_five() {
    auto snapshot = stable_field_five();
    snapshot.active_module = 1u;
    return snapshot;
}

input_replay::LoaderState cold_loader() {
    return {1, 0, 1, 0, 0, 0, 1, 0, 1, 0};
}

input_replay::LoaderState reading_loader() {
    auto loader = cold_loader();
    loader.cd_reading = 1;
    return loader;
}

struct RecordCase {
    std::string trace_path;
    input_replay::Snapshot snapshot;
    input_replay::LoaderState loader;
    bool expect_complete;
};

void run_record_case(const RecordCase& record_case) {
    std::string error;
    std::remove(record_case.trace_path.c_str());
    std::remove((record_case.trace_path + ".incomplete").c_str());
    assert(input_replay::record_begin(record_case.trace_path.c_str(), 5u, 16u, &error));
    for (int index = 0; index < 3; ++index) {
        input_replay::record_note_scene(record_case.snapshot, record_case.loader);
        input_replay::record_note_guest_vblank();
        assert(input_replay::record_snapshot(controller_snapshot(), &error));
        assert(!input_replay::record_complete());
    }
    input_replay::record_note_scene(record_case.snapshot, record_case.loader);
    input_replay::record_note_guest_vblank();
    assert(input_replay::record_snapshot(controller_snapshot(), &error));
    if (record_case.expect_complete) {
        assert(input_replay::record_complete());
        assert(!input_replay::recording());
        assert(!input_replay::active());
    } else {
        assert(!input_replay::record_complete());
        input_replay::record_abort();
    }
    std::remove(record_case.trace_path.c_str());
    std::remove((record_case.trace_path + ".incomplete").c_str());
}

void test_cold_field_five_completion_gate() {
    run_record_case({"input_record_cold_field_five.toml", stable_field_five(), cold_loader(), true});
    run_record_case({"input_record_transient_active_field_five.toml", transient_active_field_five(),
                     cold_loader(), false});
    run_record_case({"input_record_reading_field_five.toml", stable_field_five(), reading_loader(), false});
}

void test_record_replays_one_snapshot_per_guest_vblank() {
    const std::string trace_path = "input_record_test.toml";
    std::string error;
    assert(input_replay::record_begin(trace_path.c_str(), 5u, 16u, &error));
    input_replay::record_note_guest_vblank();
    assert(input_replay::record_snapshot(controller_snapshot(), &error));
    assert(!input_replay::record_snapshot(controller_snapshot(), &error));
    for (int index = 0; index < 4; ++index) {
        input_replay::record_note_scene(stable_field_five(), cold_loader());
        input_replay::record_note_guest_vblank();
        assert(input_replay::record_snapshot(controller_snapshot(), &error));
    }
    assert(input_replay::record_complete());
    assert(!input_replay::recording());
    assert(!input_replay::active());

    std::ifstream trace(trace_path);
    const std::string text((std::istreambuf_iterator<char>(trace)), {});
    assert(text.find("schema = \"xenogears.native-render-replay/v3\"") != std::string::npos);
    assert(text.find("complete = true") != std::string::npos);
    assert(text.find("repeat = 5") != std::string::npos);
    assert(text.find("p1_left_x = -12345") != std::string::npos);
    assert(text.find("p1_trigger_right = 32767") != std::string::npos);

    assert(input_replay::load(trace_path.c_str(), &error));
    assert(!input_replay::record_begin("record-replay-exclusion.toml", 5u, 16u, &error));
    SDL_GameController* players[2] = {nullptr, nullptr};
    assert(input_replay::attach(players, &error));
    assert(players[1] != nullptr);
    assert(input_replay::latch_vblank());
    assert(SDL_GameControllerGetButton(players[0], SDL_CONTROLLER_BUTTON_A) == SDL_PRESSED);
    assert(SDL_GameControllerGetAxis(players[0], SDL_CONTROLLER_AXIS_LEFTX) == -12345);
    assert(SDL_GameControllerGetAxis(players[0], SDL_CONTROLLER_AXIS_TRIGGERRIGHT) == 32767);
    assert(SDL_GameControllerGetButton(players[1], SDL_CONTROLLER_BUTTON_START) == SDL_PRESSED);
    assert(SDL_GameControllerGetAxis(players[1], SDL_CONTROLLER_AXIS_RIGHTY) == 23456);
    bool p2_connected = false;
    input_replay::PadMode p2_mode = input_replay::PadMode::Hybrid;
    assert(input_replay::current_pad_config(1, &p2_connected, &p2_mode));
    assert(p2_connected && p2_mode == input_replay::PadMode::Digital);
    input_replay::detach(players);
    std::remove(trace_path.c_str());
}

void test_incomplete_and_interrupted_records_are_rejected() {
    const std::string trace_path = "input_record_interrupted.toml";
    std::string error;
    assert(input_replay::record_begin(trace_path.c_str(), 5u, 1u, &error));
    input_replay::record_note_guest_vblank();
    assert(input_replay::record_snapshot(controller_snapshot(), &error));
    input_replay::record_abort();
    assert(!input_replay::recording());
    assert(std::ifstream(trace_path + ".incomplete").good());
    assert(!input_replay::load((trace_path + ".incomplete").c_str(), &error));
    std::remove((trace_path + ".incomplete").c_str());
    {
        std::ofstream malformed(trace_path);
        malformed << "schema = \"xenogears.native-render-replay/v2\"\n"
                  << "complete = true\nvblank_budget = 1\nrecord_stop_field = 5\n"
                  << "record_stable_vblanks = 4\n[checkpoint]\nkind = \"u16\"\n"
                  << "address = \"0x8006F94E\"\nequals = 5\n[[vblank]]\nrepeat = 1\n"
                  << "p1_connected = true\np1_mode = \"analog\"\np1_buttons = []\n"
                  << "p1_left_x = 0\np1_left_y = 0\np1_right_x = 0\np1_right_y = 0\n";
    }
    assert(!input_replay::load(trace_path.c_str(), &error));
    std::remove(trace_path.c_str());
}

void test_close_record_publishes_only_on_clean_close() {
    const std::string trace_path = "input_record_close.toml";
    std::string error;
    std::remove(trace_path.c_str());
    std::remove((trace_path + ".incomplete").c_str());
    assert(input_replay::record_begin_until_close(
        trace_path.c_str(), 16u, &error));
    for (int index = 0; index < 5; ++index) {
        input_replay::record_note_scene(stable_field_five(), cold_loader());
        input_replay::record_note_guest_vblank();
        assert(input_replay::record_snapshot(controller_snapshot(), &error));
    }
    assert(input_replay::recording());
    assert(!input_replay::record_complete());
    assert(input_replay::record_close(&error));
    assert(input_replay::record_complete());
    assert(!input_replay::recording());
    assert(std::ifstream(trace_path).good());
    assert(!std::ifstream(trace_path + ".incomplete").good());
    std::ifstream trace(trace_path);
    const std::string text((std::istreambuf_iterator<char>(trace)), {});
    assert(text.find("complete = true") != std::string::npos);
    assert(text.find("record_on_close = true") != std::string::npos);
    assert(text.find("record_stop_field") == std::string::npos);
    assert(text.find("[checkpoint]") == std::string::npos);
    std::remove(trace_path.c_str());
}

}

int main() {
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    test_incomplete_and_interrupted_records_are_rejected();
    test_cold_field_five_completion_gate();
    test_close_record_publishes_only_on_clean_close();
    test_record_replays_one_snapshot_per_guest_vblank();
    SDL_Quit();
    return 0;
}
