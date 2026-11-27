#pragma once

#include "srf/body_collision.hpp"
#include "srf/race.hpp"
#include "srf/streaming.hpp"
#include "srf/sunset_rocks.hpp"
#include "srf/night_owl_cones.hpp"
#include "srf/night_cruise_bonus.hpp"
#include "srf/sky_ramp_aircraft.hpp"
#include "srf/water_spray.hpp"
#include "srf/white_land_breakables.hpp"

namespace srf {
// Original player selection order, CPU03:B6F1. Keep 4WD as the default.
enum class PlayerCar : std::uint8_t { coupe, four_wd, f_type, kestrel, cargo };
constexpr bool custom_car(PlayerCar car) {return car==PlayerCar::kestrel || car==PlayerCar::cargo;}
constexpr PlayerCar chassis(PlayerCar car) { return custom_car(car) ? PlayerCar::coupe : car; }
// One-based course grid slots, indexed by the stable actor order in racers().
using GridOrder=std::array<std::uint16_t,4>;
// One animation clock per physical deck. Rendering and wheel contacts use the same frame.
class CourseBridges {
public:
    CourseBridges() = default;
    CourseBridges(const Rom&,const Scene&,std::span<const CollisionShape>,const StreamingCourse&);
    void reset(std::span<CollisionShape>);
    void advance(const CollisionStreamState&,std::span<CollisionShape>);
    std::span<const SceneryPose> poses() const { return bridge_poses_; }
private:
    struct Bridge {
        std::size_t shape, stream;
        CollisionAnimation animation;
        std::optional<std::uint8_t> fixed_pose;
    };
    std::vector<Bridge> bridges_;
    std::vector<SceneryPose> bridge_poses_;
};
// One step is one original GSU update phase, independent of presentation.
// Input is the original native vehicle control word: left1/right2,
// throttle0x10/reverse0x20/brake0x40/boost0x400.
class NativeRace {
public:
    explicit NativeRace(const Rom& rom, PlayerCar car = PlayerCar::four_wd, CourseId course = CourseId::easy_ride,
                        std::optional<std::uint16_t> championship_carry = std::nullopt,
                        std::optional<GridOrder> championship_grid = std::nullopt,
                        DrivingPhysics physics = DrivingPhysics::original);
    CourseId course_id() const { return course_; }
    void reset();
    // Clock ticks feed the original countdown. Four is a compatibility default
    // for the prototype, not a claim about the workload-dependent SNES clock.
    // Optional proportional steering uses original slew/recenter behavior.
    // Digital input and full stick deflection retain the exact legacy path.
    void step(std::uint16_t controls, std::uint8_t elapsed_clock_ticks = 4,
              std::optional<std::int16_t> steering = std::nullopt,
              std::array<Vehicle,4>* halfway = nullptr,bool drift_held=false);
    const std::array<Vehicle,4>& racers() const { return racers_; }
    static constexpr std::size_t player_index() { return 3; }
    std::uint64_t phase() const { return phase_; }
    const RaceCompletion& completion() const { return completion_; }
    RaceFailure failure() const { return failure_; }
    bool time_over_pending() const { return timeout_phases_left_ != 0; }
    bool ended() const { return completion_.finished || failure_!=RaceFailure::none; }
    const std::array<std::uint16_t,4>& standings() const { return standings_; }
    std::span<const RoutePoint> route() const { return route_; }
    std::span<const SceneryPose> rock_poses() const { return rocks_.poses(); }
    std::span<const std::uint8_t> rock_effects() const { return rocks_.effects(); }
    std::span<const SceneryPose> cone_poses() const { return course_==CourseId::night_owl?movable_props_.poses():std::span<const SceneryPose>{}; }
    std::span<const std::uint8_t> cone_effects() const { return course_==CourseId::night_owl?movable_props_.effects():std::span<const std::uint8_t>{}; }
    std::span<const SceneryPose> prop_poses() const { return course_==CourseId::night_owl?std::span<const SceneryPose>{}:movable_props_.poses(); }
    std::span<const std::uint8_t> prop_effects() const { return course_==CourseId::night_owl?std::span<const std::uint8_t>{}:movable_props_.effects(); }
    std::span<const SceneryPose> bridge_poses() const { return bridges_.poses(); }
    std::span<const SceneryPose> pickup_poses() const { return pickup_poses_; }
    std::span<const std::uint8_t> pickup_effects() const { return pickup_effects_; }
    std::span<const SceneryPose> bonus_poses() const {
        if(sky_aircraft_)return sky_aircraft_->poses();
        return cruise_bonus_?cruise_bonus_->poses():std::span<const SceneryPose>{};
    }
    std::span<const std::uint8_t> bonus_effects() const {
        if(sky_aircraft_)return sky_aircraft_->effects();
        return cruise_bonus_?cruise_bonus_->effects():std::span<const std::uint8_t>{};
    }
    std::span<const ScenerySprite> breakable_sprites() const {
        return breakables_?breakables_->sprites():std::span<const ScenerySprite>{};
    }
    std::span<const std::uint8_t> breakable_effects() const {
        return breakables_?breakables_->effects():std::span<const std::uint8_t>{};
    }
    std::span<const ScenerySprite> spray_sprites() const {
        if(sky_aircraft_)return sky_aircraft_->sprites();
        return spray_ ? spray_->sprites() : std::span<const ScenerySprite>{};
    }
    bool countdown_active() const { return countdown_stage_<6; }
    // The championship grid supplies locked scene updates until its own green signal.
    void release_start_signal() { countdown_stage_=6; }
    std::uint8_t countdown_stage() const { return countdown_stage_; }
    struct TimeAward { std::uint64_t phase; std::uint16_t seconds; bool checkpoint; };
    std::optional<TimeAward> time_award() const {
        if(last_award_ && phase_-last_award_->phase<28 && !ended())return last_award_;
        return {};
    }
    std::uint16_t remaining_seconds() const { return clock_.remaining_seconds; }
    std::uint32_t elapsed_centiseconds() const { return clock_.elapsed_units/100; }

private:
    NativeRace(const Rom& rom, PlayerCar car, CourseId course, const Scene& scene,
               std::optional<std::uint16_t> championship_carry,std::optional<GridOrder> championship_grid,DrivingPhysics physics);
    CourseId course_;
    DrivingPhysics physics_;
    std::optional<GridOrder> championship_grid_;
    void update_streaming();
    void refresh_candidates();
    void update_standings();
    void advance_countdown(std::uint8_t ticks);
    void acknowledge_lap();
    std::array<Vehicle,4> initial_{}, racers_{};
    std::array<VehicleTuning,4> tuning_{};
    std::array<WheelContactTuning,4> wheel_tuning_{};
    OpponentTuning opponents_{};
    std::vector<RoutePoint> route_;
    std::vector<CourseWall> walls_;
    std::vector<CollisionShape> shapes_;
    CourseBridges bridges_;
    std::vector<NativeProp> initial_props_, props_;
    bool ravine_traction_armed_{};
    struct Pickup {
        std::size_t prop;
        SceneryPose initial;
        std::uint8_t remaining{};
    };
    std::vector<Pickup> pickups_;
    std::vector<SceneryPose> pickup_poses_;
    std::vector<std::uint8_t> pickup_effects_;
    std::optional<WaterSpray> spray_;
    std::optional<WhiteLandBreakables> breakables_;
    std::optional<NightCruiseBonus> cruise_bonus_;
    std::optional<SkyRampAircraft> sky_aircraft_;
    std::vector<bool> grip_gate_contact_;
    std::optional<std::uint64_t> storm_phase_;
    std::vector<bool> prop_player_contact_;
    std::vector<std::uint8_t> prop_collected_lap_;
    StreamingCourse streaming_;
    SunsetRocks rocks_;
    MovableCourseProps movable_props_;
    CollisionStreamState stream_;
    std::vector<CollisionShape> active_shapes_;
    std::array<VehicleContactCandidates,4> wheel_candidates_;
    std::array<std::vector<std::uint32_t>,4> body_candidate_ids_;
    std::array<std::vector<BodyContactCandidate>,4> body_candidates_;
    std::array<RacePosition,4> cpu_positions_{};
    std::array<std::uint16_t,4> standings_{};
    VehicleDriveContext drive_{};
    RaceCompletion completion_{};
    RaceFailure failure_{};
    RaceClockTuning clock_tuning_{};
    void award_time(std::uint16_t seconds,bool checkpoint);
    std::optional<TimeAward> last_award_;
    RaceClock clock_{};
    std::uint16_t surface_options_{};
    std::uint16_t frozen_{};
    std::array<std::uint8_t,3> opponent_start_delay_{};
    std::uint8_t countdown_stage_{},countdown_ticks_{};
    std::uint8_t timeout_phases_left_{};
    std::uint64_t phase_{};
};
}
