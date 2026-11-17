#pragma once

#include "srf/rom.hpp"
#include <array>

namespace srf {

struct RainPixel {
    std::uint8_t x{}, y{};
    bool operator==(const RainPixel &) const = default;
};

// Original single-player 208 x 128 race viewport, palette index 42.
class RainParticles {
  public:
    explicit RainParticles(const Rom &rom);
    void reset(std::uint16_t random_seed = 0x1234);
    void step(bool raining, std::int16_t camera_yaw, std::int16_t forward_speed);
    std::span<const RainPixel> pixels() const { return pixels_; }

  private:
    struct Drop {
        int life{}, x{}, y{}, z{}, previous_x{}, previous_y{};
    };
    std::array<Drop, 80> drops_{};
    std::array<std::int16_t, 511> projection_{};
    std::vector<RainPixel> pixels_;
    std::uint16_t random_{};
    std::int16_t last_yaw_{};
    int wind_{};
    bool active_{};
};

} // namespace srf
