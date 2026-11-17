#pragma once
#include "srf/course.hpp"
#include "srf/rom.hpp"
#include "srf/vehicle.hpp"
#include <algorithm>
#include <cmath>

namespace srf {
struct DemoCameraPose {
    std::array<int,3> eye{};
    int pitch{},yaw{};
    bool operator==(const DemoCameraPose&) const = default;
};

// CPU08:BFA8 negates the high camera byte before selecting a wheel sprite.
// Negating the 16-bit word first would carry the low byte into the result.
inline std::uint8_t demo_sprite_facing(const DemoCameraPose& pose) {
    return static_cast<std::uint8_t>(-(static_cast<std::uint16_t>(pose.yaw)>>8));
}

// CPU03:8004 authors the cuts. Each tracking shot selects a ROM offset program
// in bank 08; the other shots hold course coordinates or release the camera
// with the Coupe's velocity. All phases below are completed vehicle updates.
class DemoCamera {
    using Vec=std::array<int,3>;
    const Rom& rom_;
    VehicleTuning angles_;
    enum class ShotKind { tracking, roadside, route, dolly };
    struct Shot { unsigned start; ShotKind kind; std::size_t operand; };
    std::array<Shot,8> shots_;
    unsigned phase_{}, shot_index_{};
    DemoCameraPose pose_{};
    Vec local_{}, goal_{}, velocity_{};
    std::size_t next_{};
    unsigned time_{};
    bool settle_{};

    int word(std::size_t offset) const { return static_cast<std::int16_t>(rom_.u16(offset)); }
    Vec point(std::size_t offset) const { return {word(offset),word(offset+2),word(offset+4)}; }
    static int approach(int value,int goal,int divisor) {
        const int d=goal-value;
        return value+(d>0?std::max(1,d/divisor):d<0?std::min(-1,d/divisor):0);
    }
    void start_tracking(std::size_t callback_operand) {
        // Opcode 3 sets the eye offset, opcode 2 installs the tracking flags.
        const auto routine=Rom::lorom_offset(0x080000|rom_.u16(callback_operand));
        const auto script=Rom::lorom_offset(0x080000|rom_.u16(routine+1));
        local_=goal_=point(script+2);
        settle_=(rom_.u16(script+10)&0x10)!=0;
        time_=0;velocity_={};next_=script+12;
    }
    void offset_step() {
        // 08:89E6 eases the local vector first. A newly decoded velocity
        // command starts counting on the following update.
        for(unsigned axis=0;axis<3;++axis)local_[axis]=approach(local_[axis],goal_[axis],8);
        if(time_) {
            for(unsigned axis=0;axis<3;++axis)goal_[axis]+=velocity_[axis];
            --time_;
        } else if(rom_.u16(next_)==1) {
            const auto target=point(next_+4);
            time_=rom_.bytes()[next_+2];
            const int divisor=1<<rom_.bytes()[next_+3];
            for(unsigned axis=0;axis<3;++axis)velocity_[axis]=(target[axis]-goal_[axis])/divisor;
            next_+=10;
        }
    }
    void tracking(const VehicleState& body,bool initialize) {
        VehicleState orbit{};
        orbit.word(0x24)=body.word(0x24)&0xff00;
        orbit.word(0x26)=static_cast<std::int16_t>(body.word(0x26)&0xff00)>>2;
        const auto matrix=vehicle_orientation(orbit,angles_);
        for(unsigned axis=0;axis<3;++axis) {
            int eye=static_cast<std::int16_t>(body.word(0x1e + 2*axis));
            for(unsigned j=0;j<3;++j)
                eye+=(local_[j]*static_cast<std::int16_t>(matrix[3*j+axis]))>>15;
            if(axis==1 && settle_ && !initialize) {
                const auto d=eye-pose_.eye[axis];
                eye=pose_.eye[axis]+(d>>(d<0?1:2));
            }
            pose_.eye[axis]=eye;
        }
    }
    // GSU01:81F7 uses the smaller/larger ratio, truncated to 8 bits,
    // followed by the cartridge's first-octant angle table.
    int angle(int x,int z) const {
        const int ax=std::abs(x),az=std::abs(z);
        int a;
        if(!az)a=0x4000;
        else if(ax==az)a=0x2000;
        else {
            a=rom_.u16(0x1e40+2*(256*std::min(ax,az)/std::max(ax,az)));
            if(ax>az)a=0x4000-a;
        }
        if((x<0)!=(z<0))a=-a;
        return static_cast<std::int16_t>(z<0?a+0x8000:a);
    }
    void aim(const VehicleState& body) {
        const int dx=static_cast<std::int16_t>(body.word(0x1e))-pose_.eye[0];
        const int dy=static_cast<std::int16_t>(body.word(0x20))-pose_.eye[1];
        const int dz=static_cast<std::int16_t>(body.word(0x22))-pose_.eye[2];
        pose_.yaw=angle(dx,dz);
        pose_.pitch=angle(-dy,static_cast<int>(std::sqrt(double(dx)*dx+double(dz)*dz)));
    }
public:
    explicit DemoCamera(const Rom& rom):rom_(rom),shots_{{
        // Each wait in CPU03:8004 schedules a replacement callback for the next phase.
        {1,                   ShotKind::tracking,0x18008},
        {1u+rom.u16(0x1800f),ShotKind::roadside,0x18018},
        {1u+rom.u16(0x18026),ShotKind::tracking,0x18029},
        {1u+rom.u16(0x18035),ShotKind::route,0},
        {1u+rom.u16(0x18047),ShotKind::tracking,0x1804a},
        {1u+rom.u16(0x1804d),ShotKind::roadside,0x18056},
        {1u+rom.u16(0x1805e),ShotKind::tracking,0x18061},
        {1u+rom.u16(0x18064),ShotKind::dolly,0},
    }} {
        for(unsigned i=0;i<angles_.sine.size();++i)angles_.sine[i]=rom_.u16(0x183e + 2*i);
    }
    unsigned phase() const { return phase_; }
    const DemoCameraPose& pose() const { return pose_; }

