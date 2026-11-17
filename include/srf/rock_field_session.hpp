#pragma once
#include "srf/rock_field.hpp"
#include "srf/rock_field_rules.hpp"
#include "srf/stunt_course.hpp"

namespace srf {
class RockFieldSession {
    // One credit per 14/60.0988 of a vehicle step. Integer accumulation keeps
    // the original field clock independent of rendering and floating-point drift.
    unsigned phase_credit_{};
public:
    static constexpr double field_rate=60.0988;
    RockFieldDrive drive;
    RockFieldRules rules;
    unsigned tick{},released{};
    RockFieldSession(const Rom& rom,PlayerCar car,StuntCourse course=StuntCourse::rock_field,
                     StuntAttempt attempt=StuntAttempt::first)
        :drive(rom,car,course,attempt),rules(rom,drive.scene(),car) {}

    bool advance(std::uint16_t requested,std::optional<std::int16_t> steering={}) {
        if(rules.outcome()!=RockFieldOutcome::driving)return false;
        ++tick;
        if(tick>=drive.first_clock_field()) {
            ++released;
            rules.advance_clock(1);
        }
        if(drive.drive_started()) {
            phase_credit_+=140000;
            if(phase_credit_<600988)return false;
            phase_credit_-=600988;
            drive.set_event_contact(rules.body_contact(drive.player().body));
            drive.step(requested,steering);
        } else {
            drive.set_event_contact(rules.body_contact(drive.player().body));
            if(!drive.advance_intro(requested,steering))return false;
        }
        const auto section=rules.section();
        rules.step(drive.player().body,0);
        if(rules.section()!=section)drive.checkpoint_reward(rules.last_gate_command());
        return true;
    }
};
}
