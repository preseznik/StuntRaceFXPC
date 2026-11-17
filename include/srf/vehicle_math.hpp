#pragma once

#include <array>
#include <cstdint>

namespace srf {
// Native arithmetic recovered from GSU 04:B920..B954. This projects velocity;
// it is not the acceleration, steering, collision or vehicle update routine.
std::int16_t project_speed(const std::array<std::int16_t,3>& velocity,
                          const std::array<std::int16_t,3>& coefficients);
}
