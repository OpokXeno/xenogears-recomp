#include "host_input_mapping.h"
#include "input_replay.h"
#include "psx_keybinds.h"
#include "sio.h"

#include <SDL.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
uint32_t i_stat = 0;
uint32_t g_debug_current_func_addr = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t) { return 0; }
uint32_t memory_get_sr(void) { return 0; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t, uint32_t, uint8_t) {}
void starvation_ring_record(uint8_t, uint8_t, uint8_t, uint16_t, uint16_t, uint8_t,
                            uint8_t, uint16_t, uint8_t, uint16_t, uint8_t, uint32_t) {}
void card_read_summary_record(uint8_t, uint8_t, uint16_t, uint8_t, uint8_t, uint8_t,
                              uint8_t, uint32_t) {}
void card_data_writes_arm(uint8_t, uint16_t, uint8_t, uint8_t) {}
void event_ring_record_aux(uint16_t, uint8_t, uint32_t) {}
void psx_irq_raise(uint32_t bit, uint32_t) { i_stat |= 1u << bit; }
}

namespace {

constexpr uint16_t kCross = 1u << 14;
constexpr uint16_t kL2 = 1u << 8;
constexpr uint16_t kR2 = 1u << 9;
constexpr uint16_t kStart = 1u << 3;

host_input::ControllerMap map() {
    using K = host_input::ControllerSource::Kind;
    return {{
        {1u << 4, "up", {{K::Button, SDL_CONTROLLER_BUTTON_DPAD_UP}}},
        {1u << 6, "down", {{K::Button, SDL_CONTROLLER_BUTTON_DPAD_DOWN}}},
        {1u << 7, "left", {{K::Button, SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {K::AxisNegative, SDL_CONTROLLER_AXIS_LEFTX}}},
        {1u << 5, "right", {{K::Button, SDL_CONTROLLER_BUTTON_DPAD_RIGHT}, {K::AxisPositive, SDL_CONTROLLER_AXIS_LEFTX}}},
        {kCross, "cross", {{K::Button, SDL_CONTROLLER_BUTTON_A}}}, {1u << 13, "circle", {{K::Button, SDL_CONTROLLER_BUTTON_B}}},
        {1u << 15, "square", {{K::Button, SDL_CONTROLLER_BUTTON_X}}}, {1u << 12, "triangle", {{K::Button, SDL_CONTROLLER_BUTTON_Y}}},
        {1u << 10, "l1", {{K::Button, SDL_CONTROLLER_BUTTON_LEFTSHOULDER}}}, {1u << 11, "r1", {{K::Button, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}}},
        {kL2, "l2", {{K::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERLEFT}}}, {kR2, "r2", {{K::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERRIGHT}}},
        {1u << 1, "l3", {{K::Button, SDL_CONTROLLER_BUTTON_LEFTSTICK}}}, {1u << 2, "r3", {{K::Button, SDL_CONTROLLER_BUTTON_RIGHTSTICK}}},
        {kStart, "start", {{K::Button, SDL_CONTROLLER_BUTTON_START}}}, {1u, "select", {{K::Button, SDL_CONTROLLER_BUTTON_BACK}}},
    }};
}

std::array<PsxNetPad, 2> capture(const host_input::HostInputSnapshot& snapshot,
                                 std::array<host_input::PlayerRoute, 2>* routes,
                                 bool dev_p1) {
    const host_input::ControllerMap bindings = map();
    const host_input::MappingOptions options{bindings, 12000, false, dev_p1};
    std::array<PsxNetPad, 2> pads{};
    for (int slot = 0; slot < 2; ++slot)
        host_input::capture_pad_slot(snapshot, slot, &(*routes)[slot], options, &pads[slot]);
    return pads;
}

std::array<uint8_t, 8> sio_response(int slot, const PsxNetPad& pad) {
    sio_set_pad_connected(slot, pad.connected);
    sio_set_pad_state_slot(slot, pad.buttons);
    sio_set_pad_sticks(slot, pad.lx, pad.ly, pad.rx, pad.ry);
    sio_request_pad_type(slot, pad.analog);
    const uint16_t ctrl = 1u | 2u | 0x1000u | (slot ? 0x2000u : 0u);
    const auto exchange = [ctrl](uint8_t tx) {
        sio_write(0x1F80104Au, ctrl);
        sio_write(0x1F801040u, tx);
        sio_tick(2000);
        return static_cast<uint8_t>(sio_read(0x1F801040u));
    };
    std::array<uint8_t, 8> bytes{};
    exchange(0x01u);
    bytes[0] = exchange(0x42u);
    for (size_t index = 1; index < bytes.size(); ++index) bytes[index] = exchange(0u);
    return bytes;
}

void assert_same(const std::array<PsxNetPad, 2>& left, const std::array<PsxNetPad, 2>& right) {
    assert(left[0].buttons == right[0].buttons);
    assert(left[1].buttons == right[1].buttons);
    for (size_t slot = 0; slot < left.size(); ++slot) {
        assert(left[slot].lx == right[slot].lx && left[slot].ly == right[slot].ly);
        assert(left[slot].rx == right[slot].rx && left[slot].ry == right[slot].ry);
        assert(left[slot].analog == right[slot].analog && left[slot].connected == right[slot].connected);
    }
}

void record_and_replay(const std::string& path, const host_input::HostInputSnapshot& snapshot,
                       const std::array<PsxNetPad, 2>& expected, bool dev_p1) {
    std::string error;
    std::remove(path.c_str());
    assert(input_replay::record_begin(path.c_str(), 5u, 16u, &error));
    for (int frame = 0; frame < 5; ++frame) {
        input_replay::record_note_guest_vblank();
        assert(input_replay::record_snapshot(snapshot, expected, &error));
        input_replay::record_note_scene({0x80119728u, 0u, UINT32_MAX, 0u, 5u, 5u, 7u, true},
                                        {1, 0, 1, 0, 0, 0, 1, 1, 1, 1});
    }
    assert(input_replay::record_complete());
    std::ifstream trace(path);
    const std::string text((std::istreambuf_iterator<char>(trace)), {});
    assert(text.find("repeat = 5") != std::string::npos);
    assert(input_replay::load(path.c_str(), &error));
    SDL_GameController* players[2] = {nullptr, nullptr};
    assert(input_replay::attach(players, &error));
    assert(input_replay::latch_vblank());
    host_input::HostInputSnapshot replayed = host_input::HostInputSnapshot::capture();
    std::array<host_input::PlayerRoute, 2> routes{{
        {2, expected[0].analog ? 1 : 2, false, SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(players[0]))},
        {2, expected[1].analog ? 1 : 2, false, SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(players[1]))},
    }};
    const auto actual = capture(replayed, &routes, dev_p1);
    assert_same(expected, actual);
    sio_init();
    for (int slot = 0; slot < 2; ++slot)
        assert(sio_response(slot, expected[slot]) == sio_response(slot, actual[slot]));
    input_replay::detach(players);
    std::remove(path.c_str());
}

