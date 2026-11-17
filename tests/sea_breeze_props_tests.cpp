#include "srf/night_owl_cones.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
constexpr std::array<unsigned,6> motion_fields{0x1e,0x20,0x22,0x2e,0x30,0x32};
using Motion=std::array<int,6>;
struct Step {
    unsigned phase{},index{};
    int scratch{};
    Motion before{},after{};
    int magnitude{},hit{},cpu_pose{};
    srf::Vertex position{};
    unsigned yaw{};
    std::vector<std::array<int,10>> targets;
};
void check(bool okay,const char* message) {
    if(!okay)throw std::runtime_error(message);
}
std::vector<Step> load_steps() {
    std::vector<Step> signs;
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"sea_breeze_sign_reference.txt");
    std::string line;std::getline(file,line);
    while(std::getline(file,line)) {
        std::istringstream row(line);
        unsigned frame,count;
        Step step;
        row>>frame>>step.phase>>step.index>>step.scratch;
        for(auto& v:step.before)row>>v;
        for(auto& v:step.after)row>>v;
        row>>step.magnitude>>step.hit>>step.cpu_pose;
        if(step.cpu_pose)row>>step.position.x>>step.position.y>>step.position.z>>step.yaw;
        row>>count;
        check(bool(row) && step.index<2 && count<=8,"Invalid original Sea Breeze step");
        step.targets.resize(count);
        for(auto& target:step.targets)for(auto& v:target)row>>v;
        check(bool(row),"Truncated original contacts");
        signs.push_back(std::move(step));
    }
    return signs;
}
void check_motion(const srf::VehicleState& body,const Motion& expected) {
    for(unsigned i=0;i<motion_fields.size();++i)
        check(body.word(motion_fields[i])==std::uint16_t(expected[i]),"Sea Breeze marker motion differs");
}
void check_original(const srf::Rom& rom) {
    const auto scene=srf::extract_course_scene(rom,srf::CourseId::sea_breeze);
    const auto shapes=srf::extract_collision(rom,scene);
    const auto samples=load_steps();
    unsigned count=0,cpu_count=0,hits=0,allocations=0;
    srf::MovableCourseProps signs(rom,scene,srf::CourseId::sea_breeze);
    check(signs.poses().size()==2,"Missing E360 sign");
    srf::StreamingCourse course;
    for(const auto& pose:signs.poses())course.objects.push_back({pose.command_offset,0,0});
    auto stream=srf::reset_collision_stream(course);
    std::array<srf::Vehicle,4> cars{};
    for(auto& car:cars)car.body.word(0x16)=0x400;
    for(std::size_t begin=0;begin<samples.size();) {
        auto end=begin+1;
        while(end<samples.size() && samples[end].phase==samples[begin].phase)++end;
        const std::span phase(samples.data()+begin,end-begin);
        stream.active_command_offsets.clear();
        for(const auto& sample:phase) {
            const auto id=course.objects[sample.index].command_offset;
            if(!signs.body(id)) {
                // Allocation happened at the end of the previous original CPU phase.
                stream.objects[sample.index].keepalive=true;
                signs.cpu_step(course,stream);
                ++allocations;
            }
            stream.active_command_offsets.push_back(id);
        }
        auto& car=cars.back().body;car={};car.word(0x16)=0x400;
        for(const auto& sample:phase) {
            const auto id=course.objects[sample.index].command_offset;
            const auto* body=signs.body(id);
            check(body && body->word(0x0c)==40 && body->word(0x0e)==40 &&
                  body->word(0x10)==40 && body->word(0x2a)==0x100,"Incorrect sign allocation");
            check_motion(*body,sample.before);
            auto isolated=*body;
            std::vector<srf::RockContact> contacts;
            for(const auto& t:sample.targets) {
                if(t[0]&2) {
                    car.word(0x16)=t[0];car.word(0x2a)=t[1];car.word(0x64)=t[2];
                    car.word(0x0c)=400;car.word(0x0e)=240;car.word(0x10)=400;
                    for(unsigned axis=0;axis<3;++axis)car.word(0x1e + axis*2)=std::uint16_t(t[3+axis]);
                    car.word(0x2e)=t[6];car.word(0x32)=t[7];
                    contacts.push_back(srf::BodyContactCandidate{{std::uint16_t(t[8]),std::uint16_t(t[9])},
                                       {std::uint16_t(t[6]),std::uint16_t(t[7])},&car});
                } else {
                    const srf::RacePosition xyz{std::int16_t(t[3]),std::int16_t(t[4]),std::int16_t(t[5])};
                    const auto shape=std::find_if(shapes.begin(),shapes.end(),[&](const auto& s){
                        return s.position==xyz && s.object_flags==t[0];
                    });
                    check(shape!=shapes.end(),"Missing original surface");
                    contacts.push_back(&*shape);
                    if(std::find(stream.active_command_offsets.begin(),stream.active_command_offsets.end(),
                                 shape->command_offset)==stream.active_command_offsets.end())
                        stream.active_command_offsets.push_back(shape->command_offset);
                }
            }
            isolated.word(0x18)=contacts.empty()?0:1;
            auto scratch=std::int16_t(sample.scratch);
            const auto impulse=srf::advance_night_owl_cone(isolated,contacts,&scratch);
            check_motion(isolated,sample.after);
            check(impulse.magnitude==sample.magnitude && impulse.player_hit==bool(sample.hit),
                  "Original 04:910A contact impulse differs");
        }
        signs.integrate(shapes,stream.active_command_offsets,cars);
        for(auto& state:stream.objects)state.keepalive=false;
        unsigned expected_sounds=0;
        for(const auto& sample:phase) {
            const auto id=course.objects[sample.index].command_offset;
            if(const auto* updated=signs.body(id))check_motion(*updated,sample.after);
            stream.objects[sample.index].keepalive=sample.cpu_pose;
            expected_sounds+=sample.hit;
        }
        signs.cpu_step(course,stream);
        check(signs.effects().size()==expected_sounds &&
              (!expected_sounds || signs.effects().front()==0x59),"Incorrect original E3D1 effect");
        hits+=expected_sounds;
        for(const auto& sample:phase)if(sample.cpu_pose) {
            const auto& pose=signs.poses()[sample.index];
            check(pose.visible && pose.visual_descriptor==0xb700 && pose.position==sample.position &&
                  pose.yaw==sample.yaw && !pose.pitch && !pose.roll,"Original E391 render pose differs");
            ++cpu_count;
        }
        count+=unsigned(phase.size());begin=end;
    }
    check(count==307 && cpu_count==295 && allocations==6 && hits==2,"Incomplete Sea Breeze coverage");
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path){std::cout<<"Set SRF_ROM to the supplied ROM\n";return 77;}
        check_original(srf::Rom::load(path));
        std::cout<<"307 original marker trajectories, 295 carried poses, 6 allocations and 2 player impacts matched\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
