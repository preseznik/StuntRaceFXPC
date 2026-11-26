#pragma once
#include "srf/streaming.hpp"
#include "srf/vehicle.hpp"
#include "srf/world.hpp"

namespace srf {
// Night Cruise, CPU 09:937F..96FB. The race supplies confirmed contacts from
// the previous GSU phase. The dropped item is the only new collision body.
class NightCruiseBonus {
public:
    explicit NightCruiseBonus(const Scene& scene);
    void reset();
    void begin_phase();
    void touch(std::uint32_t command_offset);
    void cpu_step(VehicleState& player,std::span<const ActivationCell> cells,VehicleState* recipient=nullptr);
    bool can_collect(const VehicleState& player) const;
    void record_contact(const VehicleState& player) { item_contact_=can_collect(player); }
    const VehicleState* body() const { return item_active_ ? &pickup_ : nullptr; }
    static constexpr std::uint32_t pickup_id=0xfffb0000;
    std::span<const SceneryPose> poses() const { return poses_; }
    std::span<const std::uint8_t> effects() const { return effects_; }
private:
    std::vector<SceneryPose> initial_,poses_;
    std::vector<std::uint8_t> effects_;
    SceneObject trigger_{};
    VehicleState pickup_{};
    std::uint8_t flags_{},track_steps_{},exit_steps_{},flight_timer_{},item_timer_{},rise_steps_{};
    bool spawn_{},started_{},dropped_{},item_active_{},item_contact_{};
};
}
