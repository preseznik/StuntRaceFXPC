#pragma once
#include "srf/scenery.hpp"
#include "srf/sunset_rocks.hpp"

namespace srf {
// ROM gates A458/A4E7 switch tire grip until the lap-two A574 weather ramp.
void update_white_land_grip(VehicleState&,std::uint32_t callback,bool entering,std::uint16_t lap,
                            std::uint64_t phase,std::optional<std::uint64_t>& storm_phase);

struct FragmentParameters {
    RacePosition velocity;
    std::uint8_t size;
};

// White Land's A06A solid sprites and the 098DDF children shared with 099048.
class WhiteLandBreakables {
public:
    static constexpr std::uint32_t first_body_id=0x82000000u;
    explicit WhiteLandBreakables(const Rom&);
    void reset();
    void begin_phase();
    ScenerySprite intact_sprite(const NativeProp&) const;
    void spawn_snowball_fragments(const SunsetRocks&);
    void update_solid(const NativeProp&,bool active,bool player_contact);
    void spawn(std::uint32_t command,const RacePosition&,const std::array<FragmentParameters,4>&);
    void spawn_rock_chips(std::uint32_t command,const RacePosition&);
    void submit_fragments();
    std::span<const SceneryPose> meshes() const {return meshes_;}
    bool solid_retired(std::uint32_t command) const;
    void integrate(std::span<const CollisionShape>,std::span<const std::uint32_t> stream_order,
                   std::span<const NativeProp>,std::span<const Vehicle> cars,std::span<const VehicleState* const> rock_bodies={});
    std::vector<std::uint32_t> overlapping(const VehicleState&) const;
    const VehicleState* body(std::uint32_t id) const;
    std::span<const ScenerySprite> sprites() const {return sprites_;}
    std::span<const std::uint8_t> effects() const {return effects_;}
private:
    struct Fragment {
        std::uint32_t id{},command{};
        VehicleState state{};
        std::vector<std::uint32_t> candidates;
        std::uint8_t size{},age{};
        std::uint16_t model{};
    };
    struct Broken {std::uint32_t command;std::uint8_t age;};
    std::vector<Broken> broken_;
    std::vector<Fragment> fragments_;
    std::vector<ScenerySprite> sprites_;
    std::vector<SceneryPose> meshes_;
    std::vector<std::uint8_t> effects_;
    std::uint16_t radius_{};
    std::uint8_t scale_{};
    std::uint32_t next_id_{};
    Fragment& add_fragment(std::uint32_t command,const RacePosition& origin,const RacePosition& velocity);
    void sprite(std::uint32_t command,const VehicleState&,std::uint16_t table,std::uint8_t size);
};
}
