#include "srf/sunset_rocks.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace srf {
namespace {
using S=std::int16_t;
S narrow(int value) {return std::bit_cast<S>(static_cast<std::uint16_t>(value));}
S div2(S value) {return value==-1?0:narrow(value>>1);}
S div8(S value) {return div2(div2(div2(value)));}
RacePosition position(const VehicleState& body) {
    return {narrow(body.word(0x1e)),narrow(body.word(0x20)),narrow(body.word(0x22))};
}
std::array<std::uint16_t,3> extents(const VehicleState& body) {
    return {body.word(0xc),body.word(0xe),body.word(0x10)};
}
BodyContactCandidate cached_contact(const VehicleState& body) {
    return {{body.word(0x1e),body.word(0x22)},{body.word(0x2e),body.word(0x32)},&body};
}
RacePosition rotate_normal(RacePosition n,std::uint8_t yaw) {
    switch(yaw) {
    case 0:return n;
    case 0x40:return {narrow(-n[2]),n[1],n[0]};
    case 0x80:return {narrow(-n[0]),n[1],narrow(-n[2])};
    case 0xc0:return {n[2],n[1],narrow(-n[0])};
    default:throw std::runtime_error("Unsupported rock contact orientation");
    }
}
RockContactResult integrate_prop_contacts(VehicleState& body,std::span<const RockContact> contacts,
                                         std::int16_t* last_plane_magnitude=nullptr) {
    for(unsigned axis=0;axis<3;++axis)
        body.word(0x1e + axis*2)+=div8(narrow(body.word(0x2e + axis*2)));
    const auto center=position(body);
    RacePosition force{};
    const auto apply=[&](const PlaneContact& c,std::uint8_t yaw) -> S {
        if(!c.flags || (c.flags&0xff00) || c.distance<0 || c.distance==32000)return 0;
        const auto magnitude=narrow(2*c.distance);
        const auto normal=rotate_normal(c.normal,yaw);
        for(unsigned axis=0;axis<3;++axis) {
            const int impulse=(int(magnitude)*normal[axis])>>16;
            force[axis]=narrow(force[axis]+std::clamp(impulse,-5000,5000));
        }
        return magnitude;
    };
    RockContactResult result{};
    for(const auto& contact:contacts) {
        S magnitude=0;
        if(last_plane_magnitude)*last_plane_magnitude=0; // Original 04:9241.
        if(const auto* shape=std::get_if<const CollisionShape*>(&contact)) {
            for(const auto& plane:query_collision_shape(**shape,center)) {
                if(!plane.flags || (plane.flags&0xff00) || plane.distance<0 || plane.distance==32000)continue;
                magnitude=narrow(-apply(plane,(*shape)->orientation));
            }
        } else {
            const auto& target=std::get<BodyContactCandidate>(contact);
            const auto impulse=radial_body_impulse(body,target);
            magnitude=apply({1,narrow(impulse.strength),{narrow(impulse.direction[0]),0,
                                                         narrow(impulse.direction[2])}},0);
            if(magnitude && (target.target->word(0x16)&2) && (target.target->word(0x64)&1))
                result.player_hit=true;
        }
        if(last_plane_magnitude)*last_plane_magnitude=
            std::holds_alternative<const CollisionShape*>(contact)?narrow(-magnitude):magnitude;
        if(!result.magnitude)result.magnitude=magnitude;
    }
    const auto ground=apply({1,center[1],{0,narrow(0x8100),0}},0);
    // 04:9575 leaves 2378 intact when a point is above the ground. Its
    // nonzero R0 return lets 92EF reuse the preceding plane's magnitude.
    if(last_plane_magnitude && center[1]>=0 && center[1]!=32000)*last_plane_magnitude=ground;
    if(!result.magnitude)result.magnitude=last_plane_magnitude?*last_plane_magnitude:ground;
    // 04:9BD2 uses half force for position, double horizontal and eightfold
    // vertical force for velocity. Only the first candidate controls drag.
    for(unsigned axis=0;axis<3;++axis) {
        body.word(0x1e + axis*2)+=div2(force[axis]);
        body.word(0x2e + axis*2)+=force[axis]*(axis==1?8:2);
    }
    return result;
}
}

