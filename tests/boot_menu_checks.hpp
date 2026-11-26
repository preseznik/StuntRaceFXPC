#pragma once
#include "../src/boot_menu_art.hpp"
#include "../src/stunt_board_art.hpp"

inline void check_boot_menu(const srf::Rom& rom) {
    using namespace srf;
    const auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    BootMenuArt art(rom);
    BootMenu menu;
    std::vector<std::uint32_t> pixels(256*224);
    struct Sample { unsigned tick; std::uint32_t rgb555_hash; };
    // Entire cartridge video field, with the title Start edge on cold field 710.
    constexpr Sample original[]{{845,0xd250cf95},{851,0x08d751c5},{860,0x164862b5},{869,0x5a27b118},
        {886,0x357eeaa9},{900,0x5c75c7b2},{905,0x72d792dc},{1040,0xd62ac45a},{1140,0x225ee0a5}};
    unsigned next=0;
    for(unsigned t=845;t<=1141;++t) {
        if(t>845)menu.advance(t>=1090 && t<1098?0x1000:0);
        if(next<std::size(original) && t==original[next].tick){
            art.draw(pixels,menu.frame());std::uint32_t hash=2166136261u;
            for(const auto p:pixels){const auto rgb=((p>>3)&31)|((p>>6)&992)|((p>>9)&31744);hash=(hash^rgb)*16777619u;}
            check(hash==original[next++].rgb555_hash,"Game Selection cartridge pixels");
        }
    }
    check(next==std::size(original) && menu.frame().screen==BootMenuScreen::car,"Original car selection handoff");
    const auto press=[&](unsigned mask){menu.advance(0);menu.advance(std::uint16_t(mask));};
    press(0x0200);check(menu.car()==PlayerCar::f_type,"Original F-Type on left");
    press(0x0200);check(menu.car()==PlayerCar::coupe,"Original Coupe wraps left");
    press(0x0100);check(menu.car()==PlayerCar::f_type,"Original car selection right");
    press(0x2000);check(menu.event()==BootMenuEvent::unavailable,"Enemy selection feedback");
    press(0x0080);check(menu.frame().screen==BootMenuScreen::car,"SNES A ignores car confirm");
    press(0x8000);check(menu.frame().screen==BootMenuScreen::initials,"SNES B opens initials");
    press(0x0200);check(menu.frame().cursor==0,"Initials left clamp");
    press(0x0800);check(menu.initials()=="BAA","Original initials increment");
    for(int i=0;i<12;++i)menu.advance(0x0800);
    check(menu.initials()=="CAA","Original held letter repeat");
    press(0x0100);press(0x0100);press(0x0100);check(menu.frame().cursor==2,"Initials right clamp");
    press(0x0400);check(menu.initials()=="CAZ","Original alphabet wraps downward");
    press(0x4000);check(menu.frame().screen==BootMenuScreen::car,"Cancel initials");
    press(0x1000);press(0x1000);
    for(unsigned i=0;i<30;++i)menu.advance(0x1000);
    check(menu.event()==BootMenuEvent::board && menu.car()==PlayerCar::f_type && menu.initials()=="CAZ","Class board handoff values");
    menu.resume_car();press(0x4000);for(int i=0;i<22;++i)menu.advance(0);
    press(0x0800);check(menu.frame().mode==3,"Original game selection upward wrap");
    press(0x8000);for(unsigned i=0;i<30;++i)menu.advance(0);
    check(menu.frame().mode==3 && menu.event()==BootMenuEvent::board,"Test Run opens the practice course");
    // Original nonblack model pixels in the isolated 104x88 car window at these
    // independent turntable angles. Native polygons use their own rasterization.
    struct Body {PlayerCar car;unsigned phase,count;};
    constexpr Body bodies[]{{PlayerCar::four_wd,58,5024},{PlayerCar::coupe,29,1985},{PlayerCar::f_type,30,2271}};
    for(const auto& b:bodies){
        BootMenuFrame f;f.screen=BootMenuScreen::car;f.car=b.car;f.entering=false;f.show_car=true;
        f.turntable=b.phase;f.header_scroll=f.panel_scroll=256;f.arrow_offset=88;
        art.draw(pixels,f);unsigned n=0;
        for(int y=95;y<183;++y)for(int x=32;x<136;++x)n+=pixels[y*256+x]!=0xff000000u;
        check(n>b.count*95/100 && n<b.count*105/100,"Car turntable silhouette area vs cartridge");
        const auto before=pixels;f.turntable=(f.turntable+16)%64;art.draw(pixels,f);
        unsigned changed=0;for(int y=95;y<183;++y)for(int x=32;x<136;++x)changed+=before[y*256+x]!=pixels[y*256+x];
        check(changed>1200,"Quarter turn did not show a new side of the ROM car");
    }
}

