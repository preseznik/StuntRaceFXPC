#pragma once
#include "../src/boot_title_art.hpp"
#include "../src/boot_menu_art.hpp"
#include "srf/demo_flow.hpp"

inline void check_boot_title(const srf::Rom& rom) {
    const auto check=[](bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);};
    struct Sample {unsigned tick;std::uint32_t hash;};
    // RGB555 hashes of the original complete 256x224 video field, Start at 710.
    constexpr Sample samples[]{
        {74,0x62aa1dc5u},{75,0xa9ea2e4bu},{193,0xa9ea2e4bu},{194,0x62aa1dc5u},
        {251,0x50459362u},{265,0x65fc4efau},{279,0x5c60b925u},{350,0x1a1a9b20u},
        {379,0x789438e6u},{401,0x4973535du},{411,0x7908b7c7u},{467,0x4973535du},
        {516,0x6b27a287u},{537,0x4973535du},{710,0x4973535du},{712,0xf1bab30du},
        {725,0x4755b7bdu},{754,0xa72cff52u},{771,0x4153314cu},{798,0x323bbbdbu},
        {844,0x8f3498c5u},
    };
    srf::BootTitle title;
    srf::BootTitleArt art(rom);
    std::vector<std::uint32_t> canvas(256*224);
    const auto hash_canvas=[&]{
        std::uint32_t hash=2166136261u;
        for(auto p:canvas){
            const auto rgb555=((p>>3)&31)|((p>>6)&992)|((p>>9)&31744);
            hash=(hash^rgb555)*16777619u;
        }
        return hash;
    };
    unsigned next=0;
    for(unsigned t=0;t<845;++t){
        const auto& f=title.frame();
        check(!title.finished() && f.tick==t,"Title advanced outside its video timeline");
        int expected=-1;
        if(t==72)expected=0x39;
        if(t==400 || t==434 || t==456 || t==466)expected=0x19;
        if(t==710)expected=0x46;
        if(t==724)expected=0x13;
        check(f.effect==expected && f.start_music==(t==231),"Original title audio cue");
        if(next<std::size(samples) && t==samples[next].tick){
            art.draw(canvas,f);
            check(hash_canvas()==samples[next++].hash,"Original title screen pixels");
        }
        title.advance(t+1==710);
    }
    check(next==std::size(samples) && title.finished(),"Title-to-menu handoff");

    srf::BootTitle held;
    for(unsigned t=1;t<=510;++t)held.advance(true);
    check(!held.finished() && held.frame().effect<0,"Held Start crossed title acceptance gate");
    held.advance(false);
    held.advance(false,true);
    check(held.frame().effect==0x46,"Fresh B did not confirm the title");
    for(unsigned i=0;i<135;++i)held.advance(false,true);
    check(held.finished(),"Held B restarted the title exit");

    srf::BootTitle early;
    for(unsigned t=1;t<493;++t)early.advance(false);
    early.advance(true);
    check(early.frame().effect<0,"Title accepted Start too early");
    early.advance(false,true);
    check(early.frame().effect==0x46,"Title missed B on the first eligible field");

    // Cold first black is 1547. A late Start still sounds and animates while
    // the already-scheduled demo fade completes. One field earlier opens the menu.
    for(unsigned press:{1517u,1518u}) {
        srf::BootTitle late;
        while(!late.finished())late.advance(late.frame().tick+1==press);
        check(late.to_demo()==(press==1518) && late.frame().tick==(press==1518?1547:1652),
              "Late title input took the wrong exit");
    }
    for(bool warm:{false,true}){
        srf::BootTitle idle(warm?srf::BootTitleEntry::demo_return:srf::BootTitleEntry::cold);
        constexpr Sample cold_fade[]{{1519,0x67a04a2f},{1524,0x7428c814},{1546,0x7036f359},{1547,0x62aa1dc5}};
        constexpr Sample warm_fade[]{{114,0x62aa1dc5},{115,0x50459362},{180,0xe771ffd8},
            {261,0x3ff73f05},{377,0x6b27a287},{1379,0x67a04a2f},{1407,0x62aa1dc5}};
        const std::span<const Sample> reference=warm?std::span<const Sample>(warm_fade):std::span<const Sample>(cold_fade);
        unsigned i=0;
        for(;;){
            const auto& f=idle.frame();
            if(warm)check(!f.nintendo && f.stop_music==(f.tick==66) && f.start_music==(f.tick==93)
                          && f.music_command==(f.tick==4?0x05:-1),
                          "Warm title splash/music timing");
            if(i<reference.size() && f.tick==reference[i].tick) {
                art.draw(canvas,f);
                check(hash_canvas()==reference[i++].hash,"Idle title pixels");
            }
            if(idle.finished())break;
            idle.advance(false);
        }
        check(idle.to_demo() && i==reference.size(),"Idle title did not enter Easy Ride");
    }

    // Demo tick zero is the first black field after the cold title. Compare a
    // held input across the loading gate, missed one-field pulses, mid-drive
    // and the last callback while the video is already black.
    struct DemoCase {unsigned press, hold, handoff, first_black;std::uint16_t pad;bool menu;};
    constexpr DemoCase cases[]{
        {0,0,1705,1702,0,false},{0,98,66,0,0x1000,true},{45,1,1705,1702,0x1000,false},
        {55,1,92,62,0x0400,true},{253,1,330,298,0x8000,true},
        {253,1,1705,1702,0x00f0,false},{1704,1,1732,1702,0x0100,true},
        {1705,1,1705,1702,0x1000,false},
    };
    for(auto test:cases) {
        srf::DemoFlow demo;
        unsigned first_black=0;bool seen=false;
        while(!demo.finished()){
            const auto t=demo.frame().tick+1;
            demo.advance(t>=test.press && t-test.press<test.hold?test.pad:0);
            const auto& f=demo.frame();
            if(f.brightness)seen=true;
            else if(seen && !first_black)first_black=t;
        }
        check(demo.frame().tick==test.handoff && demo.to_menu()==test.menu && first_black==test.first_black,
              "Easy Ride demo fade or input handoff");
    }
    srf::BootMenu after_demo(srf::PlayerCar::four_wd,"AAA",srf::BootMenuEntry::demo);
    srf::BootMenuArt menu_art(rom);
    constexpr Sample menu_fields[]{{0,0x62aa1dc5},{18,0x62aa1dc5},{19,0x99e75335},{28,0x6db56f59},
        {39,0xf449857a},{41,0x06c3ca49},{47,0xe5eaa17e},{66,0x3061fe2c},{93,0xb709ef39},{145,0xf9ac9989}};
    for(auto sample:menu_fields) {
        while(after_demo.frame().tick<sample.tick)after_demo.advance(0);
        menu_art.draw(canvas,after_demo.frame());
        check(hash_canvas()==sample.hash,"Game Selection after demo interrupt");
    }
}