RockContactResult advance_sunset_rock(VehicleState& body,std::span<const RockContact> contacts,
                                      std::int16_t* last_plane_magnitude) {
    body.word(0x30)+=120;
    const auto result=integrate_prop_contacts(body,contacts,last_plane_magnitude);
    if(result.magnitude) {
        if(result.magnitude>0)body.word(0x30)-=result.magnitude;
        body.word(0x2e)-=div8(narrow(body.word(0x2e)));
        body.word(0x32)-=div8(narrow(body.word(0x32)));
    }
    return result;
}

RockContactResult advance_night_owl_cone(VehicleState& body,std::span<const RockContact> contacts,
                                        std::int16_t* last_plane_magnitude) {
    // 04:9114 gates gravity on broadphase-list presence; even a target without
    // collision geometry counts. 9139 applies quarter horizontal drag on contact.
    if(body.word(0x18))body.word(0x30)+=120;
    const auto result=integrate_prop_contacts(body,contacts,last_plane_magnitude);
    if(result.magnitude) {
        for(const auto axis:{0x2e,0x32})body.word(axis)-=div2(div2(narrow(body.word(axis))));
    }
    return result;
}

SunsetRocks::SunsetRocks(const Scene& scene) {
    for(const auto& object:scene.objects)if(object.callback_address==0x09d4f4 || object.callback_address==0x099048 || object.callback_address==0x09dd1e) {
        snowballs_=object.callback_address==0x099048;
        ravine_=object.callback_address==0x09dd1e;
        trigger_id_=object.command_offset;
        trigger_={narrow(object.position.x),narrow(object.position.y),narrow(object.position.z)};
    }
}
void SunsetRocks::reset() {
    last_lap_=4;trigger_contact_=false;rocks_={};poses_.clear();effects_.clear();fragment_origins_.clear();
}
const VehicleState* SunsetRocks::body(std::uint32_t id) const {
    if(id<first_body_id || id-first_body_id>=rocks_.size())return nullptr;
    const auto& rock=rocks_[id-first_body_id];
    return rock.allocated?&rock.state:nullptr;
}

void SunsetRocks::cpu_step(bool streamed,const VehicleState& player,std::uint16_t laps,
                           std::span<const ActivationCell> cells) {
    effects_.clear();poses_.clear();fragment_origins_.clear();
    if(!trigger_id_)return;
    bool spawned=false;
    if(streamed && trigger_contact_ && last_lap_!=laps) {
        last_lap_=laps;
        if(laps==2 || (ravine_ && laps==1)) {
            // 09:D58E returns at D694 after these two allocations.
            constexpr std::array<RacePosition,2> rocks{{{2880,-800,-1664},{3840,-960,-1760}}};
            constexpr std::array<RacePosition,2> snow{{{960,-1280,-6400},{-960,-1280,-6400}}};
            // CPU09:DDDE and DEE5 place the two B4D0 actors closer on the final lap.
            constexpr std::array<RacePosition,2> second{{{-4480,-1280,-1920},{-3840,-1440,-1760}}};
            constexpr std::array<RacePosition,2> third{{{-4480,-1280,-1216},{-3840,-1440,-1440}}};
            const auto& offsets=ravine_?(laps==1?second:third):(snowballs_?snow:rocks);
            spawned=true;
            for(unsigned i=0;i<rocks_.size();++i) {
                auto& rock=rocks_[i];rock={};rock.allocated=rock.cpu_alive=true;rock.life=snowballs_?30:28;
                for(unsigned axis=0;axis<3;++axis) {
                    rock.cpu_position[axis]=narrow(trigger_[axis]+offsets[i][axis]);
                    rock.state.word(0x1e + axis*2)=rock.cpu_position[axis];
                    rock.state.word(0x0c + axis*2)=snowballs_?400:192;
                }
                rock.state.word(0x16)=snowballs_?8:0x18;rock.state.word(0x1a)=0x0424;
                rock.state.word(0x2a)=snowballs_?0x0800:0x4600;
                if(snowballs_) {
                    rock.state.word(0x32)=2800;
                    rock.pitch=i?120:60;rock.yaw=i?60:50;rock.roll=i?12:35;
                    rock.timer=3;
                }
            }
            effects_.push_back(snowballs_?0x11:0x53);
        }
    }
    const auto extent=static_cast<std::uint16_t>(ravine_?1920:(snowballs_?5760:7680));
    trigger_contact_=streamed && (player.word(0x16)&2) && (player.word(0x64)&1) &&
        !(player.word(0x16)&0x0424) &&
        collision_bounds_overlap(position(player),extents(player),trigger_,{extent,extent,extent});
    for(auto& rock:rocks_) {
        if(!rock.cpu_alive)continue;
        bool active=true;
        if(snowballs_) {
            // CPU02:CE08 refreshes the last published CPU cell. A missed
            // third refresh detaches the GSU body; its CPU actor survives.
            const auto cell=original_spatial_cell(rock.cpu_position[0],rock.cpu_position[2]);
            if(!spawned && rock.timer!=0xff && std::any_of(cells.begin(),cells.end(),
                [cell](auto c){return c.offset==cell;}))rock.timer=3;
            const bool initialize=rock.timer==0xff;
            if(initialize)rock.timer=3;
            else if(rock.timer && --rock.timer==0 && rock.allocated)rock.detach=1;
            active=rock.timer!=0;
            if(active && !rock.allocated) {
                if(initialize) {
                    rock.allocated=true;rock.hit=false;rock.candidates.clear();
                    for(unsigned axis=0;axis<3;++axis)rock.state.word(0x1e + axis*2)=rock.cpu_position[axis];
                    rock.state.word(0x2e)=rock.state.word(0x30)=0;rock.state.word(0x32)=2800;
                } else rock.timer=0xff;
            }
            if(active && rock.allocated)rock.cpu_position=position(rock.state);
        } else rock.cpu_position=position(rock.state);
        const auto& p=rock.cpu_position;
        if(active) {
            // 09:C56D increments three overlapping little-endian 16-bit angles.
            const unsigned pitch=rock.pitch+12,yaw=rock.yaw+12+(pitch>>8);
            rock.pitch=std::uint8_t(pitch);rock.yaw=std::uint8_t(yaw);
            rock.roll=std::uint8_t(rock.roll+12+(yaw>>8));
            if(--rock.life==0 || rock.hit) {
                // The CPU deletes the object. The attached GSU body runs this
                // phase and one final unrefreshed phase before cleanup at 903D.
                rock.retirement=2;rock.cpu_alive=false;
                if(rock.hit) {
                    effects_.push_back(snowballs_?0x56:0x5a);
                    if(snowballs_ || ravine_)fragment_origins_.push_back(p);
                }
                continue;
            }
        }
        poses_.push_back({trigger_id_,static_cast<std::uint16_t>(snowballs_?0xb6e4:0xb4d0),
                          {p[0],p[1],p[2]},rock.yaw,rock.pitch,rock.roll,0,true});
    }
}

