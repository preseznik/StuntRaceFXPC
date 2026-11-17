#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace srf {

enum class CourseMapEntry { bonus_moving, bonus_parked, skip, retire, race_result };

struct CourseMapFrame {
    unsigned tick{}, speed_class{}, brightness{}, board_scroll{}, noise_tick{}, card_step{}, record_car_phase{};
    unsigned record_page{}; // BEST TOTAL, followed by the four course splits.
    bool master_unlocked{}, base_cards{}, records_visible{}, prompt_visible{}, skip_offer_visible{};
    bool play_cancel_visible{},cancel_selected{};
    int class_outline_y=-1;
    unsigned outline_phase{};
    // Inclusive SNES screen coordinates. The three corner scanlines inset by 3, 2 and 1.
    int panel_left=1, panel_right=0, panel_top=0, panel_bottom=-1;
    unsigned panel_shade{}; // Subtract from each 5-bit background color component.
    int effect=-1, music_command=-1;
    bool start_music{}, stop_music{}, start_grid_music{}, page_sound{};

    void panel(int x1,int y1,int x2,int y2,unsigned shade) {
        panel_left=x1>>8;
        panel_right=x2>>8;
        const int top=y1>>8;
        panel_top=(top&~1)+1;
        panel_bottom=panel_top+(((y2>>8)-top-6)&~1)+5;
        panel_shade=shade;
    }
};

class CourseMap {
    struct Timing {
        unsigned first_pixel, previous_song_end, menu_start, menu_song_end, fanfare, grid, car_cycle_origin;
    };
    // Intro times start at the first fully black tally frame. Outgoing times start
    // at the accepted pad edge. Cartridge grid loading varies by about one VBlank.
    static constexpr std::array<Timing,3> moving{{
        {98,59,84,108,173,190,213}, {98,59,84,104,171,186,214}, {98,59,84,106,172,186,215}
    }};
    static constexpr Timing parked[3][3]{
        {{98,59,84,109,174,190,215},{98,59,84,109,175,190,204},{98,59,84,109,175,190,212}},
        {{97,59,83,103,168,185,217},{97,59,83,104,169,186,215},{98,60,84,104,169,186,209}},
        {{97,58,83,107,172,187,213},{97,58,83,107,172,187,213},{97,59,84,106,173,186,230}}
    };
    // First black race-result raster through the next course map and its grid.
    static constexpr std::array<Timing,3> result{{
        {101,63,87,107,173,188,239}, {101,62,87,106,171,188,235}, {101,62,87,106,176,190,243}
    }};
    static constexpr std::array<unsigned,16> opening_shade{
        31,4,4,4,4,5,6,7,8,10,12,14,16,20,24,25
    };

    CourseMapEntry entry_;
    Timing timing_;
    int card_x_;
    CourseMapFrame frame_;
    unsigned confirmed_{};
    unsigned selected_page_{}, queued_page_{};
    bool start_down_{}, b_down_{}, left_down_{}, right_down_{}, page_changed_{}, finished_{};

    // Convert original 8.8 bounds to the paired vertical spans written to HDMA.
    void panel(int x1,int y1,int x2,int y2,unsigned shade) {
        frame_.panel(x1,y1,x2,y2,shade);
    }