inline void check_stunt_menu(const srf::Rom& rom) {
    using namespace srf;
    const auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    BootMenu menu;
    const auto press_menu=[&](unsigned mask){menu.advance(0);menu.advance(static_cast<std::uint16_t>(mask));};
    for(unsigned i=0;i<41;++i)menu.advance(0);
    press_menu(0x0400);press_menu(0x1000);
    for(unsigned i=0;i<51;++i)menu.advance(0);
    check(menu.frame().mode==1 && menu.frame().screen==BootMenuScreen::car,"Stunt Trax car selection");
    press_menu(0x2000);check(menu.event()==BootMenuEvent::none,"Stunt car selection ignores opponent-selection shortcut");
    press_menu(0x0100);press_menu(0x8000);
    check(menu.frame().screen==BootMenuScreen::initials && menu.car()==PlayerCar::coupe,"Stunt Trax chosen car");
    press_menu(0x1000);for(unsigned i=0;i<30;++i)menu.advance(0);
    check(menu.event()==BootMenuEvent::board,"Stunt Trax board handoff");

    StuntBoard board;
    std::vector<std::uint32_t> pixels(256*224);
    const auto verify=[&](const StuntBoardArt& art,std::initializer_list<std::array<int,4>> regions,unsigned original){
        art.draw(pixels,board);
        unsigned hash=2166136261u;
        for(const auto& [x,y,w,h]:regions)for(int j=y;j<y+h;++j)for(int i=x;i<x+w;++i){
            const unsigned v=pixels[256*j+i],rgb=((v>>3)&31)|((v>>6)&992)|((v>>9)&31744);
            hash=(hash^rgb)*16777619u;
        }
        check(hash==original,"Stunt Trax board differs from cartridge capture");
    };
    // The original empty board and PLAY captures are cold fields 2108/2283
    // of fixtures/rock_field_menu_inputs.csv. The populated Data Box follows
    // a 4WD finish with 34 stars, 86 seconds banked, and initials AAA.
    const StuntBoardArt art(rom);
    std::array<std::vector<StuntRecord>,playable_stunt_courses.size()> records;
    records[0].push_back({34,86,"AAA",PlayerCar::four_wd});
    const StuntBoardArt recorded(rom,records);
    const auto press=[&](unsigned mask){board.advance(0);board.advance(static_cast<std::uint16_t>(mask));};
    while(board.tick<180)board.advance(0);
    check(board.course==0 && board.outline_visible(),"Rock Field must be selected initially");
    verify(art,{{40,72,64,48},{152,72,64,48},{40,152,64,48},{152,152,64,48}},0xd74b1b04);
    press(0x0800);check(board.course==2,"Up selects Ice Dance");
    press(0x0100);check(board.course==3,"Right selects Blue Lake");
    press(0x1000);while(!board.panel_visible())board.advance(0);
    auto blue=board;
    blue.advance(0x1000);
    for(int i=0;i<188;++i)blue.advance(0);
    check(blue.ready_to_drive() && playable_stunt_courses.at(blue.course)==StuntCourse::blue_lake,"Play Blue Lake");
    press(0x4000);press(0x0400);check(board.course==1,"Down selects Up'n Down");
    press(0x1000);while(!board.panel_visible())board.advance(0);
    verify(recorded,{{46,72,166,130}},0x692925a4);
    records[1].push_back({31,70,"AAA",PlayerCar::four_wd});
    const StuntBoardArt both(rom,records);
    verify(both,{{88,108,122,46}},0x4aa6f3c4);
    const std::initializer_list<std::array<int,4>> boxes{{15,100,107,21},{136,100,105,21},{15,175,107,23},{136,175,105,23}};
    press(0x0400);press(0x1000);
    verify(recorded,boxes,0xe760334d);verify(both,boxes,0x92f2c360);
    press(0x4000);while(!board.panel_visible())board.advance(0);
    press(0x4000);press(0x0200);press(0x1000);
    while(!board.panel_visible())board.advance(0);
    verify(art,{{46,72,166,130}},0x1a232d13);
    verify(both,{{88,108,122,46}},0xbbcc2321);
    press(0x0400);press(0x1000);check(board.page==StuntBoard::Page::data,"Open data box");
    verify(art,boxes,0xe760334d);verify(both,boxes,0xccdf498c);
    press(0x1000);while(!board.panel_visible())board.advance(0);
    check(board.choice==1,"Return from data box");
    press(0x0800);press(0x8000);
    for(unsigned i=0;i<187;++i)board.advance(0);
    check(!board.ready_to_drive(),"Early Rock Field trailer handoff");
    board.advance(0);check(board.ready_to_drive(),"Rock Field trailer handoff");
    for(const auto car:{PlayerCar::four_wd,PlayerCar::coupe,PlayerCar::f_type}){
        board=StuntBoard(car);
        while(!board.outline_visible())board.advance(0);
        press(0x0100);press(0x1000);
        while(!board.panel_visible())board.advance(0);
        press(0x1000);
        for(unsigned i=1;i<(car==PlayerCar::coupe?188u:189u);++i)board.advance(0);
        check(!board.ready_to_drive(),"Early Up'n Down trailer handoff");
        board.advance(0);check(board.ready_to_drive(),"Up'n Down trailer handoff");
        board=StuntBoard(car);
        while(!board.outline_visible())board.advance(0);
        press(0x0800);press(0x1000);
        while(!board.panel_visible())board.advance(0);
        press(0x1000);
        for(unsigned i=0;i<190;++i)board.advance(0);
        check(!board.ready_to_drive(),"Early Ice Dance trailer handoff");
        board.advance(0);check(board.ready_to_drive(),"Ice Dance trailer handoff");
    }
}