void SunsetRocks::integrate(std::span<const CollisionShape> shapes,std::span<const Vehicle> racers) {
    if(!rocks_[0].allocated&&!rocks_[1].allocated)return;
    std::array<std::vector<RockContact>,2> contacts;
    for(unsigned i=0;i<rocks_.size();++i) {
        auto& rock=rocks_[i];
        if(!rock.allocated)continue;
        std::vector<std::uint32_t> overlapping;
        const auto p=position(rock.state);
        const auto include=[&](std::uint32_t id,const VehicleState& target) {
            if(!(rock.state.word(0x1a)&target.word(0x16)) &&
               collision_bounds_overlap(p,extents(rock.state),position(target),extents(target)))
                overlapping.push_back(id);
        };
        for(const auto& shape:shapes)
            if(!(rock.state.word(0x1a)&shape.object_flags) &&
               collision_bounds_overlap(p,extents(rock.state),shape.position,shape.extents))
                overlapping.push_back(shape.command_offset);
        for(unsigned j=0;j<racers.size();++j)include(j,racers[j].body);
        if(const auto* other=body(first_body_id+(1-i)))include(first_body_id+(1-i),*other);
        update_contact_candidates(rock.candidates,overlapping);
        for(const auto id:rock.candidates) {
            if(id<racers.size())contacts[i].push_back(cached_contact(racers[id].body));
            else if(const auto* other=body(id))contacts[i].push_back(cached_contact(*other));
            else {
                const auto it=std::find_if(shapes.begin(),shapes.end(),
                    [id](const auto& shape){return shape.command_offset==id;});
                contacts[i].push_back(&*it);
            }
        }
    }
    // Newly allocated bodies prepend to the GSU list, second rock first.
    for(unsigned i=rocks_.size();i-- >0;) {
        auto& rock=rocks_[i];
        if(!rock.allocated)continue;
        rock.hit=advance_sunset_rock(rock.state,contacts[i]).player_hit;
        if(rock.detach && --rock.detach==0)rock.allocated=false;
        if(rock.retirement && --rock.retirement==0)rock.allocated=false;
    }
}
}
