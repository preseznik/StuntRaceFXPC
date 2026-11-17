#pragma once

#include "srf/streaming.hpp"
#include "srf/race.hpp"
#include "srf/vehicle.hpp"
#include "srf/world.hpp"

namespace srf {
struct ScenerySprite {
    std::uint32_t command_offset{};
    Vertex position{};
    std::uint16_t table{},diameter{};
    std::uint16_t frame{};
    bool operator==(const ScenerySprite&) const = default;
};
// Original CPU scenery callbacks, advanced once per original game phase. The
// player is read-only and is sampled before NativeRace::step; rendering never
// advances this state. This deliberately owns no vehicle/collision candidates.
class CourseScenery {
public:
    CourseScenery(const Rom& rom, CourseId course, bool easy_ride_demo=false);
    void reset();
    void step(const VehicleState& player,std::uint16_t acknowledged_laps=0);
    std::span<const SceneryPose> poses() const { return poses_; }
    // Each ROM tree contributes a screen-facing trunk and crown, phase zero.
    std::span<const ScenerySprite> sprites() const { return sprites_; }
    std::span<const std::uint8_t> effects() const { return effects_; }

private:
    struct Fish {
        SceneryPose initial;
        std::uint32_t callback{};
        std::uint8_t tail{}, active_timer{};
    };
    struct Rotating {
        SceneryPose initial;
        std::uint32_t callback{};
        std::uint8_t active_timer{}, pivot_timer{}, yaw{}, pitch{}, roll{};
        bool initialized{}, visible_latch{};
    };
    enum class CrossingStage { approach, change_model, jump, depart, finished };
    struct CrossingAnimal {
        CrossingStage stage=CrossingStage::approach;
        int target_z{};
        std::uint8_t active_timer=3;
    };
    std::optional<SceneObject> crossing_trigger_;
    std::array<CrossingAnimal,5> crossing_animals_{};
    std::uint8_t crossing_timer_{};
    bool crossing_contact_{}, crossing_started_{};
    std::optional<SceneObject> flyover_trigger_;
    std::uint8_t flyover_trigger_timer_{},flyover_timer_{},flyover_life_{},flyover_lap_=4;
    bool flyover_contact_{};
    struct LakeActor {
        SceneObject trigger;
        std::uint8_t trigger_timer{},actor_timer{},delay=2,moves=80;
        bool allocated{},near{},calling{},revealed{};
    };
    std::optional<LakeActor> lake_actor_;
    struct SkyGate {
        SceneObject source;
        std::uint8_t timer{},last_lap=4;
        bool contact{};
    };
    struct SkyFlight {
        std::size_t pose;
        Vertex target;
        int y_offset,z_offset,forward_step;
        std::uint8_t approach_ticks,life,timer=3;
    };
    std::vector<SkyGate> sky_gates_;
    std::vector<SkyFlight> sky_flights_;
    std::vector<Fish> fish_;
    std::vector<Rotating> rotating_;
    std::optional<SceneryPose> checkpoint_panel_;
    std::uint8_t checkpoint_timer_{};
    std::vector<ScenerySprite> sprites_;
    struct ForestTree {
        std::uint32_t command_offset{};
        Vertex position{};
        std::uint8_t timer{};
        bool contact{},latched{},shed{};
        std::array<VehicleState,4> fragments{};
        std::uint8_t life{};
    };
    std::vector<ForestTree> interactive_trees_;
    std::vector<CollisionShape> tree_surfaces_;
    std::vector<SceneryPose> poses_;
    std::vector<std::uint8_t> effects_;
    std::array<std::int8_t,256> sine_{}, cosine_{};
    ActivationMask activation_;
    Vertex trigger_{};
    bool has_trigger_{};
    std::uint8_t trigger_timer_{};
    bool following_{};
    bool rotating_started_{};
    std::optional<VehicleState> previous_player_;
    std::uint16_t phase_{};
    void step_rotating(std::span<const ActivationCell> cells);
    void step_trees(std::span<const ActivationCell> cells,const VehicleState& player);
    void step_crossing(std::span<const ActivationCell> cells,const VehicleState& player,
                       std::uint16_t acknowledged_laps);
    void step_flyover(std::span<const ActivationCell>,const VehicleState&,std::uint16_t laps);
    void step_lake_actor(std::span<const ActivationCell>,const VehicleState&,std::uint16_t laps);
    void step_sky_flights(std::span<const ActivationCell>,const VehicleState&,std::uint16_t laps);
};
}