    void set_frame() {
        auto& f=frame_;
        const unsigned t=f.tick;
        const bool skip=entry_==CourseMapEntry::skip;
        const unsigned scene=skip?t+61:(t+98)-std::min(t+98,timing_.first_pixel);
        const unsigned after=confirmed_?t-confirmed_:0;
        f.effect=-1;f.music_command=-1;
        const bool result_entry=entry_==CourseMapEntry::race_result;
        if((!skip && t==(result_entry?2u:0u)) ||
           (confirmed_ && after==(result_entry?42u:(f.speed_class==0?46u:41u))))f.music_command=5;
        f.start_music=skip?t==0:t==timing_.menu_start;
        f.stop_music=(!skip && t==timing_.previous_song_end) ||
                     (confirmed_ && after==timing_.menu_song_end);
        f.start_grid_music=confirmed_ && after==timing_.fanfare;
        if(skip && t==1)f.effect=0x2f;
        if(scene==151)f.effect=0x2d;
        if(scene==193)f.effect=0x24;
        if(confirmed_ && after==1){f.effect=0x46;f.music_command=4;}
        f.noise_tick=skip?t:(scene>=96?scene-96:0);
        f.record_car_phase=((t-timing_.car_cycle_origin)&31u)/8;
        f.brightness=skip?15u:scene<98?0u:std::min(15u,1+(scene-98)*3/5);
        f.base_cards=t==timing_.first_pixel &&
            (result_entry || (entry_==CourseMapEntry::bonus_parked && (f.speed_class==1 || (f.speed_class==2 && f.master_unlocked))) ||
             (entry_==CourseMapEntry::retire && f.speed_class==1));
        f.card_step=(scene>=162)+(scene>=164)+(scene>=172)+(scene>=180);
        f.records_visible=scene>=210 && (!confirmed_ || after<3);
        f.prompt_visible=f.records_visible;
        f.skip_offer_visible=skip && t<2;
        f.panel_left=1;f.panel_right=0;f.panel_top=0;f.panel_bottom=-1;f.panel_shade=0;
        // The selected card sits on the lower row for Novice. Expert is on the
        // upper row before Master unlock and the active Master card is on top.
        const int card_y=f.speed_class==0 || (f.speed_class==2 && !f.master_unlocked)?152:72;
        if(skip && t<17){
            const int step=static_cast<int>(t>2?t-2:0);
            const int top=68*256+4, bottom=205*256+244;
            panel(65*256+2+step*802,top+step*((card_y*256-top)/15),
                  208*256+251-step*1109,bottom+step*(((card_y+32)*256-bottom)/15),25);
        } else if(confirmed_ && after>=3 && after<22){
            const int step=static_cast<int>(after)-2;
            const int left=step<=17?57-3*step:std::max(1,2*(20-step)-(result_entry?1:0));
            const int right=std::min(255,(result_entry?217:216)+2*step);
            panel(left*256,(68-step*3)*256,right*256,
                  std::min(224,205+step)*256,std::min(31u,25+after-2));
        } else if(scene>=194 && (!confirmed_ || after<3)){
            const auto step=std::min(15u,scene-194);
            const auto coordinate=[step](int first,int last){
                return first*256+static_cast<int>(step)*((last-first)*256/15);
            };
            panel(coordinate(card_x_,57),coordinate(card_y,68),coordinate(card_x_+32,217),
                  coordinate(card_y+32,206),skip && step==0?25:opening_shade[step]);
        }
        if(confirmed_ && after>=22)f.brightness=0;
        finished_=confirmed_ && after>=timing_.grid;
    }

public:
    CourseMap(unsigned speed_class,bool master_unlocked,CourseMapEntry entry=CourseMapEntry::bonus_moving,
              bool start_down=false,bool b_down=false,unsigned car=1,unsigned round=3):
        entry_(entry),timing_(entry==CourseMapEntry::race_result?result[std::min(speed_class,2u)]:
            entry==CourseMapEntry::bonus_parked?parked[std::min(speed_class,2u)][std::min(car,2u)]:moving[std::min(speed_class,2u)]),
        card_x_(std::array{40,88,152,200}[std::clamp(round,1u,4u)-1]),
        start_down_(start_down),b_down_(b_down) {
        frame_.speed_class=std::min(speed_class,2u);
        frame_.master_unlocked=master_unlocked;
        frame_.board_scroll=frame_.speed_class==2 && master_unlocked?0:80;
        if(entry_==CourseMapEntry::skip || entry_==CourseMapEntry::retire){
            timing_.car_cycle_origin=entry_==CourseMapEntry::skip?178:217;
            if(frame_.speed_class==1){
                timing_.menu_song_end=entry_==CourseMapEntry::skip?102:103;
                timing_.fanfare=entry_==CourseMapEntry::skip?169:168;
                timing_.grid=entry_==CourseMapEntry::skip?184:185;
            }
        }
        set_frame();
    }
    const CourseMapFrame& frame() const {return frame_;}
    bool finished() const {return finished_;}
    void advance(bool start_down=false,bool b_down=false,bool left_down=false,bool right_down=false) {
        if(finished_)return;
        ++frame_.tick;
        frame_.record_page=queued_page_;
        queued_page_=selected_page_;
        frame_.page_sound=page_changed_;
        page_changed_=false;
        const unsigned gate=entry_==CourseMapEntry::skip?147:timing_.first_pixel+110;
        if(!confirmed_ && frame_.tick>=gate){
            const unsigned old=selected_page_;
            if(left_down!=right_down){
                if(right_down && !right_down_ && selected_page_<4)++selected_page_;
                if(left_down && !left_down_ && selected_page_>0)--selected_page_;
            }
            page_changed_=old!=selected_page_;
            if((start_down && !start_down_) || (b_down && !b_down_))confirmed_=frame_.tick;
        }
        start_down_=start_down;
        b_down_=b_down;
        left_down_=left_down;
        right_down_=right_down;
        set_frame();
        if(frame_.page_sound && frame_.effect<0)frame_.effect=0x45;
    }
};
}
