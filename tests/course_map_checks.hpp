#pragma once
#include "srf/course_map.hpp"
#include "srf/class_board.hpp"
#include <stdexcept>

inline void check_course_map() {
    using srf::CourseMap;
    using srf::CourseMapEntry;
    const auto check=[](bool good,const char* what){if(!good)throw std::runtime_error(what);};
    const auto to=[](CourseMap& map,unsigned tick){
        while(map.frame().tick<tick && !map.finished())map.advance();
    };
    for(bool unlocked:{false,true}) {
        srf::ClassBoard warm(unlocked,2);
        check(warm.frame().speed_class==(unlocked?2u:1u) && warm.frame().board_scroll==(unlocked?0u:80u),
              "Warm class selection respects championship unlocks");
        srf::ClassBoard board(unlocked);
        const auto wait=[&](unsigned n){for(unsigned i=0;i<n;++i)board.advance();};
        const auto press=[&](std::uint16_t pad){board.advance(pad);board.advance();};
        wait(95);check(!board.frame().brightness,"Class board black before load");
        board.advance(0x1000);check(board.frame().base_cards && board.frame().brightness==1,"First board tile upload");
        for(unsigned i=0;i<28;++i)board.advance(0x1000);
        check(board.frame().class_outline_y==137 && !board.choosing_play(),"Held Start cannot accept the class");
        press(0x0800);press(0x0800);wait(24);
        check(board.frame().speed_class==(unlocked?2u:1u),"Class selection gates Master");
        check(board.frame().board_scroll==(unlocked?0u:80u),"Master row scroll");
        press(0x8000);wait(86);board.advance(0x0100);wait(2);
        check(board.frame().records_visible && board.frame().record_page==1 && !board.choosing_play(),"Split browsing opens before Play");
        wait(30);check(board.choosing_play(),"Play and Cancel ready");
        press(0x0400);wait(1);check(board.frame().cancel_selected,"Down selects Cancel");
        press(0x1000);wait(59);
        check(board.frame().class_outline_y>=0 && !board.frame().card_step && !board.choosing_play(),"Cancel closes records and covers course");
        press(0x1000);wait(119);press(0x4000);wait(59);
        check(board.frame().class_outline_y>=0,"Y returns to class selection");
        press(0x1000);wait(119);board.advance(0x1000);
        const unsigned loading=unlocked?187:184;
        wait(loading-1);check(!board.finished(),"Early first-race handoff");
        board.advance();check(board.finished(),"First race after Play");
    }
    for(unsigned league=0;league<3;++league){
        CourseMap map(league,league==2);
        check(map.frame().music_command==5 && !map.frame().brightness,"Course map incoming fade");
        to(map,84);check(map.frame().start_music,"Course map menu bank start");
        to(map,97);check(!map.frame().brightness,"Course map incoming black");
        map.advance();check(map.frame().brightness==1,"Course map first visible board");
        to(map,122);check(map.frame().brightness==15,"Course map fully lit board");
        to(map,151);check(map.frame().effect==0x2d,"Course map card cue");
        for(unsigned i=0;i<4;++i){
            constexpr unsigned edges[]={162,164,172,180};
            to(map,edges[i]-1);check(map.frame().card_step==i,"Course map premature card change");
            map.advance();check(map.frame().card_step==i+1,"Course map card change");
        }
        to(map,193);check(map.frame().effect==0x24 && !map.frame().panel_shade,"Course map opening cue");
        map.advance();
        check(map.frame().panel_top==(league?73:153) && map.frame().panel_shade==31,"Course map pane origin");
        to(map,209);check(!map.frame().records_visible && map.frame().panel_left==57 &&
            map.frame().panel_right==216 && map.frame().panel_top==69 && map.frame().panel_bottom==204,
            "Course map settled panel bounds");
        map.advance();check(map.frame().records_visible && map.frame().prompt_visible,"Course map records appear");
        to(map,399);map.advance(true);map.advance();
        check(map.frame().effect==0x46 && map.frame().music_command==4,"Course map confirmation sound");
        map.advance();check(map.frame().records_visible,"Course map last visible records");
        map.advance();check(!map.frame().records_visible && map.frame().panel_left==54,"Course map closing pane");
        to(map,421);check(map.frame().brightness && map.frame().panel_left==2,"Course map last visible board");
        map.advance();check(!map.frame().brightness,"Course map first outgoing black");
        to(map,league?441:446);check(map.frame().music_command==5,"Course map outgoing music fade");
        constexpr unsigned stop[]={508,504,506},fanfare[]={573,571,572},grid[]={590,586,586};
        to(map,stop[league]);check(map.frame().stop_music,"Course map bank reset");
        to(map,fanfare[league]);check(map.frame().start_grid_music,"Course map grid fanfare");
        to(map,grid[league]-1);check(!map.finished(),"Course map early grid handoff");
        map.advance();check(map.finished(),"Course map grid handoff");
    }

    // Each SNES button has its own press edge. A long hold through the input
    // gate cannot consume the prompt; a fresh B while holding Start does.
    CourseMap held(0,false,CourseMapEntry::bonus_moving,true);
    for(unsigned t=1;t<=14000;++t)held.advance(true);
    check(!held.finished() && held.frame().records_visible,"Course map waited for a fresh press");
    held.advance(true,true);
    to(held,14023);check(!held.frame().brightness && !held.finished(),"Course map independent B edge");

    CourseMap pages(1,false);
    to(pages,207);pages.advance(false,false,false,true);
    pages.advance(false,false,false,true);
    check(pages.frame().effect==0x45 && pages.frame().record_page==0,"Course record page sound");
    for(int i=0;i<60;++i)pages.advance(false,false,false,true);
    check(pages.frame().record_page==1,"Course record page held without repeat");
    for(unsigned i=2;i<=4;++i){
        pages.advance();pages.advance(false,false,false,true);pages.advance();pages.advance();
        check(pages.frame().record_page==i,"Course record split order");
    }
    pages.advance(false,false,false,true);pages.advance();
    check(pages.frame().effect<0 && pages.frame().record_page==4,"Course record last page clamps silently");
    pages.advance(false,false,true);pages.advance();pages.advance();
    check(pages.frame().record_page==3,"Course record previous split");
    pages.advance(true,false,true);pages.advance();
    check(pages.frame().page_sound && pages.frame().effect==0x46,"Course record browse and confirm together");

    CourseMap skip(1,false,CourseMapEntry::skip);
    check(skip.frame().brightness==15 && skip.frame().skip_offer_visible && skip.frame().start_music,
          "Skipped Bonus keeps the lit board");
    skip.advance();check(skip.frame().effect==0x2f,"Skip confirmation effect");
    skip.advance();check(!skip.frame().skip_offer_visible && skip.frame().panel_shade==25,"Skip panel retract");
    to(skip,17);check(!skip.frame().panel_shade,"Skip panel removed");
    to(skip,146);skip.advance(false,true);skip.advance();
    check(skip.frame().effect==0x46,"Skip first accepted B press");

    constexpr unsigned first[3][3]={{98,98,98},{97,97,98},{97,97,97}};
    for(unsigned league=0;league<3;++league)for(unsigned car=0;car<3;++car){
        CourseMap parked(league,league==2,CourseMapEntry::bonus_parked,false,false,car);
        to(parked,first[league][car]-1);check(!parked.frame().brightness,"Parked map early pixels");
        parked.advance();check(parked.frame().brightness==1,"Parked map board onset");
        check(parked.frame().base_cards==(league!=0),"Parked map first thumbnail transfer");
        to(parked,first[league][car]+109);parked.advance(true);parked.advance();
        check(parked.frame().effect==0x46,"Parked map input gate");
    }
    CourseMap retired(1,false,CourseMapEntry::retire);
    to(retired,97);check(!retired.frame().brightness,"Retired Bonus black before map");
    retired.advance();check(retired.frame().brightness==1,"Retired Bonus board onset");

    for(unsigned league=0;league<3;++league) {
        CourseMap fourth(league,league==2,CourseMapEntry::race_result,false,false,1,4);
        to(fourth,100);check(!fourth.frame().brightness,"Fourth course map black lead-in");
        fourth.advance();check(fourth.frame().base_cards && fourth.frame().brightness==1,"Fourth course first board DMA");
        to(fourth,197);
        check(fourth.frame().panel_left==200 && fourth.frame().panel_right==232,"Fourth course window origin");
        to(fourth,212);
        check(fourth.frame().panel_right==217 && !fourth.frame().records_visible,"Fourth course settled window");
        fourth.advance();check(fourth.frame().records_visible,"Fourth course record pages");
        fourth.advance(true);
        const unsigned confirm=fourth.frame().tick;
        to(fourth,confirm+3);check(fourth.frame().panel_right==219,"Fourth course dismisses from original right edge");
        to(fourth,confirm+(league==2?190:188));check(fourth.finished(),"Fourth course grid handoff");
    }
}
