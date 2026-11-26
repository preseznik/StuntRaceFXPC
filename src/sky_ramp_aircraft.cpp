#include "srf/sky_ramp_aircraft.hpp"
#include "srf/presentation.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>

namespace srf {
namespace {
std::int16_t signed_word(int v) {return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(v));}
int approach(int current,int target) {
    // CPU03:CDF4 approaches by a quarter of the signed, wrapped difference.
    const int delta=signed_word(target-current);
    return signed_word(current+(delta==0?0:(delta<0?-1:1)*std::max(1,std::abs(delta)/4)));
}
bool active(const Vertex& p,std::span<const ActivationCell> cells,std::uint8_t& timer) {
    const auto cell=original_spatial_cell(signed_word(p.x),signed_word(p.z));
    if(std::ranges::any_of(cells,[=](auto c){return c.offset==cell;}))timer=3;
    return timer && --timer;
}
}
SkyRampAircraft::SkyRampAircraft(const Rom& rom) {
    const auto descriptor=Rom::lorom_offset(0x00a158);
    diameter_=static_cast<std::uint16_t>(original_sprite_diameter(rom.u16(descriptor+16),rom.bytes()[descriptor+7],100));
}
void SkyRampAircraft::reset() {
    pose_={};target_={};body_={};smoke_.clear();sprites_.clear();effects_.clear();
    life_=timer_=retirement_=0;spawned_=just_spawned_=attached_=visible_=contact_=hit_=false;
}
void SkyRampAircraft::spawn(std::uint32_t command,Vertex trigger) {
    if(spawned_)return;
    spawned_=just_spawned_=attached_=true;life_=15;timer_=3;
    pose_={command,0xaa34,{signed_word(trigger.x-9600),signed_word(trigger.y-384),signed_word(trigger.z-8960)},224};
    target_={signed_word(trigger.x+3840),trigger.y,signed_word(trigger.z+3840)};
    body_.word(0x0c)=body_.word(0x0e)=body_.word(0x10)=800;
    body_.word(0x16)=0x30;body_.word(0x1a)=0x042d;body_.word(0x1c)=4;
}
void SkyRampAircraft::step(std::span<const ActivationCell> cells) {
    sprites_.clear();effects_.clear();visible_=life_!=0;
    if(retirement_ && --retirement_==0)attached_=false;
    if(life_) {
        const bool first=just_spawned_;
        just_spawned_=false;
        if(first)effects_.push_back(0x0e);
        auto& p=pose_.position;
        const bool init=timer_==0xff;
        if(init)timer_=3;
        if(first || init || active(p,cells,timer_)) {
            if(!attached_) {
                if(init)attached_=true;
                else timer_=0xff; // 08:D070 schedules sensor reallocation for the next CPU call.
                contact_=false;
            }
            if(hit_) {
                pose_.roll+=24;pose_.yaw+=24;
                p={signed_word(p.x+650),signed_word(p.y-250),signed_word(p.z-50)};
                smoke_.push_back({{pose_.command_offset,{p.x,p.y,signed_word(p.z-200)},0x997c,diameter_,2},8,3});
            } else {
                p={approach(p.x,target_.x),signed_word(approach(p.y,target_.y)-80),approach(p.z,target_.z)};
                if(--life_==0)retirement_=2;
            }
            body_.word(0x1e)=static_cast<std::uint16_t>(p.x);body_.word(0x20)=static_cast<std::uint16_t>(p.y);
            body_.word(0x22)=static_cast<std::uint16_t>(p.z);body_.word(0x24)=std::uint16_t(pose_.yaw)<<8;
            visible_=life_!=0; // The final callback registers the pose but deletion removes it before draw.
            if(contact_ && !hit_){hit_=true;effects_.push_back(0x59);}
        } else {attached_=false;contact_=false;}
    }
    for(auto& smoke:smoke_) {
        if(!active(smoke.sprite.position,cells,smoke.timer))continue;
        if(--smoke.life==0)continue;
        smoke.sprite.frame=static_cast<std::uint16_t>((smoke.sprite.frame+1)%6);
        sprites_.push_back(smoke.sprite);
    }
    std::erase_if(smoke_,[](const Smoke& s){return !s.life;});
}
void SkyRampAircraft::record_contact(const VehicleState& first,const VehicleState* other) {
    contact_=attached_ && (touches(first) || (other && touches(*other)));
}
void SkyRampAircraft::record_contacts(std::span<const Vehicle> drivers) {
    contact_=attached_ && std::ranges::any_of(drivers,[&](const Vehicle& car){return touches(car.body);});
}
bool SkyRampAircraft::touches(const VehicleState& player) const {
    return (player.word(0x16)&2) && (player.word(0x64)&1) &&
        !(player.word(0x16)&body_.word(0x1a)) && collision_bounds_overlap(
        {signed_word(player.word(0x1e)),signed_word(player.word(0x20)),signed_word(player.word(0x22))},
        {player.word(0x0c),player.word(0x0e),player.word(0x10)},
        {signed_word(body_.word(0x1e)),signed_word(body_.word(0x20)),signed_word(body_.word(0x22))},{800,800,800});
}
}
