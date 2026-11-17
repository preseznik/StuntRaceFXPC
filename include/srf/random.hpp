#pragma once
#include <array>
#include <cstdint>

namespace srf {
// Native semantics of $02:DF7D, wrapped by $02:DF79. State is DP $E6..$E9.
// Gameplay calls must have decimal arithmetic disabled. No CPU state or
// instruction interpreter is part of this interface.
std::uint8_t advance_random(std::array<std::uint8_t, 4>& state);
}
