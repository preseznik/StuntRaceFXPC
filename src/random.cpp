#include "srf/random.hpp"

namespace srf {
std::uint8_t advance_random(std::array<std::uint8_t, 4>& state) {
    unsigned accumulator = state[0];
    unsigned borrow = 1; // CLC before the first SBC means subtract one extra.
    for (unsigned index : {1U, 2U, 3U, 0U}) {
        const unsigned subtrahend = static_cast<unsigned>(state[index]) + borrow;
        borrow = accumulator < subtrahend ? 1U : 0U;
        accumulator = (accumulator - subtrahend) & 0xffU;
        state[index] = static_cast<std::uint8_t>(accumulator);
    }
    return state[0];
}
}
