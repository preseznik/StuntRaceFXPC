#pragma once
#include "srf/streaming.hpp"
#include "srf/sunset_rocks.hpp"

namespace srf {
class MovableCourseProps {
public:
    MovableCourseProps(const Rom&,const Scene&,CourseId);
    void reset();
    void cpu_step(StreamingCourse&,const CollisionStreamState&);
    void integrate(std::span<const CollisionShape>,std::span<const std::uint32_t> active_order,
                   std::span<const Vehicle>);
    const VehicleState* body(std::uint32_t command) const;
    std::span<const SceneryPose> poses() const {return poses_;}
    std::span<const std::uint8_t> effects() const {return effects_;}
private:
    struct Prop {
        SceneryPose initial;
        std::array<std::uint16_t,3> extents{};
        VehicleState state{};
        std::vector<std::uint32_t> candidates;
        std::uint8_t spin{};
        bool attached{},retiring{},hit{},latched{},folding{},yaw_only{};
    };
    std::vector<Prop> props_;
    bool carry_plane_response_{};
    std::vector<SceneryPose> poses_;
    std::vector<std::uint8_t> effects_;
};
}
