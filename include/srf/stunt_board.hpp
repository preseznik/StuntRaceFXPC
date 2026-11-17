#pragma once
#include "srf/stunt_course.hpp"
#include "srf/native_race.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace srf {
class StuntBoard {
public:
    enum class Page { courses, records, data, loading };
    static constexpr std::array<std::string_view,4> names{"ROCK FIELD","UP'N DOWN","ICE DANCE","BLUE LAKE"};
    Page page=Page::courses;
    unsigned tick{},age{},course{},choice{};
    int effect=-1;
    bool unavailable{};
    explicit StuntBoard(PlayerCar car=PlayerCar::four_wd):load_fields_(car==PlayerCar::coupe?187:188){}
    unsigned brightness() const {
        if(tick<95)return 0;
        if(page==Page::loading)return age>=22?0:std::min(15u,22-age);
        return std::min(15u,1+3*(tick-95)/5);
    }
    bool ready_to_drive() const {
        // First visible Ice Dance trailer field after accepting PLAY, in all three cars.
        if(course==static_cast<unsigned>(StuntCourse::ice_dance))return page==Page::loading && age>=191;
        const auto extra_field=course==static_cast<unsigned>(StuntCourse::up_n_down)?1u:0u;
        return page==Page::loading && age>=load_fields_+extra_field;
    }
    bool outline_visible() const {return page==Page::courses && tick>=118;}
    bool panel_visible() const {return page==Page::records && age>=17;}
    void advance(std::uint16_t held) {
        unsigned edge=held&~held_;held_=held;++tick;++age;effect=-1;unavailable=false;
        if(tick<118 || page==Page::loading)return;
        const bool accept=edge&0x9000,cancel=edge&0x4000;
        if(page==Page::courses) {
            const unsigned before=course;
            if(accept){choice=0;change(Page::records,0x46);}
            else if(edge&0x0800)course|=2;
            else if(edge&0x0400)course&=~2u;
            else if(edge&0x0100)course|=1;
            else if(edge&0x0200)course&=~1u;
            if(course!=before)effect=0x45;
        } else if(page==Page::records && age>=17) {
            if(cancel || (accept && choice==2))change(Page::courses,0x2f);
            else if(accept && choice==1)change(Page::data,0x46);
            else if(accept && course<playable_stunt_courses.size())change(Page::loading,0x46);
            else if(accept){unavailable=true;effect=0x46;}
            else if((edge&0x0400) && choice<2){++choice;effect=0x45;}
            else if((edge&0x0800) && choice){--choice;effect=0x45;}
        } else if(page==Page::data && (cancel || accept))change(Page::records,0x2f);
    }
private:
    unsigned load_fields_;
    std::uint16_t held_=0xffff;
    void change(Page next,int sound){page=next;age=0;effect=sound;}
};
}
