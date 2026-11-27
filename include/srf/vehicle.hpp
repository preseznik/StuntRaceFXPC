#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace srf {
enum class DrivingPhysics { original, enhanced };
inline constexpr int drift_boost_min_ticks=10,drift_boost_full_ticks=28;
// Original vehicle record only, not a console address space. Words retain the
// original modulo-65536 representation for collision and differential validation.
struct VehicleState {
    std::array<std::uint16_t, 0x98 / 2> words{};
    std::uint16_t& word(std::size_t offset) { return words.at(offset / 2); }
    std::uint16_t word(std::size_t offset) const { return words.at(offset / 2); }
};
struct Vehicle {
    VehicleState body;
    std::uint16_t custom_shell{};
    std::array<VehicleState,4> wheels; // Only first0x52 bytes are original wheel fields.
    std::array<unsigned,4> tire_diameters{};
    std::uint8_t drift_ticks{};
    std::int8_t drift_direction{};
    std::uint8_t drift_charge_ticks{};
};
class Rom;
struct Scene;
struct NativeProp {
    std::uint32_t command_offset{};
    VehicleState state;
    std::uint32_t callback_address{};
    std::uint16_t cpu_x{}, destination_x{};
    std::uint8_t retirement_age{}; // 1 on CPU removal, 3 after the final unrefreshed GSU phase.
};
// Seven original Easy Ride trigger/pickup bodies. Their presence in broadphase
// matters even though flags0x20 suppress the ordinary solid-body response.
std::vector<NativeProp> extract_easy_ride_props(const Rom& rom);
std::vector<NativeProp> extract_props(const Rom& rom, const Scene& scene);
// Called at the original CPU object-update phase. player_contact is the prior
// GSU phase's confirmed prop-to-player candidate, not a new distance estimate.
void update_native_prop(NativeProp& prop, std::uint8_t player_lap,
                        bool active, bool player_contact);
struct VehicleTuning {
    std::array<std::uint16_t, 3> contact_parameters{}; // ROM09:8486, GSU2652/54/56.
    // Original ROM09:848C..84B7, copied to GSU2658..2683 for the 4WD.
    std::array<std::uint16_t, 22> words{};
    std::array<std::uint16_t, 5> gear_limits{};
    std::array<std::array<std::uint8_t, 17>, 6> gear_curves{};
    // F-TYPE has six forward gears. Keep the existing five-gear oracle payload
    // intact; this optional final gear is populated only from its ROM table.
    std::uint16_t sixth_gear_limit{};
    std::array<std::uint8_t, 17> sixth_gear_curve{};
    std::array<std::uint16_t, 257> sine{};
    std::uint16_t at(std::size_t original_address) const {
        return original_address<0x2658 ? contact_parameters.at((original_address-0x2652)/2)
                                      : words.at((original_address-0x2658)/2);
    }
};
struct VehicleForces {
    // Original GSU2334..233E: linear XYZ, then angular yaw/pitch/roll.
    std::array<std::uint16_t, 6> words{};
    std::array<std::uint16_t, 2> contact_friction{};
    unsigned rear_grip_percent=100;
    // Ledger only: words still contains the exact original combined response.
    // Body-contact kicks and control80 are impulses; wheel spring/damper,
    // traction, braking and gravity remain continuous. Direct nearby-crash
    // velocity writes are already impulses and do not pass through this array.
    std::array<std::uint16_t, 6> impulses{};
    std::uint16_t continuous_word(std::size_t axis) const {
        return static_cast<std::uint16_t>(words.at(axis)-impulses.at(axis));
    }
};
struct VehicleDriveContext {
    std::uint16_t update_phase{};   // GSU2470, controls gear change eligibility.
    std::uint16_t boost_options{}; // GSU26F2.
    std::uint16_t damage_mode{}; // GSU263C.
    std::uint16_t damage_accumulator{}; // GSU263E.
    // Original control/filter/damage clock, separate from physical substeps.
    // Default preserves every existing callback. The experimental scheduler
    // sets false between original callbacks and carries filtered controls.
    bool advance_timing = true;
    // Discrete motion responses (recovery, damping, stopping), not continuous
    // forces or hard angle/force bounds. Separate for controlled comparisons;
    // the candidate advances both clocks at original callback boundaries.
    bool advance_motion_rules = true;
    // Residual world-Y wheel contact from the parked tractor. Only the CPU
    // loading actor issues control $0800; normal racer inputs leave this empty.
    std::optional<std::int16_t> loading_contact_y{};
};
struct CollisionShape;
struct CourseWall;
struct BodyContactCandidate;
struct RoutePoint;
struct WheelContactTuning;
struct OpponentTuning;
struct VehicleStepEnvironment {
    std::span<const RoutePoint> route;
    // Broadphase runs once before both original half-step callbacks.
    std::array<const CollisionShape*,4> wheel_candidates{};
    std::span<const BodyContactCandidate> body_candidates;
    std::span<VehicleState* const> racers;
    std::uint16_t difficulty{};
    std::size_t opponent_index{};
    std::uint16_t player_rank{};
    std::array<std::vector<std::uint32_t>,4>* wheel_candidate_lists{};
    bool frozen{}; // Original GSU2472 during initialization.
    // Player only: -32767 left, +32767 right; absence keeps exact digital logic.
    std::optional<std::int16_t> steering_q15;
    // The bonus trailer has a fifth contact on the nose of its tractor unit.
    bool kasumi_walls{};
    VehicleState* trailer_guide{};
    const CollisionShape* trailer_guide_surface{};
    DrivingPhysics physics{DrivingPhysics::original};
    bool drift_held{};
    std::span<const CourseWall> walls{};
    std::span<const CollisionShape> course_shapes{};
};
struct VehicleStepResult {
    VehicleForces forces;
    std::array<std::uint16_t,9> orientation{};
};
// Original vehicle callback04:B1A1..B9AB, one original half-step.
// The scheduler runs this twice for body+1C bit8, with damage_mode0 then1.
VehicleStepResult update_vehicle(Vehicle&,const VehicleTuning&,
    const WheelContactTuning&,const OpponentTuning&,VehicleDriveContext&,
    const VehicleStepEnvironment&);
