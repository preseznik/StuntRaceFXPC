#include "srf/night_cruise_bonus.hpp"
#include "srf/race.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool v,const char* message) {if(!v)throw std::runtime_error(message);}
void replay(const srf::Scene& scene,const srf::ActivationMask& mask,srf::NightCruiseBonus& bonus) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"night_cruise_bonus_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing original bonus trace");
    srf::VehicleState player;player.word(0x16)=0x12;player.word(0x64)=1;
    player.word(0x0c)=player.word(0x10)=400;player.word(0x0e)=240;
    std::vector<std::uint32_t> contacts;
    unsigned compared=0,flyer_calls=0,pickup_calls=0,reward_calls=0,sounds=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,x,y,z,gate,flyer,fx,fy,fz,pitch,life,item,ix,iy,iz,angle,effect;
        int rise,rx,ry,rz,rise_angle,remaining;
        require(bool(row>>phase>>x>>y>>z>>gate>>flyer>>fx>>fy>>fz>>pitch>>life>>item>>ix>>iy>>iz>>angle>>effect
                        >>rise>>rx>>ry>>rz>>rise_angle>>remaining),
                "Invalid original bonus trace");
        player.word(0x1e)=x;player.word(0x20)=y;player.word(0x22)=z;
        bonus.begin_phase();
        for(const auto id:contacts)bonus.touch(id);
        bonus.cpu_step(player,srf::activation_cells(mask,x,z));
        const auto poses=bonus.poses();
        require(poses[0].pitch==gate,"AF74 gate pitch differs from original");
        if(flyer) {
            require(poses.size()>3,"AA34 flyover failed to spawn");
            const auto& p=poses[3];
            require(p.visual_descriptor==0xaa34 && p.position.x==fx && p.position.y==fy && p.position.z==fz &&
                    p.yaw==128 && p.pitch==pitch && p.visible==bool(life),"AA34 pose differs from original");
            ++flyer_calls;
        }
        if(item && !effect) {
            require(poses.size()==5 && bonus.body(),"Missing B7A8 dropped pickup");
            const auto& p=poses[4];
            require(p.position.x==ix && p.position.y==iy && p.position.z==iz &&
                    p.yaw==angle && p.pitch==angle,"B7A8 pose differs from original");
            const auto& body=*bonus.body();
            require(body.word(0x16)==48 && body.word(0x1a)==1069 && body.word(0x1c)==4 &&
                    body.word(0x2a)==5120 && body.word(0x0c)==160 && body.word(0x0e)==160 &&
                    body.word(0x10)==160,"B7A8 collision body differs from recorded GSU state");
            ++pickup_calls;
        }
        if(rise) {
            const auto& p=poses[4];
            require(bool(remaining)==p.visible,"B7A8 reward lifetime mismatch");
            if(p.visible)require(p.position==srf::Vertex{rx,ry,rz} && p.pitch==rise_angle && p.yaw==rise_angle,
                                 "B7A8 reward rise differs from original");
            ++reward_calls;
        }
        const auto events=bonus.effects();
        require(events.size()==unsigned(effect!=0) && (events.empty() || events[0]==effect),"Bonus sound mismatch");
        if(effect==0x25)require(!bonus.body() && player.word(0x6a)==192,"Original one-time bonus boost mismatch");
        bonus.record_contact(player);
        contacts.clear();
        const srf::RacePosition position{static_cast<std::int16_t>(x),static_cast<std::int16_t>(y),static_cast<std::int16_t>(z)};
        for(const auto& o:scene.objects) {
            const std::uint16_t extent=o.callback_address==0x09937f?480:o.callback_address==0x0993ed?3840:0;
            const auto p=o.position;
            if(extent && srf::collision_bounds_overlap(position,{400,240,400},
                {static_cast<std::int16_t>(p.x),static_cast<std::int16_t>(p.y),static_cast<std::int16_t>(p.z)},
                {extent,extent,extent}))contacts.push_back(o.command_offset);
        }
        ++compared;sounds+=events.size();
    }
    require(compared==131 && flyer_calls==42 && pickup_calls==3 && reward_calls==10 && sounds==2,
            "Incomplete original bonus event");
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        const auto path=argc==2?argv[1]:_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        const auto rom=srf::Rom::load(path);const auto scene=srf::extract_course_scene(rom,srf::CourseId::night_cruise);
        const auto mask=srf::extract_streaming(rom,scene).normal;
        srf::NightCruiseBonus bonus(scene);
        replay(scene,mask,bonus);bonus.reset();replay(scene,mask,bonus);
        std::cout<<"Night Cruise bonus matches the original 131-phase drive and restart\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
