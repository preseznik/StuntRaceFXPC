#include "srf/sky_ramp_aircraft.hpp"
#include "srf/presentation.hpp"
#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool ok,const std::string& reason,unsigned phase) {
    if(!ok)throw std::runtime_error(reason+" at original phase "+std::to_string(phase));
}
void replay(const srf::Rom& rom) {
    const auto scene=srf::extract_course_scene(rom,srf::CourseId::sky_ramp);
    const auto mask=srf::extract_streaming(rom,scene).normal;
    const auto trigger=std::ranges::find(scene.objects,0x099bd7u,&srf::SceneObject::callback_address);
    check(trigger!=scene.objects.end(),"Missing Sky Ramp gate",0);
    srf::SkyRampAircraft aircraft(rom);
    srf::VehicleState player;
    player.word(0x16)=0x12;player.word(0x64)=1;
    player.word(0xc)=player.word(0x10)=400;player.word(0xe)=240;
    const auto descriptor=srf::Rom::lorom_offset(0x00a158);
    const auto diameter=srf::original_sprite_diameter(rom.u16(descriptor+16),rom.bytes()[descriptor+7],100);
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"sky_ramp_aircraft_reference.txt");
    std::string line;std::getline(file,line);
    unsigned rows=0,spawns=0,hits=0,sensors=0,updates=0,draws=0,smoke=0;
    int last=-1;srf::Vertex old{};
    while(std::getline(file,line)) {
        std::istringstream in(line);
        int scenario;unsigned phase,spawn,sound,hit,attached; srf::Vertex p{};
        in>>scenario>>phase>>p.x>>p.y>>p.z>>spawn>>sound>>hit>>attached;
        if(scenario!=last){aircraft.reset();last=scenario;old=p;}
        if(spawn)aircraft.spawn(trigger->command_offset,trigger->position);
        aircraft.step(srf::activation_cells(mask,std::int16_t(old.x),std::int16_t(old.z)));
        old=p;
        player.word(0x1e)=std::uint16_t(p.x);player.word(0x20)=std::uint16_t(p.y);player.word(0x22)=std::uint16_t(p.z);
        aircraft.record_contact(player);
        auto effects=aircraft.effects();
        check(std::ranges::count(effects,0x0e)==sound && std::ranges::count(effects,0x59)==hit &&
              effects.size()==sound+hit,"Aircraft effects diverged",phase);
        spawns+=sound;hits+=hit;
        if(attached) {
            const auto* b=aircraft.body();check(b,"Missing original 800-unit contact sensor",phase);
            for(unsigned f:{0x1e,0x20,0x22,0x0c,0x0e,0x10,0x16,0x1a}) {
                int value;in>>value;check(b->word(f)==std::uint16_t(value),"Aircraft sensor changed",phase);
            }
            ++sensors;
        }
        unsigned update,draw;in>>update;
        const auto compare_pose=[&](srf::Vertex v,unsigned pitch,unsigned yaw,unsigned roll) {
            const auto models=aircraft.poses();
            check(models.size()==1 && models[0].visible && models[0].visual_descriptor==0xaa34 &&
                  models[0].position==v && models[0].pitch==pitch && models[0].yaw==yaw && models[0].roll==roll,
                  "Aircraft mesh pose changed",phase);
        };
        if(update) {
            srf::Vertex v;unsigned pitch,yaw,roll,life;in>>v.x>>v.y>>v.z>>pitch>>yaw>>roll>>life;
            if(life)compare_pose(v,pitch,yaw,roll);else check(aircraft.poses().empty(),"Expired plane remained visible",phase);
            if(!attached)check(!aircraft.body(),"Unallocated GSU sensor returned",phase);
            ++updates;
        }
        in>>draw;
        if(draw) {
            srf::Vertex v;unsigned pitch,yaw,roll;in>>v.x>>v.y>>v.z>>pitch>>yaw>>roll;
            compare_pose(v,pitch,yaw,roll);++draws;
        }
        unsigned count;in>>count;
        for(unsigned n=0;n<count;++n) {
            srf::Vertex v;unsigned frame;in>>v.x>>v.y>>v.z>>frame;
            check(std::ranges::any_of(aircraft.sprites(),[&](const auto& s){
                return s.position==v && s.table==0x997c && s.frame==frame && s.diameter==diameter;
            }),"Missing original aircraft smoke",phase);
        }
        check(count==aircraft.sprites().size(),"Unexpected aircraft smoke",phase);
        smoke+=count;
        check(bool(in),"Malformed original aircraft fixture",phase);++rows;
    }
    check(rows==92 && spawns==2 && hits==1 && sensors==45 && updates==46 && draws==43 && smoke==174,
          "Incomplete original aircraft comparison",0);
}
void two_button_strike(const srf::Rom& rom) {
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::sky_ramp);
    for(unsigned attempt=0;attempt<2;++attempt) {
        race.reset();srf::RaceAudioState audio;
        std::ifstream in(std::filesystem::path(SRF_TEST_FIXTURES)/"sky_ramp_clean_inputs.csv");
        std::string line;std::getline(in,line);
        unsigned hits=0,smoke=0,visible_phases=0;
        while(race.phase()<1208 && std::getline(in,line)) {
            std::replace(line.begin(),line.end(),',',' ');
            std::istringstream row(line);unsigned phase,buttons,ticks;
            check(bool(row>>phase>>buttons>>ticks),"Missing Sky Ramp button input",race.phase());
            if(phase==1130 || phase==1131)buttons=0x12;
            srf::RaceAudioOutput events;
            race.step(srf::prepare_player_input(audio,race.racers()[3].body,buttons,race.countdown_active(),events),ticks);
            for(auto sound:race.bonus_effects())if(sound==0x59) {
                check(race.phase()==1166,"Native aircraft hit on unexpected phase",race.phase());++hits;
            }
            const auto particles=race.spray_sprites();
            visible_phases+=!particles.empty();smoke+=static_cast<unsigned>(particles.size());
            for(const auto& p:particles)check(p.table==0x997c && p.frame<6,"Invalid aircraft smoke",race.phase());
        }
        check(race.phase()==1208 && hits==1 && smoke==174 && visible_phases==31,
              "Two-button aircraft strike or restart changed",race.phase());
    }
}
}
int wmain() {
    try {
        const auto file=_wgetenv(L"SRF_ROM");if(!file)return 77;
        const auto rom=srf::Rom::load(file);replay(rom);two_button_strike(rom);
        std::cout<<"Sky Ramp aircraft, contact sensor, strike, smoke and retirement match both original drives\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