VehicleStepResult update_player_vehicle(Vehicle&,const VehicleTuning&,
    const WheelContactTuning&,const OpponentTuning&,VehicleDriveContext&,
    const VehicleStepEnvironment&);
VehicleTuning extract_vehicle_tuning(std::span<const std::uint8_t> rom,std::uint16_t original_parameter_address);
Vehicle create_vehicle(std::span<const std::uint8_t> rom,std::uint16_t original_definition,
    const std::array<std::int16_t,3>& position,std::uint16_t yaw,std::uint16_t kind,
    std::uint16_t identifier,std::uint16_t surface_options);
VehicleTuning extract_4wd_tuning(std::span<const std::uint8_t> rom);
Vehicle create_4wd(std::span<const std::uint8_t> rom,
                   const std::array<std::int16_t,3>& position,std::uint16_t yaw=0);
Vehicle create_bonus_trailer(std::span<const std::uint8_t> rom,const std::array<std::int16_t,3>& position,
                             std::uint16_t yaw,VehicleState& guide);
std::array<std::uint16_t,9> vehicle_orientation(const VehicleState&, const VehicleTuning&);
void begin_vehicle_forces(VehicleState&, const VehicleTuning&, VehicleForces&,
                          bool advance_timing = true);
void update_vehicle_controls(VehicleState&, const VehicleTuning&, VehicleForces&, const VehicleDriveContext&,
                             std::optional<std::int16_t> steering_q15 = std::nullopt);
void position_vehicle_attachment(const VehicleState& body, VehicleState& attachment,
                                 const std::array<std::uint16_t,9>& orientation);
// Original04:BCBA..BD1F; normalized velocity alignment at body+6C.
std::array<std::uint16_t,3> normalize_vehicle_vector(const std::array<std::uint16_t,3>&);
void update_vehicle_alignment(VehicleState&, const std::array<std::uint16_t,9>& orientation);
void update_vehicle_contact_state(VehicleState&, const VehicleTuning&, VehicleForces&);
// Returns true when the original nearby-car crash impulse must be applied.
bool update_vehicle_damage(VehicleState&, const VehicleTuning&, const VehicleForces&, VehicleDriveContext&);
// Recovery keeps yaw velocity/torque suppressed between heading corrections.
// No target means recovery is cleared on its next original callback boundary.
void update_vehicle_recovery(VehicleState&, VehicleForces&,
                             std::optional<std::uint16_t> desired_yaw,
                             bool advance_motion_rules = true);

// Original04:BA05..BB16. Returns steering (+3A); input is the filtered control word.
std::uint16_t vehicle_steering(const VehicleState&, const VehicleTuning&, std::uint16_t control,
                             std::optional<std::int16_t> steering_q15 = std::nullopt);
// Original04:B44F..B57A. Updates drive force, gear, boost reserve and engine value.
void vehicle_drivetrain(VehicleState&, const VehicleTuning&, const VehicleDriveContext&, std::uint16_t control);
// Original04:95BE..9682. R1=6 updates angles too; other values update position only.
void integrate_vehicle_pose(VehicleState&, bool include_angles = true);
// Original04:B6C1..B98F. Contact routines must already have populated force words.
void integrate_vehicle_velocity(VehicleState&, VehicleForces&, const std::array<std::uint16_t, 9>& orientation,
                                 bool advance_timing = true, bool advance_motion_rules = true);
}
