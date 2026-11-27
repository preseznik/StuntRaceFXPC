#pragma once
#include "srf/driving_input.hpp"
#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"
#include "srf/backdrop.hpp"
#include "srf/audio.hpp"

namespace srf {
struct LocalCourse { unsigned index; std::wstring_view name; };
inline constexpr std::array battle_courses{
    LocalCourse{16,L"Cotton Farm"},LocalCourse{17,L"Toxic Desert"},
    LocalCourse{18,L"Marine Pipe"},LocalCourse{19,L"Port Arena"}};
inline constexpr std::array lan_courses{LocalCourse{0,L"Easy Ride"},LocalCourse{1,L"Aqua Tunnel"},LocalCourse{2,L"Sunset Valley"},LocalCourse{3,L"Night Owl"},LocalCourse{4,L"King's Forest"},LocalCourse{5,L"Sea Breeze"},LocalCourse{6,L"White Land"},LocalCourse{7,L"Night Cruise"},LocalCourse{8,L"Lake Side"},LocalCourse{9,L"Big Ravine"},LocalCourse{10,L"Sky Ramp"},LocalCourse{11,L"Harbor City"},LocalCourse{12,L"Ember Mesa"},LocalCourse{13,L"Kasumi Inner Loop"}};
constexpr bool supports_lan_course(unsigned index) { return index < lan_courses.size(); }
inline constexpr LocalCourse practice_course{27,L"Test Run"};

WorldPalette extract_local_palette(const Rom&,unsigned index);
CourseBackdrop extract_local_backdrop(const Rom&,unsigned index);
std::vector<WorldSprite> extract_local_sprites(const Rom&,unsigned index);
SoundBank extract_local_audio(const Rom&,unsigned index);

inline constexpr unsigned max_lan_drivers=4;

class LocalRace {
public:
    LocalRace(const Rom&,LocalCourse,PlayerCar first,PlayerCar second,bool cpu,std::span<const PlayerCar> extra={},
              DrivingPhysics physics=DrivingPhysics::original);
    void reset();
    void note_input(unsigned driver,std::uint16_t buttons){
        if(physics_==DrivingPhysics::original)buttons&=~drift_button;
        if(buttons)used_.at(driver)=true;
    }
    bool computer(unsigned driver) const {return computer_.at(driver);}
    void step(const std::array<std::uint16_t,max_lan_drivers>& buttons,const std::array<std::optional<std::int16_t>,max_lan_drivers>& steering={});
    std::span<const SceneryPose> bridge_poses() const { return bridges_.poses(); }
    const CollisionShape* wheel_surface(unsigned driver,unsigned wheel) const;
    std::span<const SceneryPose> prop_poses() const { return props_ ? props_->poses() : std::span<const SceneryPose>{}; }
    std::span<const ScenerySprite> spray_sprites() const { return aircraft_ ? aircraft_->sprites() : spray_ ? spray_->sprites() : std::span<const ScenerySprite>{}; }
    std::span<const std::uint8_t> prop_effects() const { return props_ ? props_->effects() : std::span<const std::uint8_t>{}; }
    std::span<const NativeProp> snowmen() const { return snowmen_; }
    const WhiteLandBreakables* ice() const { return ice_ ? &*ice_ : nullptr; }
    std::span<const SceneryPose> snowballs() const { return balls_ ? balls_->poses() : std::span<const SceneryPose>{}; }
    std::span<const std::uint8_t> snowball_effects() const { return balls_ ? balls_->effects() : std::span<const std::uint8_t>{}; }
    std::span<const SceneryPose> bonus_poses() const { return aircraft_ ? aircraft_->poses() : cruise_ ? cruise_->poses() : std::span<const SceneryPose>{}; }
    std::span<const std::uint8_t> bonus_effects() const { return aircraft_ ? aircraft_->effects() : cruise_ ? cruise_->effects() : std::span<const std::uint8_t>{}; }
    bool boost_available() const { return cruise_ && cruise_->body(); }
    std::optional<unsigned> boost_owner() const { return boost_owner_; }
    const Scene& scene() const {return scene_;}
    const Vehicle& car(unsigned i) const {return cars_.at(i);}
    unsigned count() const {return count_;}
    std::vector<std::uint16_t> standings() const;
    unsigned phase() const {return phase_;}
    // Winner is a grid index. A draw uses count() (2 for a solo wreck).
    std::optional<unsigned> result() const {return result_;}
private:
    Scene scene_;
    std::vector<RoutePoint> route_;
    std::vector<CourseWall> walls_;
    std::vector<CollisionShape> shapes_;
    StreamingCourse streamed_course_;
    CollisionStreamState stream_;
    std::optional<MovableCourseProps> props_;
    std::optional<WaterSpray> spray_;
    std::optional<WhiteLandBreakables> ice_;
    std::optional<SunsetRocks> balls_;
    std::vector<NativeProp> snowmen_,grip_gates_;
    std::vector<std::array<bool,max_lan_drivers>> gate_contacts_;
    std::array<std::optional<std::uint64_t>,max_lan_drivers> storm_phases_;
    void step_falling_objects();
    std::array<bool,max_lan_drivers> ravine_traction_{};
    bool ravine_{};
    void step_cruise();
    std::vector<ActivationCell> active_cells() const;
    void step_aircraft();
    std::optional<SkyRampAircraft> aircraft_;
    std::optional<NativeProp> aircraft_gate_;
    bool aircraft_triggered_{},aircraft_pending_{};
    std::optional<NightCruiseBonus> cruise_;
    std::vector<NativeProp> cruise_gates_;
    std::array<bool,max_lan_drivers> cruise_qualified_{};
    std::optional<unsigned> flyover_driver_,boost_contact_,boost_owner_;
    CourseBridges bridges_;
    OpponentTuning opponents_;
    std::vector<Vehicle> cars_,grid_;
    std::array<VehicleTuning,max_lan_drivers> tuning_{};
    std::array<WheelContactTuning,max_lan_drivers> wheels_{};
    std::array<VehicleContactCandidates,max_lan_drivers> candidates_{};
    std::array<VehicleDriveContext,max_lan_drivers> contexts_{};
    std::array<RaceAudioState,max_lan_drivers> inputs_{};
    unsigned count_,phase_{};
    bool cpu_,lan_;
    DrivingPhysics physics_;
    std::array<bool,max_lan_drivers> used_{},computer_{};
    std::optional<unsigned> result_;
};
}
