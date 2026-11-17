#include "srf/speed_bonus.hpp"
#include "srf/trailer_loading.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
void require(bool condition,const std::string& detail) {
    if(!condition)throw std::runtime_error(detail);
}
void compare(const srf::Rom& rom) {
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"trailer_loading.csv");
    std::string line;
    require(bool(std::getline(file,line)),"Missing original loading poses");
    constexpr unsigned closed_at[3][3]{{478,461,464},{466,450,452},{461,445,439}};
    for(unsigned cls=0;cls<3;++cls) {
        const srf::SpeedBonus race(rom,static_cast<srf::SpeedClass>(cls));
        for(unsigned car=0;car<3;++car) {
            const auto selected=static_cast<srf::PlayerCar>(car);
            srf::TrailerLoading scene(rom,race.scene(),selected);
            require(!scene.car_visible() && scene.cargo_descriptor()==0xa778,"Wrong initial trailer");
            scene.advance(8);
            const auto origin=scene.ramp().position;
            require(scene.car_visible() && scene.ramp().visual_descriptor==0xa7b0 && origin.y==-50
                    && std::int16_t(scene.car().body.word(0x22))==origin.z-3500,"Incorrect open ramp / approach");
            // 08:B5FA at the initial orbit bearing. Each car selects its own column
            // in the direction sheet. The near model replaces it below 4500.
            constexpr unsigned frame[]{17,19,16}, diameter[]{412,456,444};
            const auto far=scene.distant_car(origin.x+4500,origin.z-3500,std::uint8_t(-141));
            require(far && far->table==0x99a3 && far->frame==frame[car] && far->diameter==diameter[car]
                    && far->position.y==-480 && !scene.distant_car(origin.x+4499,origin.z-3500,0),"Incorrect distant car");
            for(unsigned sample=0;sample<4;++sample) {
                require(bool(std::getline(file,line)),"Missing loading pose");
                std::replace(line.begin(),line.end(),',',' ');
                std::istringstream in(line);
                unsigned expected_class,expected_car,phase;
                in>>expected_class>>expected_car>>phase;
                require(expected_class==cls && expected_car==car,"Unexpected original fixture order");
                while(scene.phase()<phase && !scene.door_closed() && scene.video_ticks()<500)scene.advance();
                const auto& body=scene.car().body;
                const auto wheel_y=[&](unsigned i) {
                    const auto& wheel=scene.car().wheels[i];
                    return std::int16_t(wheel.word(0x20)+wheel.word(0x4c));
                };
                const std::array<int,8> actual{std::int16_t(body.word(0x1e))-origin.x,std::int16_t(body.word(0x20)),
                    std::int16_t(body.word(0x22))-origin.z,std::int16_t(body.word(0x24)),std::int16_t(body.word(0x26)),
                    std::int16_t(body.word(0x28)),wheel_y(0),wheel_y(3)};
                require(scene.phase()==phase,"Loading phase missing");
                for(const auto value:actual) {
                    int expected;
                    require(bool(in>>expected) && value==expected,"Original car or wheel pose diverged at "+line);
                }
            }
            unsigned frames=0;
            while(!scene.door_closed() && scene.video_ticks()<500) {
                scene.advance();
                if(scene.ramp().visible && scene.ramp().visual_descriptor==0xa794)
                    frames|=1u<<scene.ramp().frame;
            }
            require(frames==0xfe && scene.video_ticks()==closed_at[cls][car] && !scene.car_visible()
                    && !scene.ramp().visible && !scene.distant_car(0,0,0) && scene.cargo_descriptor()==0xa75c,
                    "Door sequence or retirement changed");
            srf::TrailerLoading grouped(rom,race.scene(),selected);
            grouped.advance(600);
            require(grouped.door_closed() && grouped.phase()>=125 && grouped.car().body.words==scene.car().body.words,
                    "Post-close camera clock or batched car update changed");
        }
    }
    const auto sprites=srf::extract_vehicle_sprites(rom,srf::CourseId::easy_ride,false,true);
    require(std::count_if(sprites.begin(),sprites.end(),[](const auto& s){return s.material_table==0x99a3;})==32,
            "Missing original distant-car directions");
}
}

int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        compare(srf::Rom::load(path));
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
