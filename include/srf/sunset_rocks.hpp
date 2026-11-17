#pragma once
#include "srf/body_collision.hpp"
#include "srf/race.hpp"
#include "srf/world.hpp"
#include "srf/streaming.hpp"
#include <variant>

namespace srf {
using RockContact = std::variant<const CollisionShape*,BodyContactCandidate>;
struct RockContactResult {
    std::int16_t magnitude{};
    bool player_hit{};
};
// Original GSU 04:907A. Candidates were captured before this phase's
// integration. Road faces and moving solids share one ordered list.
RockContactResult advance_sunset_rock(VehicleState&,std::span<const RockContact>,
                                     std::int16_t* last_plane_magnitude=nullptr);
// GSU 04:910A and 04:907A share the 95BE/91FF integration and contacts.
RockContactResult advance_night_owl_cone(VehicleState&,std::span<const RockContact>,
                                        std::int16_t* last_plane_magnitude=nullptr);

class SunsetRocks {
public:
    explicit SunsetRocks(const Scene&);
    void reset();
    std::uint32_t trigger_id() const { return trigger_id_; }
    RacePosition trigger_position() const { return trigger_; }
    void cpu_step(bool streamed,const VehicleState& player,std::uint16_t laps,
                  std::span<const ActivationCell> cells={});
    void integrate(std::span<const CollisionShape>,const std::array<Vehicle,4>& racers);
    static constexpr std::uint32_t first_body_id=0x80000000u;
    const VehicleState* body(std::uint32_t id) const;
    void append_debris(std::span<const SceneryPose> debris) {poses_.insert(poses_.end(),debris.begin(),debris.end());}
    std::span<const SceneryPose> poses() const { return poses_; }
    std::span<const std::uint8_t> effects() const { return effects_; }
    std::span<const RacePosition> fragment_origins() const { return fragment_origins_; }
private:
    struct Rock {
        VehicleState state{};
        std::vector<std::uint32_t> candidates;
        std::uint8_t life{}, retirement{}, pitch{}, yaw{}, roll{};
        bool allocated{},hit{};
        RacePosition cpu_position{};
        std::uint8_t timer{},detach{};
        bool cpu_alive{};
    };
    std::uint32_t trigger_id_{};
    RacePosition trigger_{};
    std::uint16_t last_lap_=4;
    bool trigger_contact_{};
    bool snowballs_{},ravine_{};
    std::array<Rock,2> rocks_{};
    std::vector<SceneryPose> poses_;
    std::vector<std::uint8_t> effects_;
    std::vector<RacePosition> fragment_origins_;
};
}
