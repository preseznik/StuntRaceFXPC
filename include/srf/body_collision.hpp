#pragma once
#include "srf/race.hpp"

namespace srf {
struct BodyContactCandidate {
    // Broadphase caches X/Z at node+8/+A and velocity at+4/+6. Using the
    // target's current values instead changes the original contact response.
    std::array<std::uint16_t,2> position{}, velocity{};
    const VehicleState* target{};
};
struct BodyCollisionContext {
    std::uint16_t damage_mode{}, difficulty{}; // Original263C and26F4.
    DrivingPhysics physics{DrivingPhysics::original};
};
struct BodyCollisionWork {
    std::array<std::uint16_t,3> force{}, normal{};
    std::uint16_t closing_speed{}, magnitude{}, contact_flags{};
};
// Shared original 04:9301 radial impulse, before each actor applies its
// own displacement, damping or damage rules.
struct RadialImpulse {
    std::array<std::uint16_t,3> direction{};
    std::uint16_t closing{}, strength{};
};
RadialImpulse radial_body_impulse(const VehicleState&,const BodyContactCandidate&);
// Original04:CA99 radial/closing-speed rejection after the AABB test.
bool body_pair_overlap_filter(const VehicleState&,const VehicleState&);
// Sweep the racers together after their individual driving and terrain updates.
void resolve_racer_contacts(std::span<Vehicle>,std::span<const RacePosition> previous,
                            std::span<const VehicleTuning>,std::span<const CourseWall>,
                            std::span<const CollisionShape>,std::span<Vehicle> midpoint={});
// Original04:BD20 for the4WD's bit0x10 body-response path, including cached
// candidate order and exact original04:9301/944C force arithmetic.
void update_body_collisions(VehicleState&, VehicleForces&,
                            std::span<const BodyContactCandidate>,
                            const BodyCollisionContext&, BodyCollisionWork&);
// Original04:BF98..C085, triggered by a sufficiently large crash.
void apply_nearby_crash_impulse(const VehicleState& source,
                                std::span<VehicleState* const> racers);
}
