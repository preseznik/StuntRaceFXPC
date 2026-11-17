#pragma once
#include "srf/scenery.hpp"

namespace srf {
// Sky Ramp's second-lap 09:9BD7 aircraft carries a player contact sensor.
class SkyRampAircraft {
public:
    explicit SkyRampAircraft(const Rom& rom);
    static constexpr std::uint32_t body_id=0xfffa0000;
    void reset();
    void spawn(std::uint32_t command,Vertex trigger);
    void step(std::span<const ActivationCell> cells);
    void record_contact(const VehicleState& player);
    const VehicleState* body() const {return attached_?&body_:nullptr;}
    std::span<const SceneryPose> poses() const {return visible_?std::span(&pose_,1):std::span<const SceneryPose>{};}
    std::span<const ScenerySprite> sprites() const {return sprites_;}
    std::span<const std::uint8_t> effects() const {return effects_;}
private:
    struct Smoke {ScenerySprite sprite;std::uint8_t life,timer;};
    SceneryPose pose_{};
    Vertex target_{};
    VehicleState body_{};
    std::vector<Smoke> smoke_;
    std::vector<ScenerySprite> sprites_;
    std::vector<std::uint8_t> effects_;
    std::uint16_t diameter_{};
    std::uint8_t life_{},timer_{},retirement_{};
    bool spawned_{},just_spawned_{},attached_{},visible_{},contact_{},hit_{};
};
}
