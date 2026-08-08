#include "host_input_mapping.h"
#include "psx_keybinds.h"

#include <SDL.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {

constexpr uint16_t kUp = 1u << 4;
constexpr uint16_t kLeft = 1u << 7;
constexpr uint16_t kL2 = 1u << 8;
constexpr uint16_t kR2 = 1u << 9;
constexpr uint16_t kCross = 1u << 14;

host_input::ControllerMap default_map() {
    using Kind = host_input::ControllerSource::Kind;
    return {{
        {kUp, "up", {{Kind::Button, SDL_CONTROLLER_BUTTON_DPAD_UP}}},
        {1u << 6, "down", {{Kind::Button, SDL_CONTROLLER_BUTTON_DPAD_DOWN}}},
        {kLeft, "left", {{Kind::Button, SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {Kind::AxisNegative, SDL_CONTROLLER_AXIS_LEFTX}}},
        {1u << 5, "right", {{Kind::Button, SDL_CONTROLLER_BUTTON_DPAD_RIGHT}, {Kind::AxisPositive, SDL_CONTROLLER_AXIS_LEFTX}}},
        {kCross, "cross", {{Kind::Button, SDL_CONTROLLER_BUTTON_A}}},
        {1u << 13, "circle", {{Kind::Button, SDL_CONTROLLER_BUTTON_B}}},
        {1u << 15, "square", {{Kind::Button, SDL_CONTROLLER_BUTTON_X}}},
        {1u << 12, "triangle", {{Kind::Button, SDL_CONTROLLER_BUTTON_Y}}},
        {1u << 10, "l1", {{Kind::Button, SDL_CONTROLLER_BUTTON_LEFTSHOULDER}}},
        {1u << 11, "r1", {{Kind::Button, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}}},
        {kL2, "l2", {{Kind::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERLEFT}}},
        {kR2, "r2", {{Kind::AxisPositive, SDL_CONTROLLER_AXIS_TRIGGERRIGHT}}},
        {1u << 1, "l3", {{Kind::Button, SDL_CONTROLLER_BUTTON_LEFTSTICK}}},
        {1u << 2, "r3", {{Kind::Button, SDL_CONTROLLER_BUTTON_RIGHTSTICK}}},
        {1u << 3, "start", {{Kind::Button, SDL_CONTROLLER_BUTTON_START}}},
        {1u << 0, "select", {{Kind::Button, SDL_CONTROLLER_BUTTON_BACK}}},
    }};
}

host_input::HostInputSnapshot snapshot_with(
        std::initializer_list<SDL_Scancode> keys,
        std::vector<host_input::ControllerSnapshot> controllers) {
    std::array<uint8_t, SDL_NUM_SCANCODES> keyboard{};
    for (const SDL_Scancode key : keys) keyboard[static_cast<size_t>(key)] = SDL_PRESSED;
    return {keyboard, std::move(controllers)};
}

host_input::ControllerSnapshot controller(SDL_JoystickID instance) {
    host_input::ControllerSnapshot snapshot{};
    snapshot.instance = instance;
    return snapshot;
}

PsxNetPad capture(const host_input::HostInputSnapshot& snapshot, int slot,
                  host_input::PlayerRoute* player, bool dev_p1 = false) {
    const host_input::ControllerMap map = default_map();
    const host_input::MappingOptions options{map, 12000, false, dev_p1};
    PsxNetPad pad{};
    assert(host_input::capture_pad_slot(snapshot, slot, player, options, &pad));
    return pad;
}

void configure_keyboard() {
    psx_keybinds_reset_player(1);
    psx_keybinds_set_button(1, PSX_KB_UP, SDL_SCANCODE_UP);
    psx_keybinds_set_button(1, PSX_KB_CROSS, SDL_SCANCODE_Z);
    psx_keybinds_set_button(1, PSX_KB_L2, SDL_SCANCODE_W);
    psx_keybinds_set_button(1, PSX_KB_R2, SDL_SCANCODE_R);
}

void test_keyboard_buttons_include_triggers() {
    configure_keyboard();
    host_input::PlayerRoute player{1, 2, false, -1};
    const PsxNetPad pad = capture(snapshot_with({SDL_SCANCODE_UP, SDL_SCANCODE_Z,
                                                  SDL_SCANCODE_W, SDL_SCANCODE_R}, {}),
                                  0, &player);
    assert((pad.buttons & (kUp | kCross | kL2 | kR2)) == 0u);
    assert(pad.analog == 0u);
}

void test_controller_and_selected_p2_are_raw_slot_specific() {
    auto first = controller(10);
    first.buttons[SDL_CONTROLLER_BUTTON_A] = SDL_PRESSED;
    auto second = controller(20);
    second.buttons[SDL_CONTROLLER_BUTTON_B] = SDL_PRESSED;
    second.axes[SDL_CONTROLLER_AXIS_LEFTX] = -32768;
    second.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 32767;
    const auto snapshot = snapshot_with({}, {first, second});
    host_input::PlayerRoute player{2, 2, false, 20};
    const PsxNetPad pad = capture(snapshot, 1, &player);
    assert((pad.buttons & kCross) != 0u);
    assert((pad.buttons & (kLeft | kR2)) == 0u);
}

void test_dev_p1_merges_keyboard_and_every_controller() {
    configure_keyboard();
    auto first = controller(10);
    first.buttons[SDL_CONTROLLER_BUTTON_A] = SDL_PRESSED;
    auto second = controller(20);
    second.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 32767;
    host_input::PlayerRoute player{0, 1, true, -1};
    const PsxNetPad pad = capture(snapshot_with({SDL_SCANCODE_W}, {first, second}), 0, &player, true);
    assert((pad.buttons & (kCross | kL2 | kR2)) == 0u);
    assert(pad.analog == 1u);
}

void test_digital_analog_and_hybrid_modes() {
    auto source = controller(10);
    source.axes[SDL_CONTROLLER_AXIS_LEFTX] = -32768;
    const auto stick = snapshot_with({}, {source});
    host_input::PlayerRoute digital{2, 2, true, 10};
    const PsxNetPad digital_pad = capture(stick, 0, &digital);
    assert(digital_pad.analog == 0u && (digital_pad.buttons & kLeft) == 0u);
    assert(digital_pad.lx == 0x80u);

    host_input::PlayerRoute analog{2, 1, false, 10};
    const PsxNetPad analog_pad = capture(stick, 0, &analog);
    assert(analog_pad.analog == 1u && (analog_pad.buttons & kLeft) != 0u);
    assert(analog_pad.lx == 0u);

    host_input::PlayerRoute hybrid{2, 0, false, 10};
    const PsxNetPad hybrid_stick = capture(stick, 0, &hybrid);
    assert(hybrid_stick.analog == 1u && hybrid.hybrid_analog);
    source.axes[SDL_CONTROLLER_AXIS_LEFTX] = 0;
    source.buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] = SDL_PRESSED;
    const PsxNetPad hybrid_dpad = capture(snapshot_with({}, {source}), 0, &hybrid);
    assert(hybrid_dpad.analog == 0u && !hybrid.hybrid_analog);
    assert((hybrid_dpad.buttons & kUp) == 0u);
}

void test_disconnected_slot_stays_released() {
    const host_input::ControllerMap map = default_map();
    const host_input::MappingOptions options{map, 12000, false, false};
    host_input::PlayerRoute player{};
    PsxNetPad pad{};
    assert(!host_input::capture_pad_slot(snapshot_with({}, {}), 1, &player, options, &pad));
    assert(!pad.connected && pad.buttons == 0xFFFFu);
}

}

int main() {
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    test_keyboard_buttons_include_triggers();
    test_controller_and_selected_p2_are_raw_slot_specific();
    test_dev_p1_merges_keyboard_and_every_controller();
    test_digital_analog_and_hybrid_modes();
    test_disconnected_slot_stays_released();
    SDL_Quit();
    return 0;
}
