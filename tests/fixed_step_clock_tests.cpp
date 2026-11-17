#include "srf/fixed_step_clock.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

namespace {
using Clock = srf::FixedStepClock;
using Ns = std::chrono::nanoseconds;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
// Deliberately nonlinear state and tick-stamped input changes. A frame-clock
// input or a variable physics step produces different intermediate states.
std::vector<std::int64_t> replay(const std::vector<std::int64_t>& frames) {
    Clock clock;
    std::int64_t position=0, velocity=0;
    std::vector<std::int64_t> states;
    const auto step=[&](std::uint64_t tick) {
        require(tick==states.size(), "Skipped/duplicated tick");
        const auto input=tick<37 ? 19 : tick<113 ? -7 : tick<191 ? 0 : 4;
        position+=velocity;
        velocity+=input-velocity/23;
        if(position>12000) {position=12000;velocity=-velocity/2;}
        states.push_back(position*10000+velocity);
    };
    for(auto ns:frames) {clock.accumulate(Ns(ns));clock.run(step);}
    while(clock.pending_steps())clock.run(step);
    require(clock.ticks()==240 && clock.remainder()==0, "Clock lost elapsed time");
    return states;
}
std::vector<std::int64_t> frames(unsigned hz) {
    std::vector<std::int64_t> result;
    std::int64_t last=0;
    for(unsigned i=1;i<=hz*2;++i) {
        const auto now=std::int64_t(i)*Clock::second/hz;
        result.push_back(now-last);last=now;
    }
    return result;
}
void schedule_equivalence() {
    const auto expected=replay(frames(120));
    for(unsigned hz:{30,60,75,90,144,240})require(replay(frames(hz))==expected,"Render-rate-dependent state");
    std::vector<std::int64_t> variable;
    constexpr std::array<std::int64_t,7> pattern{2'000'000,19'000'000,7'000'000,31'000'000,11'000'000,250'000'000,0};
    std::int64_t remaining=2*Clock::second;
    for(unsigned i=0;remaining;++i) {
        const auto dt=std::min(remaining,pattern[i%pattern.size()]);
        variable.push_back(dt);remaining-=dt;
    }
    require(replay(variable)==expected,"Variable frame time or hitch changed state");
}
void boundaries_and_overload() {
    Clock clock;
    const auto step=[](std::uint64_t){};
    clock.accumulate(Ns(8'333'333));
    require(clock.run(step)==0,"Early first tick");
    clock.accumulate(Ns(1));
    require(clock.run(step)==1 && clock.remainder()==80,"Fractional tick boundary lost");
    clock.reset();
    clock.accumulate(Ns(1'000'000'000));
    require(clock.run(step)==8 && clock.pending_steps()==112,"Unbounded catch-up or dropped debt");
    require(clock.run(step,0)==0 && clock.pending_steps()==112,"Zero budget consumed time");
    while(clock.pending_steps())clock.run(step);
    require(clock.ticks()==120 && clock.remainder()==0,"Catch-up changed simulation duration");
    clock.accumulate(Ns(123));
    clock.reset();
    require(clock.ticks()==0 && clock.pending_steps()==0 && clock.remainder()==0,"Restart retained time");
    bool negative=false,overflow=false;
    try {clock.accumulate(Ns(-1));}catch(const std::invalid_argument&){negative=true;}
    try {clock.accumulate(Ns::max());}catch(const std::overflow_error&){overflow=true;}
    require(negative && overflow && clock.ticks()==0 && clock.remainder()==0,"Invalid elapsed time mutated clock");
}
}
int main() {
    try {
        schedule_equivalence();boundaries_and_overload();
        std::cout<<"Fixed 120 Hz clock: render schedules, tick inputs, catch-up and reset passed\n";
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
