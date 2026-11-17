#include "srf/environment.hpp"
#include "srf/scenery.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const std::string& reason) {if(!ok)throw std::runtime_error(reason);}
void water_probe(srf::CourseEnvironment& environment,srf::VehicleState player) {
    // This original capture changes car height only, below the lower roadway.
    // The ordinary race in replay() crosses the same X/Z twice at different heights.
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"harbor_city_water_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing Harbor City water probe");
    unsigned count=0,commands=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,x,y,z,flags,weight,target,sound;
        require(bool(row>>phase>>x>>y>>z>>flags>>weight>>target>>sound),"Malformed Harbor City water probe");
        player.word(0x1e)=x;player.word(0x20)=y;player.word(0x22)=z;
        environment.step(player);
        const auto& signals=environment.events().sound_commands;
        const auto video=environment.presentation();
        require(environment.state().flags==flags && video.fog_weight==weight && video.fog_rgb555==target &&
                signals.size()==unsigned(sound!=0) && (signals.empty() || signals[0]==sound),
                "Harbor City water response differs at original phase "+std::to_string(phase));
        ++count;commands+=signals.size();
    }
    require(count==25 && commands==2,"Incomplete Harbor City water probe");
}
void replay(srf::CourseEnvironment& environment,srf::CourseScenery& scenery) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"harbor_city_environment_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing original Harbor City trace");
    srf::VehicleState player;
    player.word(0x16)=0x12;player.word(0x64)=1;
    player.word(0x0c)=player.word(0x10)=400;player.word(0x0e)=240;
    unsigned phases=0,commands=0,panels=0,ramping=0;
    std::optional<srf::CourseEnvironment> water;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,x,y,z,lap,flags,weight,fog,target,cursor,sound,pitch;
        require(bool(row>>phase>>x>>y>>z>>lap>>flags>>weight>>fog>>target>>cursor>>sound>>pitch),"Invalid Harbor City row");
        player.word(0x1e)=x;player.word(0x20)=y;player.word(0x22)=z;
        environment.step(player,lap);scenery.step(player,lap);
        if(phase==636)water=environment;
        const auto where=" differs from original at phase "+std::to_string(phase);
        const auto video=environment.presentation();
        require(environment.state().flags==flags && video.palette_weight==weight &&
                video.fog_weight==fog && video.fog_rgb555==target,"Harbor City palette"+where);
        require((video.skyline_phase+1)%22==cursor,"Harbor City skyline cursor"+where);
        const auto& signals=environment.events().sound_commands;
        require(signals.size()==unsigned(sound!=0) && (signals.empty() || signals[0]==sound),"Harbor City water command"+where);
        if(pitch>=0) {
            require(scenery.poses()[0].pitch==pitch,"Harbor City checkpoint panel"+where);
            ++panels;
        }
        ++phases;commands+=signals.size();ramping+=(flags&0x20)!=0;
    }
    require(phases==2500 && panels==409 && commands==1 && ramping==248,"Incomplete Harbor City race trace");
    require(water.has_value(),"Missing Harbor City water branch point");
    water_probe(*water,player);
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        srf::CourseEnvironment environment(rom,srf::CourseId::harbor_city);
        srf::CourseScenery scenery(rom,srf::CourseId::harbor_city);
        require(environment.triggers().size()==2 && scenery.poses().size()==1 &&
                scenery.poses()[0].position==srf::Vertex{21952,-3040,5632} &&
                scenery.poses()[0].visual_descriptor==0xb7fc,"Missing original Harbor City checkpoint panel or gates");
        replay(environment,scenery);environment.reset();scenery.reset();replay(environment,scenery);
        std::cout<<"Harbor City original three-lap weather, skyline cursor, checkpoint panel and restart match\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
