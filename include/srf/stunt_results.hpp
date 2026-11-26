#pragma once
#include "srf/rock_field_rules.hpp"
#include "srf/stunt_record.hpp"
#include <algorithm>
#include <optional>

namespace srf {
enum class StuntPage { drive, time_up, finish_line, finish, records, retry, change_car };
enum class StuntExit { quit, restart, retry_finish, retry_timeout, course };

class StuntResults {
    std::uint16_t previous_=0xffff;
    bool finished_{};
    void set_page(StuntPage next) {page=next;age=0;previous_=0xffff;}
public:
    StuntPage page=StuntPage::drive;
    unsigned age{},choice{};
    StuntRecord score;

    void end_run(const RockFieldRules& rules,const std::string& initials,PlayerCar car) {
        score={rules.stars(),rules.result_seconds(),initials,car};
        finished_=rules.outcome()==RockFieldOutcome::finished;
        set_page(finished_?StuntPage::finish_line:StuntPage::time_up);
    }

    std::optional<StuntExit> advance(std::uint16_t held,PlayerCar& car) {
        ++age;
        const auto pressed=std::uint16_t(held&~previous_);previous_=held;
        switch(page) {
        case StuntPage::time_up:
            if(age>=365)set_page(StuntPage::retry);
            break;
        case StuntPage::finish_line:
            if(age>=205)set_page(StuntPage::finish);
            break;
        case StuntPage::finish:
            if(age>=score.stars+score.seconds && (pressed&0x9080))set_page(StuntPage::records);
            break;
        case StuntPage::records:
            if(age>=15 && (pressed&0x9080))set_page(StuntPage::retry);
            break;
        case StuntPage::change_car: {
            constexpr std::array cars{PlayerCar::f_type,PlayerCar::four_wd,PlayerCar::coupe};
            const auto pos=std::find(cars.begin(),cars.end(),car)-cars.begin();
            if(pressed&0x0300)car=cars[(pos+((pressed&0x0100)?2:1))%3];
            if(pressed&0xd080){choice=0;set_page(StuntPage::retry);}
            break;
        }
        case StuntPage::retry:
            if(pressed&0x0800)choice=(choice+2)%3;
            if(pressed&0x0400)choice=(choice+1)%3;
            if(pressed&0x9080) {
                if(choice==0)return finished_?StuntExit::retry_finish:StuntExit::retry_timeout;
                if(choice==1)return StuntExit::course;
                set_page(StuntPage::change_car);
            }
            break;
        case StuntPage::drive:break;
        }
        return {};
    }
};
}
