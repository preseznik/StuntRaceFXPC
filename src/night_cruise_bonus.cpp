#include "srf/night_cruise_bonus.hpp"
#include "srf/race.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace srf {
namespace {
int word(int n) {return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(n));}
int approach(int from,int target,int divisor) {
    const int delta=word(target-from);
    if(!delta)return from;
    return word(from+(delta<0?-1:1)*std::max(1,std::abs(delta)/divisor));
}
bool active(const Vertex& p,std::span<const ActivationCell> cells,std::uint8_t& timer) {
    const auto cell=original_spatial_cell(p.x,p.z);
    if(std::any_of(cells.begin(),cells.end(),[=](const auto& entry){return entry.offset==cell;}))timer=3;
    return timer && --timer;
}
}
NightCruiseBonus::NightCruiseBonus(const Scene& scene) {
    for(const auto& object:scene.objects) {
        if(object.callback_address==0x09937f)
            initial_.push_back({object.command_offset,object.template_word,object.position,object.orientation});
        if(object.callback_address==0x0993ed)trigger_=object;
    }
    reset();
}
void NightCruiseBonus::reset() {
    poses_=initial_;effects_.clear();pickup_={};
    flags_=track_steps_=exit_steps_=flight_timer_=item_timer_=rise_steps_=0;
    spawn_=started_=dropped_=item_active_=item_contact_=false;
}
void NightCruiseBonus::begin_phase() {effects_.clear();}
void NightCruiseBonus::touch(std::uint32_t id) {
    if(id==trigger_.command_offset) {
        flags_|=2;
        if(flags_==3 && !started_)spawn_=true;
        return;
    }
    for(std::size_t i=0;i<initial_.size();++i)if(poses_[i].command_offset==id) {
        auto& pitch=poses_[i].pitch;
        pitch=static_cast<std::uint8_t>(approach(pitch,0x33,2));
        flags_|=1;
        return;
    }
}
void NightCruiseBonus::cpu_step(VehicleState& player,std::span<const ActivationCell> cells,VehicleState* recipient) {
    const auto index=initial_.size();
    if(spawn_) {
        spawn_=false;started_=true;
        auto p=trigger_.position;
        p.y=word(p.y-192);p.z=word(p.z+11200);
        poses_.push_back({0,0xaa34,p,128});
        track_steps_=exit_steps_=10;flight_timer_=3;
        effects_.push_back(0x0e);
    }
    if(!started_)return;
    auto& flyer=poses_[index];
    if(exit_steps_ && active(flyer.position,cells,flight_timer_)) {
        auto& p=flyer.position;
        const auto target_z=word(trigger_.position.z-8960);
        if(word(target_z-p.z)<=0) {
            if(track_steps_) {
                --track_steps_;
                p.x=approach(p.x,word(player.word(0x1e)),4);
                p.z=word(approach(p.z,word(player.word(0x22)),2)-896);
            } else {
                if(!dropped_) {
                    dropped_=true;item_timer_=3;
                    pickup_.word(0x16)=0x30;pickup_.word(0x1a)=0x42d;pickup_.word(0x1c)=4;
                    pickup_.word(0x2a)=0x1400;
                    pickup_.word(0x0c)=pickup_.word(0x0e)=pickup_.word(0x10)=160;
                    pickup_.word(0x1e)=p.x;pickup_.word(0x20)=p.y;pickup_.word(0x22)=p.z;
                }
                p.z=word(p.z-384);p.y=word(p.y-30);
                flyer.pitch=static_cast<std::uint8_t>(approach(static_cast<std::int8_t>(flyer.pitch),-8,16));
            }
        } else {
            p.z=word(p.z-80);p.y=word(p.y-240);
            flyer.pitch=static_cast<std::uint8_t>(approach(static_cast<std::int8_t>(flyer.pitch),-64,16));
            --exit_steps_;
        }
        flyer.visible=exit_steps_!=0;
    } else flyer.visible=false;
    if(dropped_ && poses_.size()==index+1) {
        const Vertex p{word(pickup_.word(0x1e)),word(pickup_.word(0x20)),word(pickup_.word(0x22))};
        poses_.push_back({0,0xb7a8,p});
    }
    if(!dropped_)return;
    auto& item=poses_[index+1];
    auto& collector=recipient?*recipient:player;
    if(rise_steps_) {
        if(--rise_steps_) {
            item.position={word(collector.word(0x1e)),word(collector.word(0x20)-192-40*(10-rise_steps_)),word(collector.word(0x22))};
            item.yaw=item.pitch=static_cast<std::uint8_t>(16*(10-rise_steps_));
        } else item.visible=false;
        return;
    }
    if(!pickup_.word(0x16))return;
    item.visible=item_active_=active(item.position,cells,item_timer_);
    if(!item_active_)return;
    item.yaw+=16;item.pitch+=16;
    if(!item_contact_)return;
    apply_race_pickup(collector,RacePickup::boost);
    effects_.push_back(0x25);
    item_active_=false;pickup_={};
    // AE60 runs once in the same object pass that allocates the reward child.
    rise_steps_=9;
    item.position={word(collector.word(0x1e)),word(collector.word(0x20)-232),word(collector.word(0x22))};
    item.yaw=item.pitch=16;
}
bool NightCruiseBonus::can_collect(const VehicleState& player) const {
    bool contact=item_active_ && !(player.word(0x16)&pickup_.word(0x1a)) &&
        (player.word(0x16)&2) && (player.word(0x64)&1);
    for(unsigned i=0;i<3 && contact;++i) {
        const int distance=word(player.word(0x1e + 2*i)-pickup_.word(0x1e + 2*i));
        contact=std::abs(distance)<160+player.word(0x0c+2*i);
    }
    return contact;
}
}
