#pragma once

#include "srf/scene.hpp"
#include "srf/vehicle.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace srf {
using RacePosition = std::array<std::int16_t, 3>;
struct RoutePoint {
    RacePosition position;
    std::uint16_t metadata;
    std::uint32_t rom_offset;
};
struct RouteMatch {
    std::uint16_t index; // Original one-based index.
    std::uint16_t distance;
};
struct RouteProgress {
    std::uint16_t current_index;  // Original vehicle +58.
    std::uint16_t accepted_index; // Original vehicle +86.
    std::uint16_t laps;           // Original vehicle +7A.
    std::uint16_t next_distance;  // Original vehicle +84.
};

std::vector<RoutePoint> extract_easy_ride_route(const Rom& rom);
std::vector<RoutePoint> extract_route(const Scene& scene);
// Exact arithmetic and tie order from GSU 04:CC06..CC9F.
std::optional<RouteMatch> nearest_route_point(std::span<const RoutePoint> route,
                                             RacePosition position);
// GSU 04:CCA4..CD55; direction bit 1 selects backwards traversal.
std::uint16_t advance_route_index(std::uint16_t index, std::uint16_t count,
                                  std::uint16_t vehicle_flags);
// GSU 04:CDCA..CE1C; returns true only when the accepted index changes.
bool update_route_progress(RouteProgress& progress, std::uint16_t count);
// GSU 04:CED6..CF07: X/Z Manhattan distance to the next route point, /4.
std::uint16_t route_next_distance(std::span<const RoutePoint> route,
                                  std::uint16_t index, std::uint16_t flags,
                                  RacePosition position);
// GSU 04:CE1D..CED5: stable original linked-list order breaks exact ties.
std::vector<std::uint16_t> rank_racers(std::span<const RouteProgress> racers);

enum class LapEvent { none, first_lap, second_lap, finished };
enum class RaceFailure { none, wrecked, out_of_bounds, time_out };
// Original CPU07:E5CE..E689 tests wreck before lap-completion dispatch.
RaceFailure race_failure(std::uint16_t vehicle_flags);
enum class RacePickup { boost, repair };
// CPU08:B7D6/B80A pass this byte as the high byte of a Q15 fraction.
void apply_race_pickup(VehicleState& player, RacePickup kind,std::uint8_t fraction=0x40);
struct RaceClockTuning {
    std::uint16_t initial_seconds;
    std::array<std::uint16_t,2> lap_seconds;
    std::uint16_t checkpoint_seconds;
    std::uint16_t units_per_tick; // Original decimal ten-thousandths of a second.
};
struct RaceClock {
    std::uint32_t elapsed_units = 0;
    std::uint16_t remaining_seconds = 0;
    std::uint16_t remaining_fraction = 9999;
};
// Default normal Easy Ride settings, recovered from03:EF3F/EFBB,09:EE5A.
RaceClockTuning extract_easy_ride_clock(const Rom& rom);
RaceClockTuning extract_course_clock(const Rom& rom, CourseId course,std::uint16_t vehicle_definition=0x8411);
// Original02:8D99, once per display interrupt while the race clock is enabled.
void advance_race_clock(RaceClock& clock,const RaceClockTuning& tuning,
                        std::uint8_t ticks);
void extend_race_clock(RaceClock& clock,std::uint16_t seconds);
struct RaceCompletion {
    std::uint8_t acknowledged_laps = 0; // Original CPU WRAM 0DEA.
    bool finished = false;
};
// Normal race mode from CPU 07:E6A8..E7DC. Time-trial handling differs.
LapEvent update_race_completion(RaceCompletion& completion,
                               std::uint16_t original_lap_counter);

