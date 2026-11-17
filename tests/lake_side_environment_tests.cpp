#include "srf/environment.hpp"
#include "srf/scenery.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const std::string& reason) {if(!ok)throw std::runtime_error(reason);}
void replay(srf::CourseEnvironment& environment,srf::CourseScenery& scenery) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"lake_side_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing original Lake Side trace");
    srf::VehicleState player;
    player.word(0x16)=0x12;player.word(0x64)=1;
    player.word(0x0c)=player.word(0x10)=400;player.word(0x0e)=240;
    unsigned phases=0,calls=0,commands=0,effects=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,x,y,z,lap,flags,weight,command,called,model,child_x,yaw,frame,effect;
        require(bool(row>>phase>>x>>y>>z>>lap>>flags>>weight>>command>>called>>model>>child_x>>yaw>>frame>>effect),
                "Malformed Lake Side trace");
        player.word(0x1e)=x;player.word(0x20)=y;player.word(0x22)=z;
        environment.step(player,lap);scenery.step(player,lap);
        const auto msg=" differs from original at phase "+std::to_string(phase);
        const auto& sounds=environment.events().sound_commands;
        const auto video=environment.presentation();
        require(environment.state().flags==flags && video.fog_weight==weight &&
                (!flags || video.fog_rgb555==0x0800),"Lake Side water fade"+msg);
        require(sounds.size()==unsigned(command!=0) && (sounds.empty() || sounds[0]==command),"Lake Side ambience"+msg);
        const auto pose=scenery.poses().back();
        require(pose.visible==bool(called && model==0xb754),"Lake Side actor visibility"+msg);
        if(called) {
            require(pose.position==srf::Vertex{child_x,160,-16448} && pose.yaw==yaw &&
                    pose.frame==frame,"Lake Side actor pose"+msg);
            ++calls;
        }
        const auto sounds2=scenery.effects();
        require(sounds2.size()==unsigned(effect!=0) && (sounds2.empty() || sounds2[0]==effect),"Lake Side call"+msg);
        ++phases;commands+=sounds.size();effects+=sounds2.size();
    }
    require(phases==2011 && calls==257 && commands==19 && effects==2,"Incomplete original Lake Side trace");
}
}
int wmain() {
    try {
        const auto input=_wgetenv(L"SRF_ROM");if(!input)return 77;
        const auto rom=srf::Rom::load(input);
        srf::CourseEnvironment environment(rom,srf::CourseId::lake_side);
        srf::CourseScenery scenery(rom,srf::CourseId::lake_side);
        require(scenery.poses().size()==2 && scenery.poses()[0].visual_descriptor==0xb7fc &&
                scenery.poses()[0].position==srf::Vertex{-14528,-3744,-10688},"Missing Lake Side checkpoint panel");
        replay(environment,scenery);environment.reset();scenery.reset();replay(environment,scenery);
        std::cout<<"2,011 original Lake Side environmental phases and 257 actor poses match, including restart\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
