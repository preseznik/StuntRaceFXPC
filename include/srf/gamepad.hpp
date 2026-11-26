#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace srf {
struct GamepadInput {
    std::uint16_t requested=0;
    bool pause=false,restart=false,disconnected=false,recenter=false;
    // Present only for an armed proportional stick: -32767 left, +32767 right.
    std::optional<std::int16_t> steering_q15;
    float look_x=0,look_y=0;
};
// Keyboard steering takes priority over the stick and uses original full input.
GamepadInput combine_player_input(std::uint16_t keyboard,GamepadInput gamepad);
enum class MenuButton { none, up, down, left, right, accept, back };
struct CourseMapButtons { bool start{}, accept{}, left{}, right{}; };
// Presentation/input service. Digital mode preserves the original wishes;
// proportional mode also supplies the native vehicle's steering target.
class Gamepad {
public:
    explicit Gamepad(const std::filesystem::path& config={},std::optional<unsigned> player={});
    ~Gamepad();
    Gamepad(const Gamepad&)=delete;
    Gamepad& operator=(const Gamepad&)=delete;
    GamepadInput poll(bool focused,bool camera_look=false);
    MenuButton poll_menu(bool focused);
    std::uint16_t poll_frontend(bool focused); // Held buttons in SNES controller bit order.
    CourseMapButtons poll_course_map(bool focused);
    std::string status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