struct CollisionPlane {
    RacePosition point; // Original signed-byte vertex multiplied by descriptor scale.
    RacePosition normal; // Negated original signed-byte normal, shifted left eight bits.
    std::uint8_t flags; // Low five bits identify one of 19 convex groups.
};
struct CollisionShape {
    std::uint32_t command_offset;
    std::uint16_t descriptor_address;
    RacePosition position;
    std::array<std::uint16_t, 3> extents;
    std::uint8_t orientation;
    std::uint16_t object_flags;
    std::vector<CollisionPlane> planes;
    // CPU08:CAF6 drives visual and collision geometry from the same pose.
    std::shared_ptr<const std::vector<std::vector<CollisionPlane>>> animated_planes{};
    std::uint8_t frame{};
};
struct PlaneContact {
    std::uint16_t flags = 0;
    std::int16_t distance = 32000;
    RacePosition normal{};
};
using ContactManifold = std::array<PlaneContact, 19>;
// Finite barrier in world coordinates. Y increases downwards. Moving decks
// select their wall geometry with the same shape frame as the wheels.
struct CourseWall {
    Vertex a,b; // bottom edge
    int top_a,top_b;
    int shape=-1,frame=0;
};
std::optional<CourseWall> course_wall_from_face(std::span<const Vertex>);
void add_visible_course_walls(std::vector<CourseWall>& walls,const Rom&,const Scene&);
// Returns the absorbed horizontal impulse for the existing crash/damage sample.
std::array<int,2> resolve_course_walls(Vehicle&,RacePosition previous,const std::array<std::uint16_t,9>& orientation,
                                      std::span<const CourseWall>,std::span<const CollisionShape>);
std::vector<CollisionShape> extract_easy_ride_collision(const Rom& rom);
std::vector<CollisionShape> extract_collision(const Rom& rom, const Scene& scene,std::vector<CourseWall>* walls=nullptr);
// Original 04:A20B broadphase uses strict AABB overlap and signed-word arithmetic.
bool collision_bounds_overlap(RacePosition a, std::array<std::uint16_t, 3> a_extents,
                              RacePosition b, std::array<std::uint16_t, 3> b_extents);
// Exact static face-group path 04:9C3D..A123; rejects unsupported orientations.
ContactManifold query_collision_shape(const CollisionShape& shape, RacePosition point,
                                      ContactManifold scratch = {});

// Optional high-rate policy. Penetration remains a force on every step; the
// closing-velocity kick is emitted once on the original response clock.
struct WallContactStep {
    RacePosition velocity;
    bool apply_impulse = true;
};
struct ContactResponseInput {
    RacePosition velocity;
    std::uint16_t damping; // Contact object +44.
    std::uint16_t flags;   // Contact object +46.
    std::int16_t lateral_offset; // Contact object +3C.
    bool skip_vertical_plane = false; // Original scratch2640 != 0.
    std::optional<WallContactStep> wall_step = std::nullopt;
};
struct ContactResponse {
    RacePosition force{};        // Original scratch2340/2342/2344.
    RacePosition impulse{};      // Included in force; high-rate wall policy only.
    RacePosition displacement{}; // Original scratch2388/238A/238C.
    std::uint16_t contact_flags = 0;
    std::uint16_t body_flags = 0; // OR into original body +2C.
    std::int16_t normal_velocity = 0; // Original contact +42 (last processed group).
    std::int16_t minimum_distance = 32000; // Original scratch237E.
    bool response_applied = false;
    bool vertical_plane_seen = false;
};
// Static-plane response from 04:AF54..B189; body torque/application is separate.
ContactResponse respond_to_contacts(const ContactManifold& contacts,
                                    std::uint8_t orientation,
                                    const ContactResponseInput& input);

struct WheelContactTuning {
    std::array<std::uint8_t, 320> sine_bytes{}; // ROM00:96FE..983D, including cosine tail.
    std::uint16_t linear_transfer = 0; // Original2652.
    std::uint16_t angular_mode = 0; // Original2654, table index0,2,4,6,8,10.
    std::uint16_t displacement_limit = 0; // Original2656.
};
WheelContactTuning extract_wheel_contact_tuning(const Rom& rom,
                                               std::array<std::uint16_t, 3> parameters);
