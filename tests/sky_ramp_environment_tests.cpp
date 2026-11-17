#include "srf/environment.hpp"
#include "srf/scenery.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const std::string& reason) {if(!ok)throw std::runtime_error(reason);}
std::ifstream fixture(const char* name) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/name);
    std::string header;require(bool(std::getline(input,header)),"Missing Sky Ramp reference");
    return input;
}
struct Aircraft {srf::Vertex position;unsigned yaw,life;};
void replay(srf::CourseEnvironment& environment,srf::CourseScenery& scenery) {
    std::multimap<unsigned,Aircraft> flights;
    auto actors=fixture("sky_ramp_flyovers_reference.csv");std::string line;
    while(std::getline(actors,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        unsigned phase;Aircraft a;
        require(bool(row>>phase>>a.position.x>>a.position.y>>a.position.z>>a.yaw>>a.life),"Invalid Sky Ramp aircraft row");
        flights.emplace(phase,a);
    }
    auto input=fixture("sky_ramp_environment_reference.csv");
    srf::VehicleState player;player.word(0x16)=0x12;player.word(0x64)=1;
    player.word(0x0c)=player.word(0x10)=400;player.word(0x0e)=240;
    unsigned phases=0,poses=0,calls=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,x,y,z,lap,flags,fog,target,sounds;
        require(bool(row>>phase>>x>>y>>z>>lap>>flags>>fog>>target>>sounds),"Invalid Sky Ramp phase row");
        player.word(0x1e)=x;player.word(0x20)=y;player.word(0x22)=z;
        environment.step(player,lap);scenery.step(player,lap);
        const auto at=" at original phase "+std::to_string(phase);
        const auto video=environment.presentation();
        require(environment.state().flags==flags && video.fog_weight==fog && video.fog_rgb555==target,
                "Sky Ramp cloud mismatch"+at);
        const auto effects=scenery.effects();
        require(effects.size()==unsigned(sounds) && std::all_of(effects.begin(),effects.end(),
            [](auto effect){return effect==0x0e;}),"Sky Ramp flyover sound mismatch"+at);
        const auto [begin,end]=flights.equal_range(phase);
        unsigned visible=0;
        for(auto it=begin;it!=end;++it) {
            const auto& a=it->second;
            require(std::any_of(scenery.poses().begin(),scenery.poses().end(),[&](const srf::SceneryPose& p) {
                return p.visual_descriptor==0xaa34 && p.position==a.position && p.yaw==a.yaw &&
                       p.visible==bool(a.life);
            }),"Sky Ramp aircraft position or visibility mismatch"+at);
            ++poses;visible+=a.life!=0;
        }
        require(std::count_if(scenery.poses().begin(),scenery.poses().end(),[](const auto& p) {
            return p.visual_descriptor==0xaa34 && p.visible;
        })==visible,"Unexpected Sky Ramp aircraft"+at);
        calls+=effects.size();++phases;
    }
    require(phases==2525 && poses==134 && calls==8,"Incomplete original three-lap flyover trace");
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        srf::CourseEnvironment environment(rom,srf::CourseId::sky_ramp);
        srf::CourseScenery scenery(rom,srf::CourseId::sky_ramp);
        require(environment.triggers().size()==3 && scenery.poses().size()==2 &&
                scenery.poses()[0].visual_descriptor==0xad7c && scenery.poses()[1].visual_descriptor==0xb7fc &&
                scenery.poses()[1].position==srf::Vertex{2752,-3552,-16448},"Unexpected Sky Ramp scenery catalog");
        replay(environment,scenery);environment.reset();scenery.reset();replay(environment,scenery);
        std::cout<<"2,525 original Sky Ramp cloud phases, 134 aircraft callbacks and restart match\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
