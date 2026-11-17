#include "srf/audio.hpp"
#include "boot_title_checks.hpp"
#include "boot_menu_checks.hpp"
#include "../src/demo_dashboard.hpp"
#include "demo_camera_checks.hpp"
#include "demo_drive_checks.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
void demo_dashboard(const srf::Rom& rom) {
    srf::DemoDashboard hud(rom);
    std::vector<std::uint8_t> colors;
    // At field 2000 the original OAM reads 092 MPH. Exclude the live 3D
    // aperture and compare every other pixel, including the rounded corners.
    for(const auto pixel:hud.draw(396,15,395)) {
        const unsigned color=pixel>>24?((pixel>>3)&31)|((pixel>>6)&0x3e0)|((pixel>>9)&0x7c00):0x8000;
        colors.push_back(static_cast<std::uint8_t>(color));
        colors.push_back(static_cast<std::uint8_t>(color>>8));
    }
    require(srf::sha256(colors)=="9596faf85927df1e7f0389e3292025db22981c4b5add6cec299f69eea0f7fbd9",
            "Easy Ride demo instruments or playfield mask differ from the cartridge");
}
void title_audio(const srf::Rom& rom) {
    const auto original=srf::extract_title_audio(rom);
    const auto bank=srf::decode_audio(srf::encode_audio(original));
    require(bank.music.size()==4510 && bank.instruments.size()==29,"Incorrect title score or instruments");
    srf::MusicSequence score(bank);
    // Original SPC voice pointers sampled on the title, Game Selection,
    // rotating car and initials screens of one uninterrupted cold boot.
    constexpr unsigned checkpoints[][8]{
        {0xf3e6,0xf410,0xff40,0xf460,0xf48f,0xf4c1,0xf4ed,0xf525},
        {0xf555,0xf5a5,0xf5dd,0xf63a,0xf697,0xf6bd,0xf6f9,0xf739},
        {0xf761,0xf79e,0xf7c7,0xf82a,0xf862,0xf8aa,0xf8d8,0xf924},
        {0xf97f,0xf9d7,0xfa63,0xfaff,0xfb6a,0xfbdb,0xfc34,0xffd9}
    };
    for(const auto& expected:checkpoints) {
        bool found=false;
        for(unsigned tick=0;tick<5000 && score.playing() && !found;++tick) {
            score.tick();
            found=std::equal(std::begin(expected),std::end(expected),score.channels().begin(),
                             [](unsigned p,const srf::MusicChannel& ch){return p==ch.cursor;});
        }
        require(found,"Title/menu music diverged from the original score");
    }
    srf::MusicRenderer audio(bank);
    audio.set_engine(0,false,false,false);
    audio.set_music_enabled(false);
    audio.play_race_effect(0x39);
    std::vector<std::int16_t> pcm(32000*2);
    audio.render(pcm);
    require(std::ranges::any_of(pcm,[](int s){return std::abs(s)>100;}),"Silent Nintendo logo cue");
    audio.play_music();
    // The original sound ports remain clear through the drive. The very first
    // nonzero write is 05 on warm-title field four, then the score restarts.
    srf::DemoFlow demo;
    std::vector<std::int16_t> field(532*2);
    while(!demo.finished()) {
        demo.advance(0);
        audio.render(field);
    }
    require(audio.ticks()>0,"Title music stopped during the Easy Ride demo");
    srf::BootTitle warm(srf::BootTitleEntry::demo_return);
    while(warm.frame().tick<93) {
        warm.advance(false);
        const auto& frame=warm.frame();
        if(frame.music_command>=0)audio.sound_command(static_cast<std::uint8_t>(frame.music_command));
        if(frame.stop_music)audio.set_music_enabled(false);
        if(frame.start_music)audio.play_music();
        audio.render(field);
    }
    for(unsigned second=0;second<80;++second) {
        audio.render(pcm);
        require(std::ranges::any_of(pcm,[](int s){return std::abs(s)>100;}),"Silent title/menu music");
    }
}
}

int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_boot_title(rom);
        check_boot_menu(rom);
        check_stunt_menu(rom);
        demo_dashboard(rom);
        check_demo_drive(rom);
        check_demo_camera(rom);
        title_audio(rom);
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
