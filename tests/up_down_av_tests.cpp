#include "../src/bonus_dashboard.hpp"
#include "../src/bonus_hud_map.hpp"
#include "srf/audio.hpp"
#include <iostream>

namespace {
void check_course(const srf::Rom& rom,srf::StuntCourse course) {
    const bool ice=course==srf::StuntCourse::ice_dance;
    // Separate cartridge captures after the trailer, with the driving song at SPC EE40.
    const auto bank=srf::extract_stunt_audio(rom,course);
    if(srf::sha256(bank.music)!=(ice?"d112b7de2aec38808e863a33b4eb5178538276b2f280a106805ec7229e059bd2":
                                     "9bc305f8fe2f2a86d030241ffabb0f7b2148c14aa94aa44239c91aeab985cbb4"))
        throw std::runtime_error("Stunt Trax driving music differs from the cartridge");
    srf::MusicRenderer music(bank);
    music.play_music();
    std::array<std::int16_t,1066> samples{};
    bool audible=false;
    for(unsigned i=0;i<3600;++i) {
        music.render(samples);
        audible|=std::any_of(samples.begin(),samples.end(),[](auto sample){return sample!=0;});
    }
    if(!audible)throw std::runtime_error("Stunt Trax driving music is silent");

    // Isolate the same four large map sprites as original OAM slots 112..115.
    srf::CourseHudMap map(rom,srf::stunt_rom_index(course));
    std::vector<std::uint32_t> overlay(208*128);
    map.draw_route(overlay,5);
    std::vector<std::uint8_t> mask;
    for(unsigned y=80;y<112;++y)
        for(unsigned x=5;x<37;++x)mask.push_back(overlay[y*208+x]!=0);
    if(srf::sha256(mask)!=(ice?"201fea21257a8b8c0b7210b0d172e229b47c1d5a9affbc924157622e602efc8b":
                               "f956bcf55ec0ac9c89f30e2134bf30420dedeac58301e2747862b6b88018bcb1"))
        throw std::runtime_error("Stunt Trax map differs from the cartridge");
    for(auto car:{srf::PlayerCar::coupe,srf::PlayerCar::four_wd,srf::PlayerCar::f_type}) {
        srf::BonusDashboard panel(rom,car,true);
        std::vector<std::uint8_t> rgb;
        for(auto pixel:panel.draw(0,0,0))
            for(unsigned shift:{3u,11u,19u})rgb.push_back((pixel>>shift)&31);
        if(srf::sha256(rgb)!="9250abb67e4018c2390746b54894d9e6c641d7b8aa6cf9125308a2e7036f20f2")
            throw std::runtime_error("Stunt Trax dashboard differs from the parked cartridge run");
    }
    std::cout<<srf::stunt_course_name(course)<<" driving music, HUD map and three parked dashboards match the original\n";
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        const auto* path=argc>1?argv[1]:_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_course(rom,srf::StuntCourse::up_n_down);
        check_course(rom,srf::StuntCourse::ice_dance);
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
