#pragma once

#include "srf/native_race.hpp"
#include "srf/streaming.hpp"
#include "srf/world.hpp"

namespace srf {

enum class RockFieldOutcome { driving, finished, timeout };

class RockFieldRules {
public:
    RockFieldRules(const Rom& rom, const Scene& scene, PlayerCar car=PlayerCar::four_wd);
    // NMI can tick independently of object/vehicle updates once the grid releases the clock.
    void advance_clock(std::uint8_t ticks);
    // One original object update. Pass zero video ticks until the start lights release the clock.
    void step(const VehicleState& body, std::uint8_t clock_ticks);
    const RaceClock& clock() const { return clock_; }
    unsigned stars() const { return stars_; }
    unsigned section() const;
    std::uint32_t last_gate_command() const { return last_gate_command_; }
    unsigned bank_seconds() const { return transferring_ ? transferring_ : bank_; }
    unsigned display_bank_seconds() const { return display_bank_; }
    unsigned result_seconds() const { return (clock_.remaining_seconds + transferring_) % 100; }
    RockFieldOutcome outcome() const { return outcome_; }
    std::span<const SceneryPose> star_poses() const {return star_poses_;}
    std::span<const SceneryPose> gate_poses() const {return gate_poses_;}
    bool body_contact(const VehicleState&) const;
private:
    struct Star {
        unsigned pose, reward;
        std::array<std::uint16_t,3> extents;
        std::uint16_t cell;
        int velocity{}, acceleration=2;
        bool collected{}, contact{}, active_last_update{};
        unsigned retiring_updates{};
    };
    struct Gate {
        Vertex trigger{};
        unsigned pose{};
        bool crossed{}, contact{};
    };
    RaceClock clock_;
    RaceClockTuning tuning_;
    ActivationMask mask_;
    std::vector<SceneryPose> star_poses_,gate_poses_;
    std::vector<Star> pickups_;
    std::array<Gate,3> gates_{};
    Vertex finish_{};
    bool finish_contact_{};
    std::uint32_t last_gate_command_{};
    unsigned stars_{}, bank_{}, transferring_{}, display_bank_{};
    RockFieldOutcome outcome_=RockFieldOutcome::driving;
};
}
