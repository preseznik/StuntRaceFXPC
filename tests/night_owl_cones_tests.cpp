#include "srf/night_owl_cones.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition,const char* reason) {if(!condition)throw std::runtime_error(reason);}
constexpr std::array<unsigned,6> fields{0x1e,0x20,0x22,0x2e,0x30,0x32};
struct Sample {
    int phase{};
    std::uint32_t command{};
    std::array<int,6> before{},after{};
    int magnitude{},hit{};
    std::vector<std::array<int,10>> contacts;
};
std::vector<Sample> load_trajectory() {
    std::ifstream in(std::filesystem::path(SRF_TEST_FIXTURES)/"night_owl_cone_reference.txt");
    std::string line;std::getline(in,line);
    std::vector<Sample> rows;
    while(std::getline(in,line)) {
        std::istringstream s(line);
        Sample r;unsigned frame,slot,count;
        s>>frame>>r.phase>>slot;
        require(slot==5048 || slot==5512,"Unexpected original cone slot");
        r.command=slot==5048?174180:174202;
        for(auto& x:r.before)s>>x;
        for(auto& x:r.after)s>>x;
        s>>r.magnitude>>r.hit>>count;r.contacts.resize(count);
        for(auto& target:r.contacts)for(auto& x:target)s>>x;
        require(bool(s),"Invalid original cone record");
        rows.push_back(r);
    }
    require(rows.size()==72,"Missing original cone trajectories");return rows;
}
void check_trajectory(const srf::Rom& rom) {
    auto scene=srf::extract_course_scene(rom,srf::CourseId::night_owl);
    const auto shapes=srf::extract_collision(rom,scene);
    const auto road=std::find_if(shapes.begin(),shapes.end(),[](const auto& s){
        return s.position==srf::RacePosition{-14528,0,17152};
    });
    require(road!=shapes.end(),"Missing original cone contact surface");
    srf::MovableCourseProps cones(rom,scene,srf::CourseId::night_owl);
    require(cones.poses().size()==7,"Missing Night Owl cones");
    srf::StreamingCourse stream;
    srf::CollisionStreamState active;
    for(const auto& pose:cones.poses()) {
        stream.objects.push_back({pose.command_offset,0,0});
        const bool selected=pose.command_offset==174180 || pose.command_offset==174202;
        active.objects.push_back({2,true,selected,selected});
        if(selected)active.active_command_offsets.push_back(pose.command_offset);
    }
    active.active_command_offsets.push_back(road->command_offset);
    std::array<srf::Vehicle,4> racers{};
    const auto clear_car=[&]{racers.back().body={};racers.back().body.word(0x16)=0x400;};
    clear_car();
    const std::span surface(&*road,1);
    cones.cpu_step(stream,active);
    for(unsigned i=0;i<5;++i){cones.integrate(surface,active.active_command_offsets,racers);cones.cpu_step(stream,active);}
    const auto rows=load_trajectory();
    std::ifstream rendered(std::filesystem::path(SRF_TEST_FIXTURES)/"night_owl_cone_poses.csv");
    std::string line;require(bool(std::getline(rendered,line)),"Missing original cone presentation");
    unsigned hit_count=0,pose_count=0;
    for(std::size_t begin=0;begin<rows.size();begin+=2) {
        clear_car();
        for(std::size_t i=begin;i<begin+2;++i) {
            const auto& r=rows[i];
            const auto* body=cones.body(r.command);require(body!=nullptr,"Active cone body missing");
            srf::VehicleState isolated=*body;
            std::vector<srf::RockContact> contacts;
            for(const auto& t:r.contacts) {
                if(t[0]&0x10) {
                    auto& car=racers.back().body;
                    car.word(0x16)=t[0];car.word(0x2a)=t[1];car.word(0x64)=t[2];
                    car.word(0x0c)=400;car.word(0x0e)=240;car.word(0x10)=400;
                    for(unsigned axis=0;axis<3;++axis)car.word(0x1e + axis*2)=t[3+axis];
                    car.word(0x2e)=t[6];car.word(0x32)=t[7];
                    contacts.push_back(srf::BodyContactCandidate{{std::uint16_t(t[8]),std::uint16_t(t[9])},
                                               {std::uint16_t(t[6]),std::uint16_t(t[7])},&car});
                } else {
                    require(road->position==srf::RacePosition{short(t[3]),short(t[4]),short(t[5])},
                            "Unexpected original road contact");
                    contacts.push_back(&*road);
                }
            }
            for(std::size_t j=0;j<fields.size();++j)
                require(isolated.word(fields[j])==std::uint16_t(r.before[j]),"Carried cone body diverged");
            isolated.word(0x18)=contacts.empty()?0:1;
            const auto response=srf::advance_night_owl_cone(isolated,contacts);
            require(response.magnitude==r.magnitude && response.player_hit==bool(r.hit),"Cone impulse mismatch");
            for(std::size_t j=0;j<fields.size();++j)
                require(isolated.word(fields[j])==std::uint16_t(r.after[j]),"Cone position or velocity mismatch");
        }
        cones.integrate(surface,active.active_command_offsets,racers);
        cones.cpu_step(stream,active);
        unsigned expected_sounds=0;
        for(unsigned i=0;i<2;++i) {
            require(bool(std::getline(rendered,line)),"Truncated original CPU pose");
            std::replace(line.begin(),line.end(),',',' ');
            std::istringstream row(line);int phase,pitch,yaw,roll,effect;unsigned command;srf::Vertex xyz{};
            row>>phase>>command>>xyz.x>>xyz.y>>xyz.z>>pitch>>yaw>>roll>>effect;
            require(bool(row)&&phase==rows[begin].phase,"Original pose alignment");
            const auto poses=cones.poses();
            const auto p=std::find_if(poses.begin(),poses.end(),[&](const auto& v){return v.command_offset==command;});
            require(p!=poses.end() && p->visible && p->visual_descriptor==0xac80 &&
                p->position==xyz && p->pitch==pitch && p->yaw==yaw && p->roll==roll,"Original CPU cone pose mismatch");
            expected_sounds+=effect;++pose_count;
        }
        require(cones.effects().size()==expected_sounds,"Cone sound timing mismatch");
        for(auto effect:cones.effects()){require(effect==0x54,"Wrong cone sound");++hit_count;}
    }
    require(hit_count==3 && pose_count==72,"Incomplete cone contact coverage");
    const auto last=cones.poses()[5];
    for(auto& state:active.objects)state.keepalive=false;
    cones.cpu_step(stream,active);
    require(cones.body(last.command_offset)!=nullptr,"Cone lost its last GSU phase");
    cones.integrate(surface,active.active_command_offsets,racers);
    require(!cones.body(last.command_offset) && cones.poses()[5]==last,"Retired cone lost its last render pose");
    active.objects[5].keepalive=true;
    cones.cpu_step(stream,active);
    const auto* restored=cones.body(last.command_offset);
    require(restored && std::int16_t(restored->word(0x1e))==last.position.x &&
            std::int16_t(restored->word(0x22))==last.position.z &&
            !restored->word(0x2e) && !restored->word(0x30) && !restored->word(0x32),
            "Stream reentry did not allocate a stationary cone at its last position");
    cones.reset();
    for(const auto& p:cones.poses())require(!cones.body(p.command_offset)&&!p.pitch&&!p.roll,"Cone reset failed");
    require(cones.effects().empty(),"Cone sound survived restart");
    srf::MovableCourseProps prior(rom,scene,srf::CourseId::sunset_valley);
    require(prior.poses().empty(),"Prior course enabled Night Owl contacts");
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path){std::cout<<"Set SRF_ROM to the supplied ROM\n";return 77;}
        check_trajectory(srf::Rom::load(path));
        std::cout<<"72 original cone physics steps, 72 CPU poses and 3 player contact sounds matched\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
