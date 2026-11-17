#pragma once

#include "../src/course_map_art.hpp"
#include "srf/class_board.hpp"

inline void check_course_map_art(const srf::Rom& rom) {
    // Hash the cartridge's 5-bit components directly. Each rectangle covers an
    // entire thumbnail or the bounded center of the shaded record panel, so an
    // unrelated CPU or GSU backdrop noise seed cannot perturb these checks.
    constexpr std::array<unsigned,4> splits{11558,10881,13995,14486};
    srf::ChampionshipProgress progress;
    srf::Championship best(srf::SpeedClass::expert,srf::PlayerCar::coupe,"AAA");
    for(const auto split:splits) {
        if(best.finish({best.course(),{},1,87,split},progress)!=srf::ChampionshipOutcome::class_complete)
            best.advance();
    }
    const auto verify=[&](const auto& art,const auto& frame,bool panel,std::uint32_t expected,
                          std::array<int,4> rectangle={}) {
        std::vector<std::uint32_t> pixels(256*224);
        art.draw(pixels,frame);
        std::uint32_t hash=2166136261u;
        const auto area=[&](int left,int top,int width,int height) {
            for(int y=top;y<top+height;++y)for(int x=left;x<left+width;++x) {
                const auto rgba=pixels[y*256+x];
                const auto rgb=((rgba>>3)&31)|(((rgba>>11)&31)<<5)|(((rgba>>19)&31)<<10);
                hash=(hash^rgb)*16777619u;
            }
        };
        if(rectangle[2])area(rectangle[0],rectangle[1],rectangle[2],rectangle[3]);
        else if(panel)area(65,80,144,110);
        else for(const int y:{71,151})for(const int x:{40,88,152,200})area(x,y,32,32);
        if(hash!=expected)throw std::runtime_error("Speed Trax course map differs from cartridge capture");
    };

    // Complete selection border and both bottom choices, including the hand.
    // The captured RNG seeds keep the pixels behind the OAM comparable.
    const srf::Championship cold(srf::SpeedClass::novice,srf::PlayerCar::four_wd);
    const srf::CourseMapArt first_board(rom,cold,progress,0,43095);
    srf::ClassBoard choose(false);
    for(unsigned t=1;t<=700;++t) {
        choose.advance(t==437?0x1000:0);
        if(t==122)verify(first_board,choose.frame(),false,0x358d03e7,{8,137,240,78});
        if(t==700)verify(first_board,choose.frame(),false,0xbf360b5d,{99,172,84,32});
    }
    auto cancel=choose.frame();
    cancel.cancel_selected=true;cancel.noise_tick=429-94;
    const srf::Championship expert(srf::SpeedClass::expert,srf::PlayerCar::four_wd);
    const srf::CourseMapArt expert_board(rom,expert,progress,0,20959);
    verify(expert_board,cancel,false,0x0bf72327,{99,172,84,32});

    for(const auto league:{srf::SpeedClass::novice,srf::SpeedClass::expert,srf::SpeedClass::master}) {
        srf::Championship current(league,srf::PlayerCar::four_wd);
        for(unsigned i=0;i<2;++i) {
            current.finish({current.course(),{},1,83,splits[i]},progress);
            current.advance();
        }
        const srf::CourseMapArt art(rom,current,progress);
        srf::CourseMapFrame f;
        f.brightness=15;
        f.board_scroll=league==srf::SpeedClass::master?0:80;
        if(league==srf::SpeedClass::novice) {
            // KEEP OUT slides up one row at a time to expose Sunset Valley.
            constexpr std::array<std::uint32_t,5> captures{
                0xa07040e2,0xe7109969,0xe9702d99,0x859f276c,0xdb05d6f8};
            for(unsigned i=0;i<captures.size();++i){f.card_step=i;verify(art,f,false,captures[i]);}
            continue;
        }
        if(league==srf::SpeedClass::master) {
            // First dim Master frame precedes the tilemap DMA that installs the covers.
            f.base_cards=true;f.brightness=1;verify(art,f,false,0x6a7506a4);
            f.base_cards=false;f.brightness=15;
        }
        verify(art,f,false,league==srf::SpeedClass::expert?0xbc49732e:0x0213aaf9);
        f.card_step=4;
        verify(art,f,false,league==srf::SpeedClass::expert?0x01669fc8:0xcc1477dc);
        f.panel_left=57;f.panel_right=216;f.panel_top=69;f.panel_bottom=204;f.panel_shade=25;
        f.records_visible=true;f.prompt_visible=true;
        if(league==srf::SpeedClass::master) {
            verify(art,f,true,0xb39a20dd); // Empty Master even after Expert was cleared.
            f.record_page=1;verify(art,f,true,0xace24cfc);
            continue;
        }
        constexpr std::array<unsigned,5> car_phases{3,1,0,2,1};
        constexpr std::array<std::uint32_t,5> scores{
            0x3320ab76,0xceacca96,0x0ceed589,0x05ae63a0,0x05732955};
        for(unsigned page=0;page<scores.size();++page) {
            f.record_page=page;f.record_car_phase=car_phases[page];
            verify(art,f,true,scores[page]);
        }
    }

    // Eight visible card rectangles during the reveal after a qualified third race.
    constexpr std::uint32_t final_cards[3][5]={
        {0xdb05d6f8,0x21e63c4b,0xdefd4e2e,0x0d41bb11,0x49396101},
        {0x01669fc8,0x1eb45e59,0x533a9a44,0x0b7f85cb,0x063b2729},
        {0xcc1477dc,0x77066fd6,0x534938cc,0xb4d30c96,0x32fe2ee4}
    };
    for(unsigned i=0;i<3;++i) {
        srf::Championship current(static_cast<srf::SpeedClass>(i),srf::PlayerCar::four_wd);
        for(unsigned round=0;round<3;++round) {
            current.finish({current.course(),{},1,83,splits[round]},progress);
            current.advance();
        }
        const srf::CourseMapArt art(rom,current,progress);
        srf::CourseMapFrame f;
        f.brightness=15;f.board_scroll=i==2?0:80;
        for(unsigned step=0;step<5;++step) {
            f.card_step=step;verify(art,f,false,final_cards[i][step]);
        }
    }
}
