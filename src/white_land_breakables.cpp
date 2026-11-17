#include "srf/white_land_breakables.hpp"
#include "srf/presentation.hpp"
#include <algorithm>
#include <bit>

namespace srf {
namespace {
RacePosition position(const VehicleState& b) {
    return {std::bit_cast<std::int16_t>(b.word(0x1e)),std::bit_cast<std::int16_t>(b.word(0x20)),
            std::bit_cast<std::int16_t>(b.word(0x22))};
}
std::array<std::uint16_t,3> bounds(const VehicleState& b) {
    return {b.word(0xc),b.word(0xe),b.word(0x10)};
}
constexpr std::array<FragmentParameters,4> ice{{
    {{40,-45,0},20},{{-40,-45,0},3},{{0,-50,40},10},{{0,-50,-40},1}
}};
}
WhiteLandBreakables::WhiteLandBreakables(const Rom& rom) {
    const auto descriptor=Rom::lorom_offset(0x00a0cc);
    radius_=rom.u16(descriptor+16);
    scale_=rom.bytes()[descriptor+7];
    reset();
}
void WhiteLandBreakables::reset() {
    fragments_.clear();broken_.clear();sprites_.clear();meshes_.clear();effects_.clear();
    next_id_=first_body_id;
}
void WhiteLandBreakables::begin_phase() {
    sprites_.clear();meshes_.clear();effects_.clear();
    std::erase_if(fragments_,[](const auto& p){return p.age==16;});
    for(auto& p:broken_)if(p.age<2)++p.age;
}
void WhiteLandBreakables::sprite(std::uint32_t command,const VehicleState& state,
                                 std::uint16_t table,std::uint8_t size) {
    const auto p=position(state);
    sprites_.push_back({command,{p[0],p[1],p[2]},table,
                        static_cast<std::uint16_t>(original_sprite_diameter(radius_,scale_,size))});
}
void WhiteLandBreakables::update_solid(const NativeProp& prop,bool active,bool player_contact) {
    if(!active || std::any_of(broken_.begin(),broken_.end(),[&](const auto& b){
        return b.command==prop.command_offset;
    }))return;
    if(!player_contact) {
        sprite(prop.command_offset,prop.state,0x996d,100);
        return;
    }
    broken_.push_back({prop.command_offset,0});
    effects_.push_back(0x10);
    spawn(prop.command_offset,position(prop.state),ice);
}
bool WhiteLandBreakables::solid_retired(std::uint32_t command) const {
    return std::any_of(broken_.begin(),broken_.end(),[=](const auto& p){return p.command==command && p.age>=2;});
}
WhiteLandBreakables::Fragment& WhiteLandBreakables::add_fragment(std::uint32_t command,const RacePosition& origin,
                                                                 const RacePosition& velocity) {
    auto& f=fragments_.emplace_back();
    f.id=next_id_++;f.command=command;
    for(unsigned i=0;i<3;++i) {
        f.state.word(0x1e +2*i)=origin[i];
        f.state.word(0x2e +2*i)=static_cast<std::uint16_t>(velocity[i]*8);
    }
    f.state.word(0x16)=8;f.state.word(0x1a)=0x0424;
    return f;
}
void WhiteLandBreakables::spawn_rock_chips(std::uint32_t command,const RacePosition& origin) {
    // CPU09:D79C allocates eight ABF4 meshes. 09:C4C0 leaves their mass zero.
    constexpr std::array<RacePosition,8> velocity{{
        {435,-5,770},{390,-7,690},{-350,-10,0},{-420,-10,790},
        {600,-10,530},{-295,-20,415},{-420,-5,0},{-260,-10,-580}
    }};
    for(const auto& v:velocity)add_fragment(command,origin,v).model=0xabf4;
}
void WhiteLandBreakables::spawn(std::uint32_t command,const RacePosition& origin,
                               const std::array<FragmentParameters,4>& parameters) {
    for(const auto& p:parameters) {
        auto& f=add_fragment(command,origin,p.velocity);
        f.size=p.size;f.state.word(0x2a)=64;
    }
}
void WhiteLandBreakables::submit_fragments() {
    // 098DF8 decrements 15 before draw. The CPU copies the pre-GSU position.
    for(const auto& f:fragments_) {
        if(f.age>=14)continue;
        if(!f.model){sprite(f.command,f.state,0x996b,f.size);continue;}
        // CPU09:C4E1 adds 24 independently to the three angle bytes before draw.
        const auto angle=static_cast<std::uint8_t>((f.age+1)*24);
        const auto p=position(f.state);
        meshes_.push_back({f.command,f.model,{p[0],p[1],p[2]},angle,angle,angle});
    }
}
std::vector<std::uint32_t> WhiteLandBreakables::overlapping(const VehicleState& car) const {
    std::vector<std::uint32_t> result;
    if(!(car.word(0x1a)&8))
        for(auto it=fragments_.rbegin();it!=fragments_.rend();++it)
            if(collision_bounds_overlap(position(car),bounds(car),position(it->state),{}))result.push_back(it->id);
    return result;
}
const VehicleState* WhiteLandBreakables::body(std::uint32_t id) const {
    const auto it=std::find_if(fragments_.begin(),fragments_.end(),[=](const auto& f){return f.id==id;});
    return it==fragments_.end()?nullptr:&it->state;
}
void WhiteLandBreakables::integrate(std::span<const CollisionShape> shapes,std::span<const std::uint32_t> order,
                                    std::span<const NativeProp> props,const std::array<Vehicle,4>& cars,std::span<const VehicleState* const> rock_bodies) {
    // Children have zero extents, so they never overlap one another. Preserve
    // the original terrain/solid/car candidate order and cache car velocity.
    std::int16_t last_plane_magnitude=0;
    for(auto it=fragments_.rbegin();it!=fragments_.rend();++it) {
        auto& f=*it;
        std::vector<std::uint32_t> matches;
        const auto p=position(f.state);
        const auto prop=[&](auto id)->const VehicleState* {
            if(id<cars.size())return &cars[id].body;
            if(id>=SunsetRocks::first_body_id && id-SunsetRocks::first_body_id<rock_bodies.size())
                return rock_bodies[id-SunsetRocks::first_body_id];
            const auto it=std::find_if(props.begin(),props.end(),[=](const auto& x){return x.command_offset==id;});
            return it==props.end() || it->retirement_age>=3?nullptr:&it->state;
        };
        const auto shape=[&](auto id){return std::find_if(shapes.begin(),shapes.end(),
            [=](const auto& x){return x.command_offset==id;});};
        const auto include=[&](auto id,const VehicleState& target) {
            if(!(target.word(0x16)&0x0424) && collision_bounds_overlap(p,{},position(target),bounds(target)))
                matches.push_back(id);
        };
        for(std::size_t slot=rock_bodies.size();slot-- >0;)
            if(rock_bodies[slot])include(SunsetRocks::first_body_id+std::uint32_t(slot),*rock_bodies[slot]);
        for(auto id:order) {
            if(const auto it=shape(id);it!=shapes.end()) {
                if(!(it->object_flags&0x0424) && collision_bounds_overlap(p,{},it->position,it->extents))matches.push_back(id);
            } else if(const auto* other=prop(id);other && !solid_retired(id))include(id,*other);
        }
        for(unsigned i=0;i<cars.size();++i)include(i,cars[i].body);
        update_contact_candidates(f.candidates,matches);
        std::vector<RockContact> contacts;
        for(const auto id:f.candidates) {
            if(const auto* target=prop(id))
                contacts.push_back(BodyContactCandidate{{target->word(0x1e),target->word(0x22)},
                                    {target->word(0x2e),target->word(0x32)},target});
            else contacts.push_back(&*shape(id));
        }
        advance_sunset_rock(f.state,contacts,&last_plane_magnitude);
        ++f.age;
    }
}
}
