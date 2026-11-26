#include "srf/local_race.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void check(const srf::Rom& rom,srf::LocalCourse course){
    using namespace srf;
    const bool battle=course.index!=practice_course.index;
    LocalRace race(rom,course,PlayerCar::four_wd,PlayerCar::coupe,false);
    require(race.count()==(battle?2u:1u),"Participant count");
    const auto world=extract_world(rom,race.scene());
    require(!world.instances.empty() && !world.models.empty(),"Empty local course");
    require(!extract_local_sprites(rom,course.index).empty(),"Missing course sprites");
    extract_local_backdrop(rom,course.index);
    extract_local_audio(rom,course.index);
    const auto initial=race.car(0).body;
    auto drive=[&]{
        for(unsigned n=0;n<36;++n)race.step({0x10,0});
        require(!race.result() && race.car(0).body.word(0x4a)>0,"P1 cannot accelerate");
        if(battle)require(race.car(1).body.word(0x4a)<race.car(0).body.word(0x4a),"P2 copied P1 input");
        return race.car(0).body;
    };
    const auto moved=drive();
    race.reset();require(race.phase()==0 && race.car(0).body.word(0x1e)==initial.word(0x1e),"Restart grid");
    require(drive().word(0x22)==moved.word(0x22),"Deterministic restart");
    if(!battle){
        race.reset();
        const auto route=extract_route(race.scene());
        for(unsigned step=0;step<9000 && !race.result();++step){
            const auto& body=race.car(0).body;
            const auto& target=route.at(body.word(0x58)%route.size()).position;
            const double angle=std::atan2(double(target[0])-std::int16_t(body.word(0x1e)),double(target[2])-std::int16_t(body.word(0x22)));
            const auto error=std::int16_t(int(angle*65536/6.28318530718)+body.word(0x24));
            auto buttons=std::uint16_t(error>600?2:error< -600?1:0);
            if(body.word(0x4a)<(std::abs(int(error))>4800?145:275))buttons|=0x10;
            race.step({buttons,0});
        }
        std::cout<<"Practice phase "<<race.phase()<<" lap "<<race.car(0).body.word(0x7a)<<" point "<<race.car(0).body.word(0x58)<<std::endl;
        require(race.result()==0,"Practice did not finish three laps");
    }
    if(battle){
        race.reset();
        for(unsigned i=0;i<36;++i)race.step({0,0x10});
        require(race.car(1).body.word(0x4a)>race.car(0).body.word(0x4a),"P2 cannot accelerate independently");
        LocalRace solo(rom,course,PlayerCar::four_wd,PlayerCar::coupe,true);
        for(unsigned i=0;i<10000 && !solo.result();++i)solo.step({0x40,0});
        std::cout<<"Course "<<course.index<<" phase "<<solo.phase()<<" CPU laps "<<solo.car(1).body.word(0x7a)<<std::endl;
        require(solo.result()==1 && solo.car(1).body.word(0x7a)==3,"CPU did not complete three laps");
        LocalRace unattended(rom,course,PlayerCar::four_wd,PlayerCar::coupe,false);
        for(unsigned i=0;i<5000 && !unattended.result();++i)unattended.step({});
        require(unattended.computer(0) && unattended.computer(1) && unattended.result().has_value(),
                "Unattended Battle did not finish");
        const auto finish=solo.phase();
        solo.step({0x40,0});require(solo.phase()==finish,"Finished race advanced");
        solo.reset();
        for(unsigned i=0;i<finish;++i)solo.step({0x40,0});
        require(solo.result()==1 && solo.phase()==finish,"Restart did not reproduce the finish");
    }
}
}
int wmain(){
    try{
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        for(const auto course:srf::battle_courses)check(rom,course);
        check(rom,srf::practice_course);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
