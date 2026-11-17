#include "srf/night_owl_cones.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
using Motion=std::array<int,6>;
constexpr std::array<unsigned,6> fields{0x1e,0x20,0x22,0x2e,0x30,0x32};
void compare(const srf::VehicleState& state,const Motion& wanted,unsigned phase,unsigned slot) {
    for(unsigned i=0;i<6;++i)if(state.word(fields[i])!=std::uint16_t(wanted[i])) {
        std::cerr<<"Phase "<<phase<<" slot "<<slot<<" field "<<fields[i]<<": "<<std::int16_t(state.word(fields[i]))
                 <<" expected "<<wanted[i]<<'\n';
        throw std::runtime_error("Night Cruise sign motion changed");
    }
}
struct Sample {unsigned slot;Motion before,after;};
void original_signs(const srf::Rom& rom) {
    const auto scene=srf::extract_course_scene(rom,srf::CourseId::night_cruise);
    const auto surfaces=srf::extract_collision(rom,scene);
    srf::MovableCourseProps props(rom,scene,srf::CourseId::night_cruise);
    check(props.poses().size()==9,"Missing original signs or cones");
    srf::StreamingCourse course;
    for(const auto& pose:props.poses())course.objects.push_back({pose.command_offset,0,0});
    auto stream=srf::reset_collision_stream(course);
    std::array<srf::Vehicle,4> cars{};
    std::vector<Sample> steps;
    std::vector<std::size_t> effects;
    std::vector<srf::CollisionShape> active;
    unsigned phase=0,gsu=0,cpu=0;
    const auto integrate=[&]{
        if(!phase)return;
        stream.active_command_offsets.clear();
        for(const auto& step:steps) {
            const auto id=course.objects.at(step.slot).command_offset;
            const auto* body=props.body(id);check(body,"Missing original GSU sign");
            compare(*body,step.before,phase,step.slot);
            stream.active_command_offsets.push_back(id);
        }
        for(const auto& shape:active)stream.active_command_offsets.push_back(shape.command_offset);
        props.integrate(active,stream.active_command_offsets,cars);
        for(const auto& step:steps) {
            if(const auto* body=props.body(course.objects.at(step.slot).command_offset))compare(*body,step.after,phase,step.slot);
            ++gsu;
        }
        steps.clear();
    };
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"night_cruise_sign_reference.txt");
    std::string line;
    while(std::getline(input,line)) {
        if(line.empty() || line[0]=='#')continue;
        std::istringstream row(line);char type;row>>type;
        if(type=='P') {
            integrate();
            unsigned mask,roads,present,mass,controller,height;int x,y,z,vx,vz;srf::RacePosition at{};
            row>>phase>>mask>>present>>x>>y>>z>>vx>>vz>>height>>mass>>controller>>roads;
            active.clear();
            for(unsigned i=0;i<roads;++i) {
                row>>at[0]>>at[1]>>at[2];
                const auto it=std::find_if(surfaces.begin(),surfaces.end(),[&](const auto& shape){return shape.position==at;});
                check(it!=surfaces.end(),"Missing original road");active.push_back(*it);
            }
            auto& car=cars.back().body;car.word(0x16)=present?18:0x400;car.word(0x2a)=mass;car.word(0x64)=controller;
            car.word(0xc)=400;car.word(0xe)=height;car.word(0x10)=400;
            car.word(0x1e)=x;car.word(0x20)=y;car.word(0x22)=z;car.word(0x2e)=vx;car.word(0x32)=vz;
            stream.objects[5].keepalive=mask&1;stream.objects[6].keepalive=mask&2;
            props.cpu_step(course,stream);
            for(auto event:props.effects()) {
                check(event==0x55,"Unexpected original AF58 sound");effects.push_back(phase-1);
            }
        } else if(type=='G') {
            auto& sample=steps.emplace_back();row>>sample.slot;
            for(auto& v:sample.before)row>>v;
            for(auto& v:sample.after)row>>v;
        } else if(type=='C') {
            unsigned slot,yaw,frame;srf::Vertex at;row>>slot>>at.x>>at.y>>at.z>>yaw>>frame;
            const auto& pose=props.poses()[slot];
            if(pose.position!=at || pose.yaw!=yaw || pose.frame!=frame) {
                std::cerr<<"Original CPU pose mismatch in phase "<<phase<<" slot "<<slot<<'\n';
                throw std::runtime_error("Night Cruise sign visual drift");
            }
            ++cpu;
        } else throw std::runtime_error("Unknown fixture row");
        check(bool(row),"Truncated original sign fixture");
    }
    integrate();
    check(gsu==134 && cpu==415 && effects==std::vector<std::size_t>{396,1146},"Incomplete original two-hit comparison");
}
}
int wmain(){
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        original_signs(srf::Rom::load(path));
        std::cout<<"Night Cruise two original sign hits, 134 GSU updates, 415 CPU poses and two sound edges\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
