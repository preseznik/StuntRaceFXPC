#pragma once
#include <algorithm>

namespace srf {
enum class BootTitleEntry { cold, demo_return };

struct BootTitleFrame {
    unsigned tick{}, brightness{}, car_weight{};
    int logo_scroll{}, logo_top{}, effect=-1, music_command=-1;
    bool nintendo{}, checker{}, logo{}, copyright{}, prompt{}, start_music{}, stop_music{};
};

class BootTitle {
    BootTitleFrame frame_{};
    BootTitleEntry entry_;
    unsigned confirmed_{};
    bool previous_start_{},previous_b_{},idle_exit_{};

    // The title falls, then bounces three times. The original changes its
    // integer vertical velocity every other video field.
    static int bounce(unsigned age,int speed) {
        int height=0;
        for(unsigned i=0;i<age;++i)height+=speed-static_cast<int>(i/2);
        return height;
    }
    static int entering(unsigned tick) {
        if(tick<401){
            const int n=static_cast<int>(tick-381);
            return 128-2*n-(n/2)*((n-1)/2);
        }
        if(tick<435)return bounce(tick-401,8);
        if(tick<457)return bounce(tick-435,5);
        if(tick<467)return bounce(tick-457,2);
        return 0;
    }
    static int departing(unsigned age) {
        if(age<2)return 0;
        const int n=static_cast<int>(age-1);
        return 10*n-1-(n/2)*((n+1)/2);
    }
public:
    // After a demo the last three controller samples consume the first three
    // black fields. Keep their elapsed time on the warm title's video clock.
    explicit BootTitle(BootTitleEntry entry=BootTitleEntry::cold):entry_(entry){
        if(entry==BootTitleEntry::demo_return)frame_.tick=3;
    }
    const BootTitleFrame& frame() const{return frame_;}
    bool to_demo() const{return idle_exit_;}
    bool finished() const{
        if(idle_exit_)return frame_.tick>=(entry_==BootTitleEntry::cold?1547u:1407u);
        return confirmed_ && frame_.tick-confirmed_>=135;
    }
    void advance(bool start_down,bool b_down=false) {
        if(finished())return;
        auto& f=frame_;
        const unsigned tick=++f.tick;
        const bool warm=entry_==BootTitleEntry::demo_return;
        const unsigned t=tick+(warm?139:0); // Bounce, car palette and prompt share this clock.
        const unsigned lit=warm?115:251;
        const bool press=(start_down && !previous_start_) || (b_down && !previous_b_);
        previous_start_=start_down;
        previous_b_=b_down;
        if(!confirmed_ && tick==(warm?1379u:1518u))idle_exit_=true;
        if(!confirmed_ && t>=494 && press)confirmed_=tick;
        const unsigned age=confirmed_?tick-confirmed_:0;
        f.nintendo=!warm && tick>=75 && tick<194;
        f.checker=tick>=lit;
        f.brightness=f.nintendo?15:f.checker?std::min(15u,1+(tick-lit)/2):0;
        const unsigned dim=warm?1379:1519;
        if(idle_exit_ && tick>=dim)f.brightness=14-std::min(14u,(tick-dim)/2);
        f.car_weight=t<317?0:std::min(32u,(t-315)/2);
        if(age>=59)f.car_weight=age<90?90-age:0;
        f.logo=t>=381 && (!confirmed_ || age<57);
        f.logo_scroll=f.logo?(confirmed_?departing(age):entering(t)):0;
        // The outgoing screen-mask change clips the wordmark above line 60.
        f.logo_top=age>=44?60:0;
        f.copyright=f.checker && (!confirmed_ || age==0);
        f.prompt=f.copyright && t>=516 && ((t-516)/21)%2==0;
        f.start_music=tick==(warm?93u:231u);
        f.stop_music=warm && tick==66;
        f.music_command=warm && tick==4?0x05:-1;
        f.effect=-1;
        if(!warm && t==72)f.effect=0x39;
        if(t==400 || t==434 || t==456 || t==466)f.effect=0x19;
        if(confirmed_ && age==0)f.effect=0x46;
        if(confirmed_ && age==14)f.effect=0x13;
    }
};
}
