#include "srf/night_owl_cones.hpp"
#include <algorithm>
#include <bit>

namespace srf {
namespace {
RacePosition position(const VehicleState& state) {
    return {std::bit_cast<std::int16_t>(state.word(0x1e)),std::bit_cast<std::int16_t>(state.word(0x20)),
            std::bit_cast<std::int16_t>(state.word(0x22))};
}
std::array<std::uint16_t,3> extents(const VehicleState& state) {
    return {state.word(0x0c),state.word(0x0e),state.word(0x10)};
}
std::uint8_t level_angle(std::uint8_t angle) {
    // CPU03:CFE3 approaches zero by half the shortest signed-byte difference.
    const auto difference=std::bit_cast<std::int8_t>(std::uint8_t(0-angle));
    return std::uint8_t(angle+(difference==-1 || difference==1?difference:difference/2));
}
}
MovableCourseProps::MovableCourseProps(const Rom& rom,const Scene& scene,CourseId course)
    :carry_plane_response_(course!=CourseId::night_owl) {
    const bool harbor=course==CourseId::harbor_city;
    const bool cones=harbor || course==CourseId::night_owl || course==CourseId::night_cruise || course==CourseId::lake_side;
    const bool signs=harbor || course==CourseId::night_cruise;
    const bool markers=harbor || course==CourseId::sea_breeze;
    for(const auto& object:scene.objects) {
        if(!((cones && object.callback_address==0x0991ca) ||
             (signs && object.callback_address==0x099289) ||
             (markers && object.callback_address==0x09e360)))continue;
        Prop prop{};
        prop.yaw_only=object.callback_address==0x09e360;
        for(unsigned axis=0;axis<3;++axis)prop.extents[axis]=rom.u16(object.command_offset+14+2*axis);
        prop.initial={object.command_offset,object.template_word,object.position,object.orientation};
        prop.folding=object.callback_address==0x099289;
        props_.push_back(prop);
    }
    reset();
}
void MovableCourseProps::reset() {
    effects_.clear();poses_.clear();
    for(auto& prop:props_) {
        const auto initial=prop.initial;const auto size=prop.extents;const bool folding=prop.folding,yaw_only=prop.yaw_only;
        prop={};prop.initial=initial;prop.extents=size;prop.folding=folding;prop.yaw_only=yaw_only;
        poses_.push_back(initial);
    }
}
const VehicleState* MovableCourseProps::body(std::uint32_t id) const {
    const auto it=std::find_if(props_.begin(),props_.end(),[id](const auto& c){return c.initial.command_offset==id;});
    return it!=props_.end() && it->attached?&it->state:nullptr;
}
void MovableCourseProps::cpu_step(StreamingCourse& course,const CollisionStreamState& stream) {
    effects_.clear();
    for(std::size_t i=0;i<props_.size();++i) {
        auto& prop=props_[i];auto& pose=poses_[i];
        const auto entry=std::find_if(course.objects.begin(),course.objects.end(),
            [&](const auto& item){return item.command_offset==pose.command_offset;});
        const auto& state=stream.objects.at(entry-course.objects.begin());
        if(!state.keepalive) {
            // CPU has detached. GSU 903D removes the body only after its final
            // unrefreshed update. Its last CPU pose still represents the prop.
            prop.retiring=prop.attached;
            continue;
        }
        if(prop.folding && prop.latched) {
            // 09:92C7 approaches pose 3 by one per CPU call, then bypasses
            // CE02. The last refreshed body runs one further GSU phase.
            if(pose.frame<3)++pose.frame;
            prop.retiring=prop.attached;
            continue;
        }
        if(!prop.attached) {
            // 08:CBE6 / GSU04:8C0A allocate a fresh body at the last CPU pose.
            auto& b=prop.state;b={};
            b.word(0x1e)=std::uint16_t(pose.position.x);b.word(0x20)=std::uint16_t(pose.position.y);
            b.word(0x22)=std::uint16_t(pose.position.z);b.word(0x24)=std::uint16_t(pose.yaw<<8);
            for(unsigned axis=0;axis<3;++axis)b.word(0x0c+2*axis)=prop.extents[axis];
            b.word(0x16)=0x18;b.word(0x1a)=0x0424;b.word(0x2a)=0x100;
            prop.attached=true;
        }
        // 09:91FF tumbles cones in three axes. Sea Breeze 09:E391 rotates yaw.
        if(prop.spin) {
            pose.yaw+=6;
            if(!prop.yaw_only){pose.pitch+=10;pose.roll+=10;}
            --prop.spin;
        }
        else if(pose.pitch) {
            pose.pitch=level_angle(pose.pitch);
            pose.roll=level_angle(pose.roll);
            pose.yaw+=12;
        }
        // 08:CE02 reindexes the old CPU pose before copying current GSU XYZ.
        entry->cell_offset=original_spatial_cell(std::int16_t(pose.position.x),std::int16_t(pose.position.z));
        const auto p=position(prop.state);pose.position={p[0],p[1],p[2]};
        prop.state.word(0x24)=std::uint16_t(pose.yaw<<8);
        if(prop.hit && !prop.latched) {
            if(prop.folding){effects_.push_back(0x55);pose.yaw+=12;}
            else {effects_.push_back(prop.yaw_only?0x59:0x54);prop.spin=6;}
        }
        prop.latched=prop.hit;
    }
}
void MovableCourseProps::integrate(std::span<const CollisionShape> shapes,std::span<const std::uint32_t> order,
                              const std::array<Vehicle,4>& cars) {
    if(props_.empty())return;
    // Cache every candidate position/velocity before moving any body, as in
    // the original broadphase. A prop may itself be another prop's target.
    std::vector<std::vector<RockContact>> contacts(props_.size());
    for(std::size_t i=0;i<props_.size();++i) {
        auto& prop=props_[i];
        if(!prop.attached)continue;
        const auto p=position(prop.state);
        std::vector<std::uint32_t> overlapping;
        const auto include=[&](std::uint32_t id,std::uint16_t flags,const RacePosition& target,
                               const std::array<std::uint16_t,3>& size) {
            if(!(flags&0x0424) && collision_bounds_overlap(p,prop.extents,target,size))overlapping.push_back(id);
        };
        for(const auto id:order) {
            if(id==prop.initial.command_offset)continue;
            if(const auto* other=body(id))include(id,other->word(0x16),position(*other),extents(*other));
            else {
                const auto shape=std::find_if(shapes.begin(),shapes.end(),[id](const auto& s){return s.command_offset==id;});
                if(shape!=shapes.end())include(id,shape->object_flags,shape->position,shape->extents);
            }
        }
        for(unsigned j=0;j<cars.size();++j) {
            const auto& car=cars[j].body;include(j,car.word(0x16),position(car),extents(car));
        }
        update_contact_candidates(prop.candidates,overlapping);
        for(const auto id:prop.candidates) {
            const auto* other=id<cars.size()?&cars[id].body:body(id);
            if(other)contacts[i].push_back(BodyContactCandidate{{other->word(0x1e),other->word(0x22)},
                                              {other->word(0x2e),other->word(0x32)},other});
            else {
                const auto shape=std::find_if(shapes.begin(),shapes.end(),[id](const auto& s){return s.command_offset==id;});
                contacts[i].push_back(&*shape);
            }
        }
    }
    std::int16_t last_plane_magnitude=0;
    // Original 2332 links newly allocated bodies first. E360's airborne
    // sign may inherit the plane response just calculated by the other one.
    for(const auto id:order) {
        const auto it=std::find_if(props_.begin(),props_.end(),[id](const auto& p){return p.initial.command_offset==id;});
        if(it==props_.end() || !it->attached)continue;
        auto& prop=*it;
        const auto i=std::size_t(it-props_.begin());
        prop.state.word(0x18)=prop.candidates.empty()?0:1;
        prop.hit=advance_night_owl_cone(prop.state,contacts[i],carry_plane_response_?&last_plane_magnitude:nullptr).player_hit;
        if(prop.retiring) {
            prop.attached=prop.retiring=prop.hit=false;
            prop.candidates.clear();
        }
    }
}
}
