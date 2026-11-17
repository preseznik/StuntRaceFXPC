#pragma once

#include "srf/race.hpp"
#include "srf/streaming.hpp"
#include "srf/vehicle.hpp"

#include <array>

namespace srf {
struct EnvironmentTrigger {
    std::uint32_t command_offset{},callback_address{};
    RacePosition position{};
    std::uint16_t extent{};
};
struct EnvironmentState {
    std::uint8_t flags{},second_player_flags{},palette_weight{};
    bool operator==(const EnvironmentState&) const = default;
};
struct EnvironmentEvents {
    std::vector<std::uint8_t> sound_commands,effects;
};
struct EnvironmentPresentation {
    std::uint16_t fixed_rgb555{};
    std::uint8_t palette_weight{},color_math_layers{};
    bool alternate_backdrop{},blank_backdrop{};
    std::array<std::uint16_t,128> row_subtract_rgb555{};
    std::uint8_t fog_weight{};
    std::uint8_t skyline_phase{};
    std::uint16_t fog_rgb555=0x7fff;
    bool additive_fixed_color{};
    std::uint8_t skyline_blend_phase{};
    bool operator==(const EnvironmentPresentation&) const = default;
};
// Original RGB555 component blend, including the signed floor and /32 scale.
std::uint16_t blend_environment_rgb555(std::uint16_t from,std::uint16_t to,std::uint8_t weight);
// Recovered video-clock controls (CPU02:AFF0/BA39/BB00); useful for trace checks.
struct EnvironmentVideoState {
    std::uint8_t blue_level{0x80},green_level{0x40},green_counter{},green_step{1},speed_bucket{};
    std::uint8_t color_mode{},active_color_mode{};
    std::array<std::uint8_t,3> pending_color{};
    std::uint16_t fixed_rgb555{0x7fe0};
    std::uint8_t flash_level{0xf0},flash_tick{},flash_step{0xfe},flash_pairs{};
    bool flash_window{},visible_flash_window{};
    bool operator==(const EnvironmentVideoState&) const = default;
};
void advance_environment_video(EnvironmentState& state,EnvironmentVideoState& video,
                               std::uint8_t speed,unsigned ticks);
// Original course callbacks, kept separate from all vehicle forces/candidates.
// State describes recovered background/palette switches, not a camera effect.
class CourseEnvironment {
public:
    CourseEnvironment(const Rom& rom,CourseId course);
    void reset();
    // Once per original phase, passing the pre-physics player. Callbacks consume
    // the previous call's confirmed contact, as CPU09:E9E1..EBE4 do.
    void step(const VehicleState& player,std::uint16_t acknowledged_laps=0,
              std::uint32_t elapsed_centiseconds=0);
    // Advance original 60.0988-Hz video ticks before the next phase callback.
    // This updates only presentation controls; it never changes the vehicle.
    void advance_video(unsigned ticks);
    EnvironmentPresentation presentation() const;
    const EnvironmentState& state() const { return state_; }
    const EnvironmentVideoState& video_state() const { return video_; }
    const EnvironmentEvents& events() const { return events_; }
    std::span<const EnvironmentTrigger> triggers() const { return triggers_; }
private:
    std::vector<EnvironmentTrigger> triggers_;
    std::vector<bool> contact_,latched_,fired_;
    RacePosition previous_cell_position_{};
    bool have_cell_position_{};
    StreamingCourse streaming_;
    CollisionStreamState stream_;
    EnvironmentState state_;
    EnvironmentVideoState video_;
    std::uint8_t speed_{};
    std::uint16_t palette_accumulator_{},palette_step_{},palette_end_{};
    bool forest_{},white_land_{},bridge_outside_latched_{};
    bool night_cruise_{},lake_side_{},big_ravine_{},sky_ramp_{},harbor_city_{},fog_entered_{};
    std::uint8_t skyline_phase_{},skyline_blend_phase_{};
    std::uint16_t fog_accumulator_{};
    struct NightLighting {
        std::uint16_t level=0xe020,step=0x20,previous_height{},height{};
        std::uint8_t first_count=20,middle_count=23;
        std::uint8_t first_limit=24;
        std::array<std::uint8_t,60> table{};
        std::array<std::uint16_t,128> visible{};
        bool active{},vehicle_selected{};
    };
    std::optional<NightLighting> night_;
    std::array<std::uint8_t,56> night_gradient_{};
    void advance_night_video();
    bool dusk_triggered_{};
    EnvironmentEvents events_;
};
}
