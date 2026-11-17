#include "srf/environment.hpp"
#include "srf/scenery.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const std::string& reason) { if(!ok)throw std::runtime_error(reason); }
std::ifstream fixture(const char* name) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/name);
    std::string header;require(bool(std::getline(input,header)),"Missing original Big Ravine trace");
    return input;
}
void replay(srf::CourseEnvironment& environment,const char* name,bool targeted=false) {
    auto input=fixture(name);
    srf::VehicleState player;
    player.word(0x16)=0x12;player.word(0x64)=1;
    player.word(0x0c)=player.word(0x10)=400;player.word(0x0e)=240;
    unsigned phases=0,commands=0,thunder=0,impacts=0;
    std::string line;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,ticks,x,y,z,speed,lap,centis,flags,weight,command,effect;
        require(bool(row>>phase>>ticks>>x>>y>>z>>speed>>lap>>centis>>flags>>weight>>command>>effect),
                "Malformed Big Ravine race trace");
        player.word(0x1e)=x;player.word(0x20)=y;player.word(0x22)=z;player.word(0x4a)=speed;
        environment.advance_video(ticks);environment.step(player,lap,centis);
        const auto& events=environment.events();
        const auto at=" at original phase "+std::to_string(phase);
        require((environment.state().flags&0x68)==(flags&0x68) &&
            environment.presentation().palette_weight==weight,"Backdrop or dusk mismatch"+at);
        require(events.sound_commands.size()==unsigned(command!=0) &&
            (!command || events.sound_commands.front()==command),"Echo or bridge ambience mismatch"+at);
        require(events.effects.size()==unsigned(effect!=0) &&
            (!effect || events.effects.front()==effect),"Thunder or roadside impact mismatch"+at);
        ++phases;commands+=events.sound_commands.size();thunder+=effect==0x36;impacts+=effect==0x5a;
    }
    if(targeted)require(phases==60 && commands==0 && thunder==1 && impacts==3,"Incomplete roadside impact trace");
    else require(phases==2618 && commands==40 && thunder==2 && impacts==0,"Incomplete three-lap Big Ravine trace");
}
void lightning() {
    auto input=fixture("big_ravine_lightning_reference.csv");
    srf::EnvironmentState state;srf::EnvironmentVideoState video;
    std::string line;int previous=-1,count=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int frame,flags,level,tick,step,pairs,r,g,b,color,mode,window;
        require(bool(row>>frame>>flags>>level>>tick>>step>>pairs>>r>>g>>b>>color>>mode>>window),
                "Malformed Big Ravine lightning trace");
        if(frame!=previous+1) {
            state.flags=static_cast<std::uint8_t>(flags|0x10);
            video={};video.fixed_rgb555=color;video.color_mode=mode;
        }
        srf::advance_environment_video(state,video,0,1);
        require(state.flags==flags && video.flash_level==level && video.flash_tick==tick &&
                video.flash_step==step && video.flash_pairs==pairs &&
                video.pending_color==std::array<std::uint8_t,3>{std::uint8_t(r),std::uint8_t(g),std::uint8_t(b)} &&
                video.active_color_mode==mode && video.fixed_rgb555==color && video.visible_flash_window==bool(window),
                "Lightning differs from original CPU or PPU at video frame "+std::to_string(frame));
        previous=frame;++count;
    }
    require(count==64,"Incomplete lightning capture");
    srf::advance_environment_video(state,video,0,2);
    require(!video.visible_flash_window && video.active_color_mode==4,"Lightning window remained open");
}
}
int wmain() {
    try {
        const auto input=_wgetenv(L"SRF_ROM");if(!input)return 77;
        const auto rom=srf::Rom::load(input);
        srf::CourseEnvironment environment(rom,srf::CourseId::big_ravine);
        srf::CourseScenery scenery(rom,srf::CourseId::big_ravine);
        require(scenery.poses().size()==1 && scenery.poses()[0].visual_descriptor==0xb7fc &&
            scenery.poses()[0].position==srf::Vertex{832,-4704,21952},"Missing Big Ravine checkpoint panel");
        replay(environment,"big_ravine_environment_reference.csv");environment.reset();
        replay(environment,"big_ravine_environment_reference.csv");environment.reset();
        replay(environment,"big_ravine_roadside_reference.csv",true);lightning();
        std::cout<<"2,618 original Big Ravine race phases, 3 targeted roadside impacts, 64 lightning frames and restart match\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
