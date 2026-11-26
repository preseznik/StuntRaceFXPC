#pragma once

#include "srf/scenery.hpp"

namespace srf {
// CPU08:D303/D33F and D549. Both particle types draw four billboard frames.
struct WaterParticle {
    ScenerySprite sprite{};
    int dx{},dz{};
    bool advance();
};

class WaterSpray {
public:
    WaterSpray(const Rom& rom,std::span<const Vehicle> cars,
               std::array<std::uint8_t,4> grid_seed={0xc8,0xa7,0x55,0x7f});
    void reset();
    void step(std::span<const Vehicle> cars);
    std::span<const ScenerySprite> sprites() const { return sprites_; }
    std::optional<WaterParticle> spawn(const VehicleState& body,const VehicleState& wheel,
                                       Vertex previous_center,std::uint8_t random) const;
private:
    std::vector<std::array<std::uint8_t,4>> wheel_sizes_;
    std::vector<std::array<Vertex,4>> centers_;
    std::array<std::uint8_t,4> grid_seed_,random_{};
    std::uint16_t radius_{};
    std::uint8_t scale_{};
    std::vector<WaterParticle> particles_;
    std::vector<ScenerySprite> sprites_;
};
}