    // Advance consecutively using the car that supplied this scene packet.
    // A display field can present an existing phase more than once.
    const DemoCameraPose& view(unsigned phase,const Vehicle& car) {
        if(phase==phase_)return pose_;
        if(phase!=phase_+1)throw std::logic_error("Nonconsecutive demo camera phase");
        phase_=phase;
        const auto& body=car.body;
        if(shot_index_+1<shots_.size() && phase==shots_[shot_index_+1].start)++shot_index_;
        const auto& shot=shots_[shot_index_];
        const unsigned age=phase-shot.start;
        switch(shot.kind) {
        case ShotKind::tracking:
            if(age==0)start_tracking(shot.operand);
            else offset_step();
            tracking(body,age==0);
            break;
        case ShotKind::roadside:
            if(age==0) {
                const auto p=shot.operand;
                pose_.eye={word(p),pose_.eye[1]+word(p+5),word(p+2)};
            }
            break;
        case ShotKind::route:
            if(age==0) {
                const auto& course=course_definition(CourseId::easy_ride);
                // 08:8BFB advances three one-based entries on the racing line.
                const auto route=(body.word(0x58)+2)%course.route_points;
                pose_.eye=point(course.coordinate_offset+2+8*route);
                pose_.eye[0]+=1000;
                pose_.eye[1]-=500;
                pose_.eye[2]+=1000;
            }
            break;
        case ShotKind::dolly:
            if(age==0) {
                for(unsigned axis=0;axis<3;++axis)
                    velocity_[axis]=static_cast<std::int16_t>(body.word(0x2e + 2*axis))>>3;
            }
            if(age<=40) {
                for(unsigned axis=0;axis<3;++axis) {
                    pose_.eye[axis]+=velocity_[axis];
                    velocity_[axis]=approach(velocity_[axis],axis==1?-10:0,8);
                }
            }
            // 08:9488 takes over the angles on the final dolly step.
            if(age>=40) {
                pose_.pitch=approach(pose_.pitch,0x1000,32);
                return pose_;
            }
            break;
        }
        aim(body);
        return pose_;
    }
};
}
