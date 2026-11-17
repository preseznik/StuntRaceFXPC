#pragma once
#include "srf/championship.hpp"
#include "srf/race_audio.hpp"

namespace srf {
struct BonusReward {
    unsigned extra_seconds{}, extra_cars{};
};

class SpeedBonus {
public:
    SpeedBonus(const Rom& rom, SpeedClass league);
    void step(std::uint16_t requested, std::uint8_t clock_ticks,
              std::optional<std::int16_t> steering = {});
    void coast_after_timeout();
    const Vehicle& truck() const { return truck_; }
    const Scene& scene() const { return scene_; }
    const std::vector<SceneryPose>& gates() const { return gates_; }
    const RaceClock& clock() const { return clock_; }
    BonusReward reward() const { return reward_; }
    // Lap spares are awarded on crossing. Flag seconds are banked only on timeout.
    BonusReward collected_reward() const { return {ended()?reward_.extra_seconds:0u,reward_.extra_cars}; }
    bool ended() const { return !clock_.remaining_seconds; }
    bool stayed_parked() const { return stayed_parked_; }
private:
    void update_truck(std::optional<std::int16_t> steering);
    void score_crossings();
    Scene scene_;
    std::vector<RoutePoint> route_;
    std::vector<CollisionShape> shapes_;
    Vehicle truck_;
    VehicleState guide_;
    std::vector<std::uint32_t> guide_candidates_;
    VehicleTuning tuning_;
    WheelContactTuning wheel_tuning_;
    OpponentTuning direction_;
    VehicleDriveContext drive_{};
    std::optional<std::int16_t> last_steering_;
    VehicleContactCandidates candidates_;
    std::vector<SceneryPose> gates_;
    std::vector<std::array<std::uint16_t,3>> gate_extents_;
    RaceAudioState controls_{};
    RaceClockTuning clock_tuning_{};
    RaceClock clock_{};
    BonusReward reward_{};
    bool stayed_parked_{true};
};

}
