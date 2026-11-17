#include "srf/white_land_breakables.hpp"
#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void impacts(const srf::Rom& rom) {
    const auto scene=srf::extract_course_scene(rom,srf::CourseId::white_land);
    const auto shapes=srf::extract_collision(rom,scene);
    const auto props=srf::extract_props(rom,scene);
    const auto road=std::find_if(shapes.begin(),shapes.end(),[](const auto& shape){
        return shape.position==srf::RacePosition{19072,0,4672};
    });
    check(road!=shapes.end(),"Missing original White Land road");
    std::vector<std::uint32_t> order;
    for(const auto& p:props)if(p.callback_address==0x09a06a) {
        check(p.state.word(0x0c)==80 && p.state.word(0x0e)==80 && p.state.word(0x10)==80 &&
              p.state.word(0x16)==24 && p.state.word(0x1a)==1060 && p.state.word(0x2a)==512 &&
              p.state.word(0x20)==std::uint16_t(-120),"Original ice solid definition changed");
        order.push_back(p.command_offset);
    }
    check(order.size()==6,"Missing original breakable placements");
    order.push_back(road->command_offset);
    std::array<srf::Vehicle,4> cars{};
    srf::WhiteLandBreakables ice(rom);
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"white_land_impact_reference.txt");
    check(bool(input),"Missing White Land original impact reference");
    std::string line;unsigned steps=0,draws=0,hits=0;
    std::vector<std::uint32_t> parents;
    constexpr std::array<unsigned,6> motion{0x1e,0x20,0x22,0x2e,0x30,0x32};
    constexpr std::array<unsigned,4> diameters{224,156,184,148};
    while(std::getline(input,line)) {
        if(line.empty() || line[0]=='#')continue;
        std::istringstream row(line);char type;row>>type;
        if(type=='P') {
            unsigned phase,id,present;int x,y,z,vx,vz;
            row>>phase>>id>>present>>x>>y>>z>>vx>>vz;
            auto& car=cars.back().body;
            car.word(0x16)=present?18:0x400;car.word(0x64)=1;car.word(0x2a)=4096;
            car.word(0xc)=400;car.word(0xe)=240;car.word(0x10)=400;
            car.word(0x1e)=x;car.word(0x20)=y;car.word(0x22)=z;
            car.word(0x2e)=vx;car.word(0x32)=vz;
            ice.begin_phase();
            for(const auto& p:props)if(p.callback_address==0x09a06a)
                ice.update_solid(p,true,p.command_offset==id);
            if(id) {
                ++hits;
                for(unsigned i=0;i<4;++i)parents.push_back(id);
                check(ice.effects().size()==1 && ice.effects()[0]==0x10,"Missing original impact sound");
            } else check(ice.effects().empty(),"Unexpected repeated impact");
            ice.submit_fragments();
            ice.integrate(std::span(&*road,1),order,props,cars);
        } else if(type=='F') {
            unsigned slot,draw;row>>slot;
            const auto* body=ice.body(0x82000000u+slot);
            check(body!=nullptr,"Original child is missing");
            for(auto field:motion) {
                int expected;row>>expected;
                if(body->word(field)!=std::uint16_t(expected)) {
                    std::cerr<<"Step "<<steps<<" slot "<<slot<<" field "<<field<<" wanted "<<expected
                             <<" got "<<std::int16_t(body->word(field))<<'\n';
                    throw std::runtime_error("Original ice fragment trajectory differs");
                }
            }
            srf::Vertex p;row>>draw>>p.x>>p.y>>p.z;
            const auto sprites=ice.sprites();
            const auto sprite=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s){
                return s.table==0x996b && s.command_offset==parents.at(slot) && s.diameter==diameters[slot%4];
            });
            check(draw==(sprite!=sprites.end()),"Original fragment visibility differs");
            if(draw) {check(sprite->position==p,"Original CPU fragment position differs");++draws;}
            ++steps;
        } else throw std::runtime_error("Unknown impact fixture row");
        check(bool(row),"Truncated original impact fixture");
    }
    check(hits==2 && steps==128 && draws==112,"Incomplete original impacts");
    ice.begin_phase();
    check(!ice.body(0x82000007),"Fragment survived both original cleanup phases");
    ice.reset();
    check(ice.effects().empty() && ice.sprites().empty() && !ice.solid_retired(parents.front()),
          "Restart retained destroyed ice or fragments");
}
void drive_into_ice(const srf::Rom& rom) {
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::white_land);
    std::vector<std::vector<srf::ScenerySprite>> recorded;
    for(unsigned run=0;run<2;++run) {
        race.reset();srf::RaceAudioState audio{};
        std::ifstream in(std::filesystem::path(SRF_TEST_FIXTURES)/"white_land_breakable_inputs.csv");
        std::string line;std::getline(in,line);
        unsigned hits=0,draws=0;
        while(std::getline(in,line)) {
            std::replace(line.begin(),line.end(),',',' ');
            unsigned phase,buttons,ticks;std::istringstream row(line);row>>phase>>buttons>>ticks;
            check(bool(row) && phase==race.phase() && !race.ended(),"Invalid ordinary-input collision replay");
            srf::RaceAudioOutput events;
            const auto controls=srf::prepare_player_input(audio,race.racers()[3].body,buttons,race.countdown_active(),events);
            race.step(controls,ticks);
            for(auto effect:race.breakable_effects()) {
                check(effect==0x10 && race.phase()==273,"Unexpected ice destruction sound");
                ++hits;
            }
            const auto sprites=race.breakable_sprites();
            const auto fragments=std::count_if(sprites.begin(),sprites.end(),[](const auto& s){return s.table==0x996b;});
            check(fragments==(race.phase()>=273 && race.phase()<287?4:0),"Fragment visibility changed after impact");
            draws+=static_cast<unsigned>(fragments);
            if(!run)recorded.emplace_back(sprites.begin(),sprites.end());
            else check(std::ranges::equal(recorded.at(phase),sprites),"Impact sprite sequence differed after restart");
        }
        check(hits==1 && draws==56 && race.phase()==291,"Incomplete impact replay");
    }
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);impacts(rom);drive_into_ice(rom);
        std::cout<<"Two original impacts, 128 fragment updates and 112 submitted sprites match\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
