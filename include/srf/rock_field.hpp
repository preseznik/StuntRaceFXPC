#pragma once
#include "srf/backdrop.hpp"
#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"
#include "srf/stunt_course.hpp"

namespace srf {
WorldPalette extract_rock_field_palette(const Rom&,StuntCourse course=StuntCourse::rock_field);
CourseBackdrop extract_rock_field_backdrop(const Rom&,StuntCourse course=StuntCourse::rock_field);
std::vector<WorldSprite> extract_rock_field_sprites(const Rom&,StuntCourse course=StuntCourse::rock_field);

class RockFieldDrive {
public:
    RockFieldDrive(const Rom&,PlayerCar,StuntCourse course=StuntCourse::rock_field,
                   StuntAttempt attempt=StuntAttempt::first);
    const Scene& scene() const { return scene_; }
    const Vehicle& player() const { return player_; }
    std::span<const SceneryPose> poses() const { return trailer_; }
    std::span<const SceneryPose> bridge_poses() const {
        return deck_?std::span(&deck_->pose,1):std::span<const SceneryPose>{};
    }
    unsigned phase() const { return drive_.update_phase; }
    unsigned video_tick() const { return field_; }
    unsigned first_clock_field() const;
    bool drive_started() const;
    // One NTSC field; true when the vehicle advanced a simulation phase.
    bool advance_intro(std::uint16_t requested=0,std::optional<std::int16_t> steering={});
    // Original eye XYZ, pitch and yaw, in world units and signed angle words.
    std::array<std::int16_t,5> trailer_camera() const;
    void step(std::uint16_t requested, std::optional<std::int16_t> steering={});
    void checkpoint_reward(std::uint32_t gate_command);
    void set_event_contact(bool active) { player_.body.word(0x18)=active; }
private:
    void advance_car(std::uint16_t control,std::optional<std::int16_t> steering={});
    void scripted_step();
    Scene scene_;
    Vehicle player_;
    VehicleTuning tuning_;
    WheelContactTuning wheels_;
    OpponentTuning directions_;
    VehicleDriveContext drive_{};
    VehicleContactCandidates candidates_;
    RaceAudioState input_{};
    std::vector<CollisionShape> shapes_;
    std::array<SceneryPose,2> trailer_;
    std::array<std::size_t,3> gate_shapes_{};
    struct MovingDeck {
        SceneryPose pose;
        std::size_t shape;
        std::size_t stream_index;
        CollisionAnimation animation;
        StreamingCourse streaming;
        CollisionStreamState stream;
    };
    std::optional<MovingDeck> deck_;
    std::vector<CollisionShape> streamed_shapes_;
    unsigned intro_,field_{};
};
}
