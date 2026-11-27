#pragma once
#include <cstdint>

namespace srf {
// A player request, separate from the original vehicle control word.
inline constexpr std::uint16_t drift_button=0x100;

class DriftInput {
    bool down_{},pending_{};
public:
    void sample(std::uint16_t buttons,bool driving) {
        const bool down=driving && (buttons&drift_button);
        pending_=driving && (pending_ || (down && !down_));
        down_=down;
    }
    std::uint16_t consume(std::uint16_t buttons) {
        const auto result=(buttons&~drift_button) | (pending_ || down_?drift_button:0);
        pending_=false;
        return static_cast<std::uint16_t>(result);
    }
};
}
