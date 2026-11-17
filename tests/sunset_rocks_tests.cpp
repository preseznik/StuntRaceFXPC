#include "srf/sunset_rocks.hpp"
#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void original_trajectory(const srf::Rom& rom,srf::CourseId course) {
    const bool snow=course==srf::CourseId::white_land;
    const bool ravine=course==srf::CourseId::big_ravine;
    const auto shapes=srf::extract_collision(rom,srf::extract_course_scene(rom,course));
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/(snow?"white_land_snowball_contact.txt":ravine?"big_ravine_rocks_reference.txt":"sunset_rocks_reference.txt"));
    std::string line;
    while(input.peek()=='#')std::getline(input,line);
    unsigned frame,phase,slot,steps=0,hits=0;
    while(input>>frame>>phase>>slot) {
        srf::VehicleState rock{};rock.word(0x2a)=snow?0x0800:0x4600;
        std::array<std::int16_t,6> expected{};
        constexpr std::array<unsigned,6> fields{0x1e,0x20,0x22,0x2e,0x30,0x32};
        for(auto field:fields){int value;input>>value;rock.word(field)=std::uint16_t(value);}
        for(auto& value:expected)input>>value;
        int magnitude;bool hit;unsigned count;input>>magnitude>>hit>>count;
        std::vector<srf::VehicleState> bodies(count);
        std::vector<srf::RockContact> contacts;
        for(unsigned i=0;i<count;++i) {
            unsigned flags,mass,controller;srf::RacePosition p{};
            int vx,vz,x,z;input>>flags>>mass>>controller>>p[0]>>p[1]>>p[2]>>vx>>vz>>x>>z;
            if(flags&0x10) {
                auto& target=bodies[i];target.word(0x16)=flags;target.word(0x2a)=mass;target.word(0x64)=controller;
                contacts.push_back(srf::BodyContactCandidate{{std::uint16_t(x),std::uint16_t(z)},
                                      {std::uint16_t(vx),std::uint16_t(vz)},&target});
            } else if(flags!=8) {
                // Flag 8 debris has no collision mesh; 04:9C3D returns zero.
                const auto match=std::find_if(shapes.begin(),shapes.end(),[&](const auto& s){return s.position==p;});
                require(match!=shapes.end(),"Missing original contacted surface");contacts.push_back(&*match);
            }
        }
        require(bool(input),"Truncated original rock trajectory");
        const auto result=srf::advance_sunset_rock(rock,contacts);
        if(result.magnitude!=magnitude || result.player_hit!=hit)
            throw std::runtime_error("Original rock contact diverged at "+std::to_string(phase)+
                " on "+std::to_string(slot)+": got "+std::to_string(result.magnitude)+
                " expected "+std::to_string(magnitude));
        for(unsigned i=0;i<fields.size();++i)
            require(rock.word(fields[i])==std::uint16_t(expected[i]),"Original rock pose/velocity diverged");
        ++steps;hits+=hit;
    }
    require(steps==(snow?51:ravine?91:50) && hits==(ravine?2:1),"Incomplete original rock comparison");
}
void ravine_rock_chips(const srf::Rom& rom) {
    const auto shapes=srf::extract_collision(rom,srf::extract_course_scene(rom,srf::CourseId::big_ravine));
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"big_ravine_chip_reference.txt");
    std::string line;
    while(input.peek()=='#')std::getline(input,line);
    srf::WhiteLandBreakables chips(rom);
    std::vector<std::uint8_t> rendered;
    unsigned phase,age,render_mask,steps=0,original_draws=0;
    while(input>>phase>>age>>render_mask) {
        srf::RacePosition origin{};
        for(auto& v:origin)input>>v;
        if(age==1){chips.reset();chips.spawn_rock_chips(9,origin);}
        chips.begin_phase();chips.submit_fragments();
        require(chips.meshes().size()==(age<=14?8u:0u),"Incorrect rock chip visibility");
        for(unsigned i=0;i<8;++i)if(render_mask&(1u<<i)) {
            const auto& state=*chips.body(chips.first_body_id+i);
            const auto& pose=chips.meshes()[i];
            const auto angle=std::uint8_t(age*24);
            require(pose.position==srf::Vertex{static_cast<std::int16_t>(state.word(0x1e)),
                                               static_cast<std::int16_t>(state.word(0x20)),
                                               static_cast<std::int16_t>(state.word(0x22))} &&
                    pose.visual_descriptor==0xabf4 && pose.visible && pose.yaw==angle &&
                    pose.pitch==angle && pose.roll==angle,"Rock chip render pose diverged");
            for(int coordinate:{pose.position.x,pose.position.y,pose.position.z}) {
                const auto word=std::uint16_t(coordinate);
                rendered.push_back(std::uint8_t(word));rendered.push_back(std::uint8_t(word>>8));
            }
            rendered.insert(rendered.end(),{pose.pitch,pose.yaw,pose.roll});
            ++original_draws;
        }
        std::array<srf::Vehicle,4> cars{};
        srf::VehicleState rock{};
        std::array<const srf::VehicleState*,1> rock_ptr{};
        for(auto* body:{&rock,&cars.back().body}) {
            bool present;input>>present;
            if(present) {
                for(auto offset:{0x16,0x2a,0x64,0xc,0xe,0x10,0x1e,0x20,0x22,0x2e,0x32})input>>body->word(offset);
                if(body==&rock)rock_ptr[0]=body;
            }
        }
        std::vector<std::uint32_t> road_ids;
        unsigned count;input>>count;
        for(unsigned i=0;i<count;++i) {
            int x;input>>x;
            const auto found=std::find_if(shapes.begin(),shapes.end(),[=](const auto& s) {
                return s.position[0]==x && s.position[2]==4672;
            });
            require(found!=shapes.end(),"Original rock chip terrain missing");
            road_ids.push_back(found->command_offset);
        }
        chips.integrate(shapes,road_ids,{},cars,rock_ptr);
        for(unsigned i=0;i<8;++i) {
            const auto& state=*chips.body(chips.first_body_id+i);
            for(unsigned f:{0x1e,0x20,0x22,0x2e,0x30,0x32}) {
                std::uint16_t expected;input>>expected;
                if(state.word(f)!=expected)throw std::runtime_error("Rock chip differs at phase "+
                    std::to_string(phase)+", slot "+std::to_string(i)+", field "+std::to_string(f)+
                    ": expected "+std::to_string(expected)+", got "+std::to_string(state.word(f)));
            }
            ++steps;
        }
        require(bool(input),"Incomplete Big Ravine rock chip fixture");
    }
    chips.begin_phase();
    require(srf::sha256(rendered)=="23378f220bda51bf4e769d2fd6bdb8a74e17fb251746648009b99e41387c3121",
            "Original Big Ravine ABF4 render queue mismatch");
    require(steps==256 && original_draws==217 && !chips.body(chips.first_body_id),"Incomplete original debris comparison");
}
void lifecycle(const srf::Rom& rom) {
    srf::SunsetRocks rocks(srf::extract_course_scene(rom,srf::CourseId::sunset_valley));
    require(rocks.trigger_id()==0x29de9,"Original rock trigger missing");
    auto p=rocks.trigger_position();
    std::array<srf::Vehicle,4> cars{};auto& player=cars.back().body;
    player.word(0x16)=0x12;player.word(0x64)=1;
    for(unsigned axis=0;axis<3;++axis)player.word(0x1e +2*axis)=std::uint16_t(p[axis]);
    for(unsigned lap:{0u,0u,1u,1u}){rocks.cpu_step(true,player,lap);require(rocks.poses().empty(),"Premature rockfall");}
    rocks.cpu_step(true,player,2);
    require(rocks.effects().size()==1 && rocks.effects()[0]==0x53 && rocks.poses().size()==2,"Rockfall spawn");
    require(rocks.poses()[0].position==srf::Vertex{13312,-4640,26048} &&
            rocks.poses()[1].position==srf::Vertex{14272,-4800,25952},"Wrong source spawn offsets");
    for(unsigned age=1;age<28;++age) {
        rocks.integrate({},cars);rocks.cpu_step(true,player,2);
        if(age==21)require(rocks.poses()[0].pitch==8 && rocks.poses()[0].yaw==9 &&
                           rocks.poses()[0].roll==9,"Original rotation carry");
    }
    require(rocks.poses().empty() && rocks.effects().empty(),"Expired rock visible or sounded");
    for(unsigned phase=0;phase<2;++phase) {
        require(rocks.body(srf::SunsetRocks::first_body_id)!=nullptr,"Original solid retired early");
        rocks.integrate({},cars);rocks.cpu_step(true,player,2);
    }
    require(!rocks.body(srf::SunsetRocks::first_body_id),"Solid retained after GSU retirement");
    rocks.reset();
    rocks.cpu_step(true,player,2);require(rocks.effects().empty(),"Stale prior contact after restart");
    rocks.cpu_step(true,player,2);require(rocks.poses().size()==2,"Rockfall did not rearm on restart");
}
void snowball_poses(const srf::Rom& rom) {
    const auto scene=srf::extract_course_scene(rom,srf::CourseId::white_land);
    auto shapes=srf::extract_collision(rom,scene);
    std::reverse(shapes.begin(),shapes.end());
    const auto streaming=srf::extract_streaming(rom,scene);
    srf::SunsetRocks rocks(scene);
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"white_land_hazard_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing White Land snowball render capture");
    std::uint16_t old_x{},old_z{};unsigned compared=0,spawns=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        unsigned phase;row>>phase;
        std::array<srf::Vehicle,4> racers{};
        for(auto& vehicle:racers)
            for(unsigned offset:{0x64,0x16,0x1a,0x2a,0x1e,0x20,0x22,0x2e,0x32,0xc,0xe,0x10})
                require(bool(row>>vehicle.body.word(offset)),"Bad original White Land racer input");
        const auto& player=racers.back().body;
        const auto cells=srf::activation_cells(streaming.normal,static_cast<std::int16_t>(old_x),
                                               static_cast<std::int16_t>(old_z));
        old_x=player.word(0x1e);old_z=player.word(0x22);
        rocks.cpu_step(true,player,2,cells);
        spawns+=std::count(rocks.effects().begin(),rocks.effects().end(),0x11);
        unsigned count;require(bool(row>>count),"Missing White Land snowball count");
        for(unsigned slot=0;slot<count;++slot) {
            srf::Vertex p;unsigned pitch,yaw,roll;
            require(bool(row>>p.x>>p.y>>p.z>>pitch>>yaw>>roll),"Malformed snowball pose");
            const auto poses=rocks.poses();
            if(slot>=poses.size() || poses[slot].position!=p || poses[slot].pitch!=pitch ||
               poses[slot].yaw!=yaw || poses[slot].roll!=roll || poses[slot].visual_descriptor!=0xb6e4)
                throw std::runtime_error("White Land snowball differs at phase "+std::to_string(phase)+", slot "+
                   std::to_string(slot)+(slot>=poses.size()?" missing":" at "+std::to_string(poses[slot].position.x)+","+
                   std::to_string(poses[slot].position.y)+","+std::to_string(poses[slot].position.z)+
                   " angles "+std::to_string(poses[slot].pitch)+","+std::to_string(poses[slot].yaw)));
            ++compared;
        }
        rocks.integrate(shapes,racers);
    }
    require(compared==52 && spawns==1 && rocks.poses().empty(),"Incomplete original snowball lifecycle");
}
void deliberate_native_hit(const srf::Rom& rom) {
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::sunset_valley);
    srf::VehicleState previous{};
    for(unsigned run=0;run<2;++run) {
        race.reset();
        srf::RaceAudioState audio{};
        std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"sunset_valley_clean_inputs.csv");
        std::string line;require(bool(std::getline(input,line)),"Missing Sunset native driver");
        unsigned spawns=0,hits=0;
        while(std::getline(input,line)) {
            std::replace(line.begin(),line.end(),',',' ');
            unsigned phase,request,ticks;std::istringstream row(line);
            require(bool(row>>phase>>request>>ticks),"Malformed native input");
            // Two phases of throttle + right steer send the car into the falling rock.
            if(phase==1597 || phase==1598)request=18;
            srf::RaceAudioOutput events;
            const auto controls=srf::prepare_player_input(audio,race.racers()[3].body,
                std::uint16_t(request),race.countdown_active(),events);
            race.step(controls,std::uint8_t(ticks));
            for(const auto effect:race.rock_effects()) {
                if(effect==0x53)++spawns;
                if(effect==0x5a) {
                    ++hits;
                    require(phase==1697 && race.racers()[3].body.word(0x82)==23,
                            "Native rock hit timing or player response diverged");
                }
            }
            if(phase==1710)break;
        }
        require(spawns==1 && hits==1 && race.rock_poses().empty(),"Native rock lifecycle incomplete");
        if(run==0)previous=race.racers()[3].body;
        else require(previous.words==race.racers()[3].body.words,"Rock impact changed after restart");
    }
}
void hit_white_land_snowball(const srf::Rom& rom) {
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::white_land);
    for(unsigned run=0;run<2;++run) {
        race.reset();srf::RaceAudioState audio{};
        std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"white_land_clean_inputs.csv");
        std::string line;require(bool(std::getline(input,line)),"Missing White Land race fixture");
        unsigned hits=0;
        while(std::getline(input,line)) {
            std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
            unsigned phase,request,ticks;require(bool(row>>phase>>request>>ticks),"Malformed White Land input");
            if(phase>=1981 && phase<=1983)request=0x11; // Throttle + left for three original phases.
            srf::RaceAudioOutput events;
            race.step(srf::prepare_player_input(audio,race.racers()[3].body,request,race.countdown_active(),events),ticks);
            if(std::find(race.rock_effects().begin(),race.rock_effects().end(),0x56)!=race.rock_effects().end()) {
                require(race.phase()==2002,"Snowball impact phase changed");
                ++hits;
            }
            if(race.phase()>=2002 && race.phase()<=2016) {
                const auto sprites=race.breakable_sprites();
                const auto debris=std::count_if(sprites.begin(),sprites.end(),[](const auto& sprite) {
                    return sprite.command_offset==0x2cf07 && sprite.table==0x996b;
                });
                require(debris==(race.phase()==2016?0:4),"Snowball impact fragments changed lifetime");
            }
            if(race.phase()==2016)break;
        }
        require(hits==1,"White Land strike did not retire one snowball");
    }
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        original_trajectory(rom,srf::CourseId::sunset_valley);
        original_trajectory(rom,srf::CourseId::white_land);
        original_trajectory(rom,srf::CourseId::big_ravine);
        ravine_rock_chips(rom);
        snowball_poses(rom);
        hit_white_land_snowball(rom);
        lifecycle(rom);deliberate_native_hit(rom);
        std::cout<<"Original Sunset, White Land and Big Ravine trajectories, player strikes, retirement and restart passed\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