struct WheelContactWork {
    RacePosition force{};
    RacePosition impulse{}; // Included in force, before wheel-to-body transfer.
    RacePosition local_force{};
    RacePosition displacement{};
    std::int16_t minimum_distance = 32000;
    std::uint16_t flags = 0;
    std::uint16_t friction = 0;
};
// Original04:AB4B..AEDF. Input work is the preceding plane accumulation.
void finish_wheel_contact(VehicleState& body, VehicleState& wheel,
                          VehicleForces& forces, WheelContactWork& work,
                          const VehicleTuning& tuning,
                          const WheelContactTuning& contact_tuning,
                          const std::array<std::uint16_t, 9>& orientation,
                          std::optional<RacePosition> braking_velocity = std::nullopt);
// Original04:A893..AEDF for a normal attached wheel. Candidate is the head
// of the persistent original broadphase list; null means no current candidate.
WheelContactWork update_wheel_contact(VehicleState& body, VehicleState& wheel,
                                      VehicleForces& forces,
                                      const CollisionShape* candidate,
                                      const VehicleTuning& tuning,
                                      const WheelContactTuning& contact_tuning,
                                      const std::array<std::uint16_t, 9>& orientation,
                                      std::optional<RacePosition> braking_velocity = std::nullopt,
                                      std::optional<WallContactStep> wall_step = std::nullopt);
// Add newly overlapping IDs at the front; retain existing order, remove stale.
// Call in original active-object order, matching04:A345/A462.
void update_contact_candidates(std::vector<std::uint32_t>& candidates,
                               std::span<const std::uint32_t> overlaps);
using VehicleContactCandidates = std::array<std::vector<std::uint32_t>, 4>;
// Original broadphase runs before vehicle callbacks, once per simulation phase.
void refresh_vehicle_contact_candidates(const Vehicle& vehicle,
                                        std::span<const CollisionShape> active_shapes,
                                        VehicleContactCandidates& candidates);
// active_shapes must retain original active-object traversal order. The caller
// refreshes candidates before any attachment/vehicle updates in the phase.
void update_vehicle_wheel_contacts(Vehicle& vehicle, VehicleForces& forces,
                                   std::span<const CollisionShape> active_shapes,
                                   VehicleContactCandidates& candidates,
                                   const VehicleTuning& tuning,
                                   const WheelContactTuning& contact_tuning,
                                   const std::array<std::uint16_t, 9>& orientation);
struct OpponentTuning {
    std::array<std::uint16_t, 256> direction_angles{}; // ROM00:9E40..A03F.
    std::array<std::uint8_t, 320> sine_bytes{};
    struct DistantMotion { std::uint16_t period, fraction; };
    std::array<std::array<DistantMotion, 7>, 42> distant_motion{};
    std::array<std::uint16_t, 42> transition_speed{};
};
OpponentTuning extract_opponent_tuning(const Rom& rom);
// Exact original04:C2F8..C377 lookup angle, including quadrant wrapping.
std::uint16_t original_direction_angle(std::int16_t x, std::int16_t z,
                                       const OpponentTuning& tuning);
// Original04:C7D5..C932 writes vehicle controls+42 from next route target.
// The route metadata is the original desired-speed input, not a new tuning value.
void update_opponent_route_controls(VehicleState& body, RacePosition target,
                                    std::uint16_t desired_speed,
                                    const OpponentTuning& tuning);
// Original04:C933..CA98. Participants retain original racer-list ordering.
void update_opponent_avoidance(std::span<VehicleState> participants, std::size_t index,
                               const std::array<std::uint16_t, 9>& orientation);
// Original04:C595..C72E distant-opponent path, used when object+16 bit400.
// Returns true when the original clears collision candidates after a route step.
bool update_distant_opponent(Vehicle& vehicle, std::span<const RoutePoint> route,
                             const OpponentTuning& tuning, std::uint8_t configuration,
                             std::uint16_t player_rank);
// Original04:C3D5 controller dispatch. True requests ordinary vehicle physics;
// false means the distant controller already updated position for this call.
bool update_opponent_ai(Vehicle& vehicle, std::span<const RoutePoint> route,
                        const OpponentTuning& tuning, const VehicleTuning& vehicle_tuning,
                        std::uint8_t configuration, std::uint16_t player_rank,
                        std::span<const VehicleState> racers, std::size_t index,
                        VehicleContactCandidates& wheel_candidates,
                        std::array<std::uint16_t, 9>& orientation);
}
