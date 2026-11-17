#pragma once

#include "srf/native_race.hpp"
#include <algorithm>
#include <string>

namespace srf {
enum class BootMenuScreen { game, car, initials };
enum class BootMenuEntry { title, demo };
enum class BootMenuEvent { none, title, board, unavailable };
struct BootMenuFrame {
    BootMenuScreen screen=BootMenuScreen::game;
    BootMenuEntry entry=BootMenuEntry::title;
    PlayerCar car=PlayerCar::four_wd;
    std::string initials="AAA";
    unsigned tick{}, mode{}, cursor{}, turntable{}, lettering{}, brightness=15;
    bool bright_arrows{},gold_stats{}, switching_stats{};
    unsigned noise=4, arrow_offset{}, header_scroll{}, panel_scroll=168;
    bool show_choices=true, show_car=false, entering=true;
    int effect=-1, music_command=-1;
};

class BootMenu {
    BootMenuFrame f_;
    BootMenuEvent event_{};
    unsigned held_{},repeat_{},transition_{},clock_{},car_clock_{};
    enum class Transition { none,to_car,to_game,to_board } transition_kind_{};
    void move_car(int step) {
        static constexpr PlayerCar order[]{PlayerCar::f_type,PlayerCar::four_wd,PlayerCar::coupe};
        const int pos=int(std::find(std::begin(order),std::end(order),f_.car)-std::begin(order));
        f_.car=order[(pos+step+3)%3];
    }
    void stable_car() {
        f_.screen=BootMenuScreen::car; f_.header_scroll=f_.panel_scroll=256;
        f_.arrow_offset=88;f_.show_choices=f_.show_car=true;f_.switching_stats=false;
    }
public:
    explicit BootMenu(PlayerCar initial_car=PlayerCar::four_wd,std::string initials="AAA",
                      BootMenuEntry entry=BootMenuEntry::title) {
        f_.car=initial_car;
        f_.entry=entry;
        if(entry==BootMenuEntry::demo)f_.brightness=0;
        if(initials.size()==3 && std::all_of(initials.begin(),initials.end(),[](char c){return c>='A' && c<='Z';}))
            f_.initials=std::move(initials);
    }
    const BootMenuFrame& frame() const {return f_;}
    BootMenuEvent event() const {return event_;}
    PlayerCar car() const {return f_.car;}
    const std::string& initials() const {return f_.initials;}
    void resume_car(unsigned mode=0) {
        transition_kind_=Transition::none;event_=BootMenuEvent::none;f_.brightness=15;
        f_.mode=mode;f_.entering=false;stable_car();held_=0xffff;repeat_=0;
    }
    void advance(std::uint16_t held_snes) {
        const unsigned edges=held_snes&~held_;
        const unsigned direction=held_snes&0x0f00;
        if(direction!=(held_&0x0f00))repeat_=0;else if(direction)++repeat_;
        held_=held_snes;event_=BootMenuEvent::none;f_.effect=f_.music_command=-1;
        ++f_.tick; ++clock_;
        if(f_.entry==BootMenuEntry::demo && f_.tick<=47)
            f_.brightness=f_.tick<19?0:1+(f_.tick-19)/2;
        f_.noise=((f_.entry==BootMenuEntry::demo?3:4)+f_.tick)%8;
        f_.lettering=f_.entry==BootMenuEntry::demo && f_.tick==42?0:clock_%256;
        if(f_.entering) {
            f_.lettering=0;
            if(f_.tick==39)f_.effect=0x19;
            if(f_.tick<41)return;
            f_.entering=false;clock_=254;
        }
        if(f_.show_car && transition_kind_!=Transition::to_board) {
            ++car_clock_;
            // The Super FX can finish two Coup� poses in three fields each.
            const bool coupe=f_.car==PlayerCar::coupe;
            const unsigned age=car_clock_%(coupe?254:256);
            f_.turntable=(age+(coupe && age>=123)+(coupe && age>=130))/4;
            const unsigned dma=car_clock_%4;
            if(dma==2 || dma==3)f_.noise=(f_.noise+7)%8;
            f_.bright_arrows=f_.screen==BootMenuScreen::car && ((car_clock_+23)%24)<12;
            f_.gold_stats=((car_clock_+7)%10)<5;
        }
        if(transition_kind_!=Transition::none) {
            ++transition_;
            if(transition_kind_==Transition::to_board) {
                f_.brightness=15-std::min(15u,transition_/2);
                if(transition_==30){event_=BootMenuEvent::board;transition_kind_=Transition::none;}
                return;
            }
            const bool forward=transition_kind_==Transition::to_car;
            const unsigned t=forward?transition_:51-std::min(51u,transition_+29);
            f_.show_choices=t<18; f_.show_car=false;
            constexpr unsigned scroll[]{0,0,3,4,6,8,11,15,19,24,29,35,42,49,56,64,72,81,90,99,108,117,
                126,135,144,153,162,171,180,188,196,204,211,218,225,231,236,241,245,248,251,253,255,255,256};
            f_.header_scroll=scroll[std::min(44u,t)];
            f_.panel_scroll=std::max(168u,f_.header_scroll);
            f_.arrow_offset=(t>=42 && t<47)?87:f_.panel_scroll-168;
            f_.switching_stats=t>=29;
            if(forward && transition_>=51){stable_car();f_.turntable=3;car_clock_=13;transition_kind_=Transition::none;}
            if(!forward && transition_>=22){f_.screen=BootMenuScreen::game;f_.show_choices=true;clock_=0;transition_kind_=Transition::none;}
            return;
        }
        const bool confirm=edges&(0x1000|0x8000), back=edges&0x4000;
        if(f_.screen==BootMenuScreen::game) {
            if(edges&(0x0800|0x0400)) {
                f_.mode=(f_.mode+((edges&0x0800)?3:1))%4;clock_=f_.lettering=0;f_.effect=0x45;
            } else if(confirm) {
                f_.effect=0x46;
                if(f_.mode>1)event_=BootMenuEvent::unavailable;
                else {transition_kind_=Transition::to_car;transition_=0;}
            }
            return;
        }
        if(f_.mode==0 && (edges&0x2000)) {event_=BootMenuEvent::unavailable;f_.effect=0x46;return;}
        if(f_.screen==BootMenuScreen::car) {
            if(back){transition_kind_=Transition::to_game;transition_=0;f_.effect=0x47;}
            else if(confirm){f_.screen=BootMenuScreen::initials;f_.cursor=0;f_.effect=0x46;}
            else if(edges&(0x0200|0x0100)){move_car((edges&0x0100)?1:-1);f_.effect=0x45;}
            return;
        }
        if(back){f_.screen=BootMenuScreen::car;f_.effect=0x47;}
        else if(confirm){transition_kind_=Transition::to_board;transition_=0;f_.effect=0x46;f_.music_command=4;}
        else if(edges&(0x0200|0x0100)){
            const unsigned next=(edges&0x0100)?std::min(2u,f_.cursor+1):f_.cursor?f_.cursor-1:0u;
            if(next!=f_.cursor){f_.cursor=next;f_.effect=0x46;}
        } else if((edges|(repeat_ && repeat_%12==0?direction:0))&(0x0800|0x0400)) {
            auto& c=f_.initials[f_.cursor];c=char('A'+(c-'A'+((direction&0x0800)?1:25))%26);f_.effect=0x45;
        }
    }
};
}
