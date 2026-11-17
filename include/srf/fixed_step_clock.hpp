#pragma once
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace srf {
// Rendering contributes elapsed time; only completed fixed steps advance the
// simulation. Catch-up is bounded per call, and excess work remains owed.
// Input/replays use the zero-based tick passed to the callback, never a frame ID.
class FixedStepClock {
public:
    static constexpr std::uint64_t frequency = 120;
    static constexpr std::uint64_t second = 1'000'000'000;
    static constexpr unsigned default_step_budget = 8;

    void reset() { credit_ = ticks_ = 0; }
    void accumulate(std::chrono::nanoseconds elapsed) {
        if (elapsed.count() < 0) throw std::invalid_argument("Negative elapsed time");
        const auto ns = static_cast<std::uint64_t>(elapsed.count());
        if (ns > (std::numeric_limits<std::uint64_t>::max() - credit_) / frequency)
            throw std::overflow_error("Simulation clock overflow");
        credit_ += ns * frequency;
    }
    template<class Step>
    unsigned run(Step&& step, unsigned budget = default_step_budget) {
        unsigned count = 0;
        while (count < budget && credit_ >= second) {
            step(ticks_);
            ++ticks_;
            credit_ -= second;
            ++count;
        }
        return count;
    }
    std::uint64_t ticks() const { return ticks_; }
    std::uint64_t pending_steps() const { return credit_ / second; }
    // Fractional wall time is scheduling credit, not a pose interpolation factor.
    std::uint64_t remainder() const { return credit_ % second; }
private:
    std::uint64_t credit_{}, ticks_{};
};
}
