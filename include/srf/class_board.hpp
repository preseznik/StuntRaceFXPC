#pragma once
#include "srf/course_map.hpp"

namespace srf {
// First black raster after the driver accepts their initials. Up selects the
// class above the cursor; Master is available only after clearing Expert.
class ClassBoard {
    enum class View { choose, records, cancel, start };
    View view_=View::choose;
    CourseMapFrame f_;
    unsigned tick_{},entered_{},moved_{},old_class_{},league_{},scroll_from_=80,scroll_to_=80;
    unsigned page_{},queued_page_{},shown_page_{};
    unsigned input_gate_=118,outline_on_=121,ring_phase_=1;
    bool cancel_{},queued_cancel_{},shown_cancel_{},unlocked_{},finished_{},reopened_{};
    std::uint16_t held_{};
    int next_effect_=-1;

    void change(View view) {view_=view;entered_=tick_;}
    void move_class(unsigned league) {
        old_class_=league_;league_=league;moved_=tick_;
        scroll_from_=f_.board_scroll;scroll_to_=scroll_from_;
        if(league_==2)scroll_to_=0;
        if(league_==0)scroll_to_=80;
        input_gate_=tick_+(scroll_to_==scroll_from_?1:22);
        next_effect_=0x45;
    }
    void input(std::uint16_t edge) {
        const bool accept=(edge&0x9000)!=0;
        const unsigned age=tick_-entered_;
        if(view_==View::choose && tick_>=input_gate_) {
            if(accept) {
                change(View::records);
                page_=queued_page_=shown_page_=0;
                cancel_=queued_cancel_=shown_cancel_=false;
                next_effect_=0x46;
            } else if((edge&0x0800) && league_<(unlocked_?2u:1u))move_class(league_+1);
            else if((edge&0x0400) && league_)move_class(league_-1);
        } else if(view_==View::records && age>=88) {
            if((edge&0x0100) && page_<4){++page_;next_effect_=0x45;}
            else if((edge&0x0200) && page_){--page_;next_effect_=0x45;}
            if(age<120)return;
            if((edge&0x4000) || (accept && cancel_)) {
                change(View::cancel);next_effect_=0x2f;
            } else if(accept) {
                change(View::start);next_effect_=0x46;
            } else if((edge&0x0400) && !cancel_){cancel_=true;next_effect_=0x45;}
            else if((edge&0x0800) && cancel_){cancel_=false;next_effect_=0x45;}
        }
    }
    void opening_window(unsigned step,int y,unsigned shade) {
        const auto coord=[step](int from,int to){return from*256+int(step)*((to-from)*256/15);};
        f_.panel(coord(40,57),coord(y,68),coord(72,217),coord(y+32,206),shade);
    }
    void paint() {
        auto& f=f_;
        f.tick=tick_;f.speed_class=league_;f.master_unlocked=unlocked_;
        f.brightness=tick_<96?0u:std::min(15u,1+(tick_-96)*3/5);
        f.base_cards=tick_==96;
        f.noise_tick=tick_>=94?tick_-94:0;
        f.record_car_phase=((tick_-11)&31)/8;
        f.card_step=0;f.records_visible=f.play_cancel_visible=false;f.class_outline_y=-1;
        f.panel_left=1;f.panel_right=0;f.panel_top=0;f.panel_bottom=-1;f.panel_shade=0;
        f.record_page=shown_page_;f.cancel_selected=shown_cancel_;
        const unsigned age=tick_-entered_,move_age=tick_-moved_;
        const auto distance=int(std::min(20u,move_age>=3?move_age-2:0))*4;
        f.board_scroll=scroll_from_<scroll_to_?std::min(scroll_to_,scroll_from_+distance):
                     std::max(int(scroll_to_),int(scroll_from_)-distance);
        const int row_y=232-80*int(league_)-int(f.board_scroll);
        if(view_==View::choose || (view_==View::records && age<2)) {
            const bool scrolling=scroll_from_!=scroll_to_ && move_age>=3 && move_age<23;
            if(tick_>=outline_on_ && !scrolling) {
                const unsigned row=moved_ && move_age<4?old_class_:league_;
                f.class_outline_y=217-80*int(row)-int(f.board_scroll);
                f.outline_phase=ring_phase_;
            }
            return;
        }
        if(view_==View::records) {
            f.card_step=(age>=42)+(age>=44)+(age>=52)+(age>=60);
            if(age==31)f.effect=0x2d;
            if(age==73)f.effect=0x24;
            if(age<74)return;
            const unsigned step=std::min(15u,age-74);
            constexpr unsigned shade[]{0,4,4,4,4,5,6,7,8,10,12,14,16,20,24,25};
            opening_window(step,row_y,step==0 && reopened_?25:shade[step]);
            f.records_visible=f.play_cancel_visible=age>=90;
            return;
        }
        f.records_visible=f.play_cancel_visible=age<2;
        if(view_==View::cancel) {
            f.card_step=4-(age>=34)-(age>=42)-(age>=50)-(age>=52);
            if(age<17)opening_window(15-(age>2?age-2:0),row_y,25);
            if(age==16)f.effect=0x2d;
            return;
        }
        f.card_step=4;
        if(age<3)opening_window(15,row_y,25);
        else if(age<22) {
            const int step=int(age)-2;
            f.panel(std::max(2*(20-step),56-3*step)*256,(68-step*3)*256,(216+2*step)*256,
                    std::min(224,205+step)*256,std::min(31u,25+age-2));
        } else f.brightness=0;
        if(age==1)f.music_command=4;
        if(age==41)f.music_command=5;
        constexpr unsigned stop[]{107,102,105},music[]{172,168,172},grid[]{189,184,187};
        f.stop_music=age==stop[league_];f.start_grid_music=age==music[league_];
        finished_=age>=grid[league_];
    }
public:
    explicit ClassBoard(bool master_unlocked,unsigned initial_class=0):
        league_(std::min(initial_class,master_unlocked?2u:1u)),
        scroll_from_(league_==2?0u:80u),scroll_to_(scroll_from_),unlocked_(master_unlocked) {paint();}
    const CourseMapFrame& frame() const {return f_;}
    bool finished() const {return finished_;}
    bool choosing_play() const {return view_==View::records && tick_-entered_>=120;}
    // Keyboard and controller levels are sampled at the same SNES video rate.
    void advance(std::uint16_t pad=0) {
        if(finished_)return;
        ++tick_;
        f_.effect=next_effect_;next_effect_=-1;f_.music_command=tick_==1?5:-1;
        f_.start_music=tick_==83;f_.stop_music=tick_==58;f_.start_grid_music=false;
        shown_page_=queued_page_;queued_page_=page_;
        shown_cancel_=queued_cancel_;queued_cancel_=cancel_;
        if(view_==View::cancel && tick_-entered_==58) {
            change(View::choose);input_gate_=tick_;outline_on_=tick_+2;reopened_=true;
        }
        const auto move_age=tick_-moved_;
        const bool scrolling=scroll_from_!=scroll_to_ && move_age>=3 && move_age<23;
        if(view_==View::choose && tick_>=120 && !scrolling && (tick_&3)==0)ring_phase_=(ring_phase_+2)%3;
        input(static_cast<std::uint16_t>(pad&~held_));held_=pad;
        paint();
    }
};
}
