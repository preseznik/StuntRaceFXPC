#pragma once
#include "srf/scenery.hpp"

namespace srf {
enum class PlayerCar : std::uint8_t;

// Tick zero is the original 08:AF0F truck allocation. Only the car being
// loaded advances here. The player takes over the separate tractor on green.
class TrailerLoading {
public:
    TrailerLoading(const Rom& rom, const Scene& scene, PlayerCar selected);
    void advance(unsigned ticks=1);
    unsigned video_ticks() const { return ticks_; }
    unsigned phase() const { return phase_; } // Original 70:2470 through camera handoff.
    const Vehicle& car() const { return car_; }
    std::optional<ScenerySprite> distant_car(double eye_x,double eye_z,std::uint8_t camera_yaw) const;
    bool car_visible() const { return ramp_.visible; }
    const SceneryPose& ramp() const { return ramp_; }
    bool door_closed() const { return closed_; }
    std::uint16_t cargo_descriptor() const { return closed_ ? 0xa75c : 0xa778; }

private:
    void update_car();
    unsigned callback_tick(unsigned phase) const;
    Vehicle car_;
    VehicleTuning tuning_;
    WheelContactTuning wheels_;
    OpponentTuning directions_;
    VehicleDriveContext drive_{};
    VehicleContactCandidates candidates_;
    std::vector<RoutePoint> route_;
    std::vector<CollisionShape> shapes_;
    SceneryPose ramp_;
    std::uint16_t distant_diameter_{};
    std::uint8_t distant_variant_{};
    unsigned league_{}, selected_{}, ticks_{}, phase_=2;
    bool closing_{}, closed_{};
};
}
