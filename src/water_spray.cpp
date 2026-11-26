#include "srf/water_spray.hpp"
#include "srf/presentation.hpp"
#include "srf/random.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace srf {
namespace {
int signed_word(int value) { return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value)); }
void move(WaterParticle& p) {
    p.sprite.position.x=signed_word(p.sprite.position.x+p.dx);
    p.sprite.position.z=signed_word(p.sprite.position.z+p.dz);
}
}

bool WaterParticle::advance() {
    if(++sprite.frame==4)return false;
    move(*this);
    return true;
}
WaterSpray::WaterSpray(const Rom& rom,std::span<const Vehicle> cars,
                       std::array<std::uint8_t,4> grid_seed)
    :wheel_sizes_(cars.size()),grid_seed_(grid_seed),radius_(rom.u16(0x21ac+16)),scale_(rom.bytes()[0x21ac+7]) {
    for(std::size_t i=0;i<cars.size();++i)
        for(std::size_t w=0;w<cars[i].wheels.size();++w)
            wheel_sizes_[i][w]=static_cast<std::uint8_t>(rom.u16(0x48000+cars[i].wheels[w].word(0x12)+4));
    reset();
}
void WaterSpray::reset() {
    particles_.clear();sprites_.clear();centers_.assign(wheel_sizes_.size(),{});
    // The original menu shares DP E6..E9 with other consumers. Start this
    // presentation-only stream at the observed grid seed for each course.
    random_=grid_seed_;
}
std::optional<WaterParticle> WaterSpray::spawn(const VehicleState& body,const VehicleState& wheel,
                                              Vertex previous_center,std::uint8_t random) const {
    if(random<0x7f)return std::nullopt; // 08:D1BF allocation gate
    const auto wheel_speed=signed_word(std::abs(signed_word(wheel.word(0x2e)))+
                                      std::abs(signed_word(wheel.word(0x32))));
    const bool fast=wheel_speed>=100;
    const auto size=fast ? std::min(120,std::abs(signed_word(body.word(0x4a)))/2+20) : 20;
    previous_center.y=signed_word(previous_center.y+40);
    const auto diameter=original_sprite_diameter(radius_,scale_,static_cast<std::uint8_t>(size));
    WaterParticle p{{0,previous_center,static_cast<std::uint16_t>(fast?0x9b63:0x9b79),
                     static_cast<std::uint16_t>(diameter)},
                    signed_word(body.word(0x2e))>>3,signed_word(body.word(0x32))>>3};
    move(p); // Initial callback falls through to D549 and advances immediately.
    return p;
}
void WaterSpray::step(std::span<const Vehicle> cars) {
    if(cars.size()!=wheel_sizes_.size())throw std::invalid_argument("Water spray vehicle count changed");
    std::erase_if(particles_,[](WaterParticle& particle){return !particle.advance();});
    for(std::size_t i=0;i<cars.size();++i) {
        const auto& car=cars[i];
        if(car.body.word(0x16)&0x400)continue; // Far-controller wheel callback exits at BD54.
        for(std::size_t w=0;w<car.wheels.size();++w) {
            const auto& wheel=car.wheels[w];
            auto& center=centers_[i][w];
            if((car.body.word(0x64)&3) && (wheel.word(0x46)&0x211)==0x11 && (wheel.word(0x2c)&0x80)) {
                auto p=spawn(car.body,wheel,center,advance_random(random_));
                if(p)particles_.push_back(*p);
            }
            // BF53 writes the wheel's next CPU anchor after the child copied its old XYZ.
            center={signed_word(wheel.word(0x1e)+wheel.word(0x4a)),
                    signed_word(wheel.word(0x20)+wheel.word(0x4c)-wheel_sizes_[i][w]-20),
                    signed_word(wheel.word(0x22)+wheel.word(0x4e))};
        }
    }
    sprites_.clear();
    for(const auto& p:particles_)sprites_.push_back(p.sprite);
}
}
