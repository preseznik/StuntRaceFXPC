#pragma once
#include "srf/demo_drive.hpp"
#include <cstdlib>
#include <stdexcept>

inline void check_demo_drive(const srf::Rom& rom) {
    srf::DemoDrive drive(rom);
    auto mix=[](std::uint32_t& hash,unsigned word){hash=(hash^word)*16777619u;};
    std::uint32_t chassis=2166136261u,wheels=2166136261u,fields=2166136261u;
    for(unsigned phase=1;phase<=drive.last_phase;++phase) {
        const auto& car=drive.car_at(phase);
        for(unsigned offset:{0x1e,0x20,0x22,0x24,0x26,0x28,0x2e,0x30,0x32,0x34,0x36,0x38,0x3a,0x42,0x4a,0x58,0x86})
            mix(chassis,car.body.word(offset));
        for(const auto& wheel:car.wheels)
            for(unsigned offset:{0x1e,0x20,0x22,0x24,0x26,0x28})
                mix(wheels,wheel.word(offset));
    }
    // Cold cartridge, all 463 completed GSU phases, body and four wheels.
    if(chassis!=0xcf266b8fu || wheels!=0x920ac6f7u) throw std::runtime_error("Original demo trajectory");
    for(unsigned tick=0;tick<1645;++tick) {
        if(drive.tick()!=tick) throw std::runtime_error("Demo video field skipped");
        mix(fields,drive.phase());
        mix(fields,drive.display_phase());
        mix(fields,std::abs(static_cast<int>(drive.dashboard_speed()))*15/64);
        drive.advance();
    }
    // Independent physics clock, visible BG2 page, and MPH read from actual
    // OAM digit pixels for every lit field (cold 1604..3248).
    if(fields!=0x50f59347u || drive.phase()!=463 || drive.car_at(0).body.word(0x22)!=1472)
        throw std::runtime_error("Original demo field timing or speed digits");
}
