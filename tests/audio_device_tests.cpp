#include "srf/audio_device.hpp"
#include <SDL3/SDL_timer.h>
#include <iostream>
#include <stdexcept>

int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Usage: srf_audio_device_tests ROM");
        const auto rom=srf::Rom::load(argv[1]);
        for(const bool title:{true,false}) {
            srf::AudioDevice menu(title?srf::extract_title_audio(rom):srf::extract_course_map_audio(rom));
            menu.update({},false,false,false,false);
            if(title)menu.play_race_effect(0x39);
            menu.play_music();
            SDL_Delay(250);
            menu.pause(true);
            if(!menu.error().empty())throw std::runtime_error(menu.error());
            if(!menu.frames_rendered())throw std::runtime_error("SDL did not request menu audio");
        }
        srf::AudioDevice device(srf::extract_easy_ride_audio(rom));
        srf::RaceAudioOutput sound;sound.engine_parameter=144;sound.effects={0x49};sound.commands={0x09,0x32};
        device.update(sound,true,false,true,true);
        for(unsigned i=0;i<10;++i) {
            SDL_Delay(15);device.pause(true);device.restart();device.pause(false);
            device.update(sound,true,false,true,true);
        }
        SDL_Delay(100);device.pause(true);
        if(!device.error().empty())throw std::runtime_error(device.error());
        if(device.frames_rendered()==0)throw std::runtime_error("SDL did not request native samples");
        std::cout<<"{\"native_frames\":"<<device.frames_rendered()<<",\"restart_cycles\":10}\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
