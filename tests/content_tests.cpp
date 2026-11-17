#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"
#include "srf/rom.hpp"
#include "srf/world.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
bool equal(const std::array<srf::Vehicle,4>& a,const std::array<srf::Vehicle,4>& b) {
    for(unsigned i=0;i<4;++i) {
        if(a[i].body.words!=b[i].body.words)return false;
        for(unsigned j=0;j<4;++j)if(a[i].wheels[j].words!=b[i].wheels[j].words)return false;
    }
    return true;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        const auto path=argc==2?argv[1]:_wgetenv(L"SRF_ROM");
        if(!path) {std::cout<<"Set SRF_ROM to the user-supplied ROM\n";return 77;}
        const auto rom=srf::Rom::load(path);
        const std::array cars{srf::PlayerCar::coupe,srf::PlayerCar::four_wd,srf::PlayerCar::f_type};
        const std::array<unsigned,3> definitions{0x853d,0x8411,0x81b9};
        const std::array<unsigned,3> parameters{0x85b2,0x8486,0x822e};
        const std::array<unsigned,3> sound_offsets{0x98,0x130,0};
        for(const auto& course:srf::courses) {
        const auto scene=srf::extract_course_scene(rom,course.id);
        const auto world=srf::extract_world(rom,scene);
        const auto collision=srf::extract_collision(rom,scene);
        const auto clock=srf::extract_course_clock(rom,course.id);
        require(scene.points.size()==course.route_points,"Course route has wrong length");
        const bool night=course.id==srf::CourseId::night_owl;
        const bool forest=course.id==srf::CourseId::kings_forest;
        const bool sea=course.id==srf::CourseId::sea_breeze;
        const bool white=course.id==srf::CourseId::white_land;
        const bool cruise=course.id==srf::CourseId::night_cruise;
        const bool lake=course.id==srf::CourseId::lake_side;
        const bool ravine=course.id==srf::CourseId::big_ravine;
        const bool sky=course.id==srf::CourseId::sky_ramp;
        const bool harbor=course.id==srf::CourseId::harbor_city;
        const std::uint16_t lap_time=sky?50:night||ravine||harbor?40:lake?35:30;
        require(clock.initial_seconds==(white||lake?40:sea?30:night||forest||ravine?60:50) && clock.lap_seconds==std::array{lap_time,lap_time},
                "Original normal race clock mismatch");
        if(harbor) {
            require(scene.command_count==173 && scene.objects.size()==142 && world.instances.size()==142 &&
                    scene.end_offset==0x6bd71 && collision.size()==85 && clock.checkpoint_seconds==30,
                    "Harbor City scene or Master clock mismatch");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "483a7014ad3bedaff7467ced6a1c7e895107c0fe82d62bd0ee691582f659e3ac","Harbor City route mismatch");
        }
        if(sky) {
            require(scene.command_count==187 && scene.objects.size()==158 && scene.end_offset==0x6ad25 &&
                    collision.size()==89 && clock.checkpoint_seconds==20,"Sky Ramp scene or Master clock mismatch");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "9aed870a1783ce75d8a94df2cabc12fc8e9fe736cd4282a7107d6ff609c477ef","Sky Ramp route mismatch");
        }
        if(ravine) {
            require(scene.command_count==155 && scene.objects.size()==127 && scene.end_offset==0x69b5e &&
                    collision.size()==65 && clock.checkpoint_seconds==30,"Big Ravine scene or Master clock mismatch");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "4c6d0b0ca98704e75ac6f570667c670fca3b590b07d1585f46bc86732ae8994a","Big Ravine route mismatch");
        }
        if(lake) {
            require(scene.command_count==146 && scene.objects.size()==118 && scene.end_offset==0x68cf9 &&
                    collision.size()==58 && clock.checkpoint_seconds==20,"Lake Side scene or Master clock mismatch");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "6e29719fa9743364f7cd930351a9f3bc174a9d2f121a0146d0ebaff1c74339a7","Lake Side route mismatch");
        }
        if(cruise) {
            require(scene.command_count==144 && scene.objects.size()==109 && scene.end_offset==0x2e509 &&
                    collision.size()==58 && clock.checkpoint_seconds==20,"Night Cruise scene or clock mismatch");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "48b20a6c01ee5ea8eadc4fb8f1a58c8220fe05654744d1f5029a82562215ced2","Night Cruise original route mismatch");
        }
        if(white) {
            require(scene.command_count==144 && scene.objects.size()==118 && scene.end_offset==0x2d7f4 &&
                    collision.size()==54 && clock.checkpoint_seconds==30,"White Land scene or clock mismatch");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "780a518a1d88f0e6a0b036c2c4233662b334c783bfeabab964d011c8baa2828b","White Land original route mismatch");
        }
        if(sea) {
            require(scene.command_count==125 && scene.objects.size()==103 && scene.end_offset==0x2cada &&
                    world.instances.size()==103 && collision.size()==54 && clock.checkpoint_seconds==20,
                    "Sea Breeze scene or Expert round-two clock differs from original");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "6a5109b54c1b6780f7773b0ff6d77283637e3d006a598639e6547600999d3732",
                    "Sea Breeze route differs from original pointer 05:C8C8");
        }
        if(forest) {
            require(scene.command_count==147 && scene.objects.size()==118 && scene.end_offset==0x2bf92 &&
                    world.instances.size()==118 && collision.size()==43 && clock.checkpoint_seconds==20,
                    "King's Forest scene or Expert race clock differs from original");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "875feb3c02424a9c378516fb157ec0705ac9c7954768c34e765ad493afdb8922",
                    "King's Forest route differs from original loader 05:BD18");
            require(std::count_if(collision.begin(),collision.end(),[](const auto& shape) {
                return shape.descriptor_address==0xe73c && shape.animated_planes && shape.animated_planes->size()==5;
            })==2,"Both original animated bridge solids must be present");
            const auto props=srf::extract_props(rom,scene);
            require(std::count_if(props.begin(),props.end(),[](const auto& prop) {
                const auto& body=prop.state;
                return prop.callback_address==0x09e0ff && body.word(0xc)==80 && body.word(0xe)==80 &&
                    body.word(0x10)==80 && body.word(0x16)==0x18 && body.word(0x1a)==0x0424 &&
                    body.word(0x2a)==0x0800 && body.word(0x20)==static_cast<std::uint16_t>(-140);
            })==2,"Both tree trunks must match the original impact capture");
        }
        if(night) {
            require(scene.command_count==152 && scene.objects.size()==122 && scene.end_offset==0x2b285 &&
                    world.instances.size()==122 && collision.size()==47 && clock.checkpoint_seconds==20,
                    "Night Owl scene or clock differs from original");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "c5796e47a1f7e184ad7905a80d956fd0b722fca004458304dbdb928c97331d7e",
                    "Night Owl route differs from original loader 05:AFE3");
        }
        if(course.id==srf::CourseId::aqua_tunnel) {
            require(scene.command_count==160 && scene.objects.size()==132,"Aqua original scene inventory mismatch");
            require(collision.size()==57,"Aqua solid road/wall placement count mismatch");
            require(world.route_points.size()==82 && world.instances.size()==132,"Aqua world omitted route or objects");
        }
        if(course.id==srf::CourseId::sunset_valley) {
            require(scene.command_count==134 && scene.objects.size()==105 && scene.end_offset==0x2a4fc,
                    "Sunset Valley scene differs from the original scene table");
            require(world.instances.size()==105 && world.route_points.size()==80 && collision.size()==53,
                    "Sunset Valley world omitted route points, placements or solid surfaces");
            require(clock.checkpoint_seconds==20,"Sunset Valley checkpoint time differs from original");
            const auto props=srf::extract_props(rom,scene);
            const auto roadside=std::find_if(props.begin(),props.end(),
                [](const auto& prop){return prop.callback_address==0x09e097;});
            require(roadside!=props.end(),"Missing original Sunset roadside solid");
            const auto& solid=roadside->state;
            require(solid.word(0x16)==0x18 && solid.word(0x1a)==0x0424 && solid.word(0x2a)==0x1400 &&
                    solid.word(0x0c)==320 && solid.word(0x0e)==320 && solid.word(0x10)==320,
                    "Roadside solid differs from captured original GSU body");
            require(srf::sha256(rom.slice(scene.coordinate_offset,scene.end_offset-scene.coordinate_offset))==
                    "8b7ef432cde179ec4790dde568ebabb62bfdbe863da84fa31195eac1a38f5606",
                    "Sunset Valley route differs from the original loader's bank05:A27A data");
        }
        for(unsigned i=0;i<cars.size();++i) {
            srf::NativeRace race(rom,cars[i],course.id);
            require(race.course_id()==course.id,"Selected course lost");
            const auto initial=race.racers();
            const auto& body=initial[3].body;
            if(harbor) {
                const auto tuning=srf::extract_course_clock(rom,course.id,body.word(0x66));
                require(race.remaining_seconds()==50 && tuning.lap_seconds==std::array<std::uint16_t,2>{40,40},
                        "Harbor City car clock differs from the original");
                constexpr std::array x{-4608,-3328,-4288,-3648},z{2592,3552,4512,5312};
                for(unsigned slot=0;slot<4;++slot) {
                    const auto& racer=initial[slot].body;
                    require(racer.word(0x1e)==static_cast<std::uint16_t>(x[slot]) &&
                            racer.word(0x20)==static_cast<std::uint16_t>(-4480-(slot==3?160:480)) &&
                            racer.word(0x22)==z[slot] && !racer.word(0x24),"Harbor City original grid mismatch");
                }
            }
            if(sky) {
                const auto tuning=srf::extract_course_clock(rom,course.id,body.word(0x66));
                const auto lap=static_cast<std::uint16_t>(cars[i]==srf::PlayerCar::four_wd?50:40);
                require(race.remaining_seconds()==50 && tuning.lap_seconds==std::array{lap,lap},
                        "Sky Ramp car clock differs from ROM");
                constexpr std::array x{-8448,-7168,-8128,-7488},z{-1248,-288,672,1472};
                for(unsigned slot=0;slot<4;++slot) {
                    const auto& racer=initial[slot].body;
                    require(racer.word(0x1e)==static_cast<std::uint16_t>(x[slot]) &&
                            racer.word(0x22)==static_cast<std::uint16_t>(z[slot]) &&
                            racer.word(0x20)==static_cast<std::uint16_t>(-1536-(slot==3?160:480)) &&
                            !racer.word(0x24),"Sky Ramp original grid mismatch");
                }
            }
            if(ravine) {
                const auto tuning=srf::extract_course_clock(rom,course.id,body.word(0x66));
                require(race.remaining_seconds()==60 && tuning.lap_seconds==std::array<std::uint16_t,2>{40,40},
                        "Big Ravine car clock differs from ROM");
                constexpr std::array x{-3168,-2208,-1248,-448},z{-29568,-28288,-29248,-28608};
                for(unsigned slot=0;slot<4;++slot) {
                    const auto& racer=initial[slot].body;
                    require(racer.word(0x1e)==static_cast<std::uint16_t>(x[slot]) &&
                            racer.word(0x22)==static_cast<std::uint16_t>(z[slot]) &&
                            racer.word(0x20)==static_cast<std::uint16_t>(slot==3?-160:-480) &&
                            racer.word(0x24)==0xc000,"Big Ravine original grid mismatch");
                }
            }
            if(lake) {
                const auto tuning=srf::extract_course_clock(rom,course.id,body.word(0x66));
                const auto lap=static_cast<std::uint16_t>(i==1?35:30);
                require(race.remaining_seconds()==40 && tuning.lap_seconds==std::array{lap,lap},
                        "Lake Side car clock differs from the original");
                constexpr std::array x{4032,5312,4352,4992},z{-5088,-4128,-3168,-2368};
                for(unsigned slot=0;slot<4;++slot) {
                    const auto& racer=initial[slot].body;
                    require(racer.word(0x1e)==x[slot] && racer.word(0x22)==static_cast<std::uint16_t>(z[slot]) &&
                            racer.word(0x20)==static_cast<std::uint16_t>(slot==3?-160:-480) && !racer.word(0x24),
                            "Lake Side grid differs from the original");
                }
            }
            if(cruise) {
                require(race.remaining_seconds()==(i==1?50:40),"Night Cruise car clock differs from the ROM");
                constexpr std::array x{-10368,-9088,-10048,-9408},z{672,1632,2592,3392};
                for(unsigned slot=0;slot<4;++slot) {
                    const auto& racer=initial[slot].body;
                    require(racer.word(0x1e)==static_cast<std::uint16_t>(x[slot]) &&
                            racer.word(0x20)==static_cast<std::uint16_t>(-3840-(slot==3?160:480)) &&
                            racer.word(0x22)==z[slot] && !racer.word(0x24),"Night Cruise original grid mismatch");
                }
            }
            if(white) {
                constexpr std::array x{2112,3392,2432,3072},z{-8928,-7968,-7008,-6208};
                for(unsigned slot=0;slot<initial.size();++slot) {
                    const auto& racer=initial[slot].body;
                    require(racer.word(0x1e)==x[slot] && racer.word(0x22)==static_cast<std::uint16_t>(z[slot]) &&
                            racer.word(0x20)==static_cast<std::uint16_t>(slot==3?-160:-480) &&
                            racer.word(0x24)==0,"White Land original grid mismatch");
                }
            }
            if(sea) {
                const auto tuning=srf::extract_course_clock(rom,course.id,body.word(0x66));
                const auto lap=static_cast<std::uint16_t>(i==1?30:20);
                require(race.remaining_seconds()==(i==1?30:40) && tuning.lap_seconds==std::array{lap,lap},
                        "Sea Breeze car-specific clock differs from 03:EFBB");
                constexpr std::array grid_x{24192,25472,24512,25152},grid_z{-2208,-1248,-288,512};
                for(unsigned slot=0;slot<initial.size();++slot)
                    require(initial[slot].body.word(0x1e)==grid_x[slot] &&
                            initial[slot].body.word(0x20)==static_cast<std::uint16_t>(-1536-(slot==3?160:480)) &&
                            initial[slot].body.word(0x22)==static_cast<std::uint16_t>(grid_z[slot]) &&
                            initial[slot].body.word(0x24)==0,"Sea Breeze starting grid differs from ROM");
            }
            if(forest) {
                constexpr std::array grid_x{23232,24512,23552,24192},grid_z{10272,11232,12192,12992};
                for(unsigned slot=0;slot<initial.size();++slot)
                    require(initial[slot].body.word(0x1e)==grid_x[slot] &&
                            initial[slot].body.word(0x22)==grid_z[slot] && !initial[slot].body.word(0x24),
                            "King's Forest starting grid differs from ROM");
            }
            if(night) {
                constexpr std::array grid_x{21312,22592,21632,22272},grid_z{14112,15072,16032,16832};
                for(unsigned slot=0;slot<4;++slot)
                    require(initial[slot].body.word(0x1e)==grid_x[slot] &&
                            initial[slot].body.word(0x22)==grid_z[slot] && !initial[slot].body.word(0x24),
                            "Night Owl starting grid differs from ROM");
            }
            if(course.id==srf::CourseId::sunset_valley) {
                const std::array<int,4> grid_x{-4768,-5728,-6688,-7488};
                const std::array<int,4> grid_z{-27648,-26368,-27328,-26688};
                for(unsigned slot=0;slot<initial.size();++slot) {
                    const auto& racer=initial[slot].body;
                    require(racer.word(0x1e)==static_cast<std::uint16_t>(grid_x[slot]) &&
                            racer.word(0x22)==static_cast<std::uint16_t>(grid_z[slot]) &&
                            racer.word(0x24)==0x4000,"Sunset Valley starting grid differs from ROM");
                }
            }
            require(body.word(0x66)==definitions[i],"Player definition does not match original selection table");
            require(body.word(0x6e)==parameters[i],"Player tuning does not match its car");
            const auto descriptor=rom.u16(0x48000+(definitions[i]&0x7fff));
            require(std::any_of(world.models.begin(),world.models.end(),[&](const auto& model){
                return model.descriptor_address==descriptor&&!model.primitives.empty();}),"Player model missing from package");
            const auto sound=srf::extract_race_audio_tuning(rom.bytes(),body.word(0x66));
            const auto tuning=srf::extract_vehicle_tuning(rom.bytes(),body.word(0x6e));
            require(tuning.sixth_gear_limit==(cars[i]==srf::PlayerCar::f_type?600:0),"Sixth gear must only be enabled by the F-TYPE table");
            if(tuning.sixth_gear_limit) {
                auto high_speed=body; high_speed.word(0x4a)=575;
                srf::vehicle_drivetrain(high_speed,tuning,{},0x10);
                require(high_speed.word(0x4e)==6,"F-TYPE cannot select its sixth gear");
                for(unsigned j=0;j<tuning.sixth_gear_curve.size();++j)
                    require(tuning.sixth_gear_curve[j]==rom.bytes()[0x48153+5*17+j],"Sixth-gear curve differs from ROM");
            }
            for(unsigned j=0;j<sound.speed_curve.size();++j)
                require(sound.speed_curve[j]==rom.bytes()[0x5b158+sound_offsets[i]+j],"Wrong engine sound curve");
            auto drive=[&] {
                srf::RaceAudioState audio;
                for(unsigned tick=0;tick<120;++tick) {
                    srf::RaceAudioOutput events;
                    const auto input=srf::prepare_player_input(audio,race.racers()[3].body,
                        tick<100?0x10:0x40,race.countdown_active(),events);
                    race.step(input,4);
                }
            };
            drive();
            require(!race.countdown_active()&&race.phase()==120,"Car did not leave countdown");
            require(race.racers()[3].body.word(0x1e)!=initial[3].body.word(0x1e) ||
                    race.racers()[3].body.word(0x22)!=initial[3].body.word(0x22),"Car did not move");
            require(race.failure()==srf::RaceFailure::none,"Drive smoke ended in failure");
            const auto driven=race.racers();
            for(unsigned opponent=0;opponent<3;++opponent)
                require(driven[opponent].body.word(0x1e)!=initial[opponent].body.word(0x1e) ||
                        driven[opponent].body.word(0x22)!=initial[opponent].body.word(0x22),"Opponent did not move");
            race.reset();require(equal(race.racers(),initial),"Restart lost selected car/grid");
            drive();require(equal(race.racers(),driven),"Selected-car restart is not deterministic");
            std::cout<<"PASS "<<course.slug<<" car "<<i<<": model, tuning, engine curve, driving, opponents and restart\n";
        }
        }
        srf::NativeRace legacy(rom),explicit_default(rom,srf::PlayerCar::four_wd);
        for(unsigned phase=0;phase<250;++phase) {
            const auto input=std::uint16_t(phase<130?0x10:phase<190?0x11:0x40);
            legacy.step(input,4);explicit_default.step(input,4);
            require(equal(legacy.racers(),explicit_default.racers()),"Default 4WD trajectory changed");
        }
        bool rejected=false;
        try {srf::NativeRace invalid(rom,static_cast<srf::PlayerCar>(3));} catch(const std::invalid_argument&) {rejected=true;}
        require(rejected,"Unsupported car accepted");
        rejected=false;
        try {srf::NativeRace invalid(rom,srf::PlayerCar::four_wd,static_cast<srf::CourseId>(srf::courses.size()));} catch(const std::invalid_argument&) {rejected=true;}
        require(rejected,"Unsupported course accepted");
        std::cout<<"PASS default compatibility and invalid selection\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