void test_trigger_and_dev_merge_parity() {
    psx_keybinds_reset_player(1);
    psx_keybinds_set_button(1, PSX_KB_CROSS, SDL_SCANCODE_Z);
    psx_keybinds_set_button(1, PSX_KB_L2, SDL_SCANCODE_W);
    host_input::ControllerSnapshot p1{};
    p1.instance = 10;
    p1.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 32767;
    p1.axes[SDL_CONTROLLER_AXIS_LEFTX] = -20000;
    host_input::ControllerSnapshot p2{};
    p2.instance = 20;
    p2.buttons[SDL_CONTROLLER_BUTTON_START] = SDL_PRESSED;
    std::array<uint8_t, SDL_NUM_SCANCODES> keys{};
    keys[SDL_SCANCODE_Z] = keys[SDL_SCANCODE_W] = SDL_PRESSED;
    const host_input::HostInputSnapshot snapshot{keys, {p1, p2}};
    std::array<host_input::PlayerRoute, 2> routes{{{0, 0, true, -1}, {2, 1, false, 20}}};
    const auto expected = capture(snapshot, &routes, true);
    assert((expected[0].buttons & (kCross | kL2 | kR2)) == 0u);
    assert(expected[0].analog && expected[0].lx < 0x80u);
    assert((expected[1].buttons & kStart) == 0u && expected[1].analog);
    for (int sample = 0; sample < 2; ++sample) {
        const auto repeated = capture(snapshot, &routes, true);
        assert_same(expected, repeated);
    }
    record_and_replay("input_parity_test.toml", snapshot, expected, true);
}

}

int main() {
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    test_trigger_and_dev_merge_parity();
    SDL_Quit();
    return 0;
}
