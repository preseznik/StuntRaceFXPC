#include "srf/rain.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace srf {
namespace {
// GSU DIV2 maps -1 to zero and otherwise shifts the signed word.
int halve(int value) { return value == -1 ? 0 : value >> 1; }
int shift(int value, unsigned count) {
    while (count--)
        value = halve(value);
    return value;
}

// GSU swap, rotate right through carry, then ADD and ADC. The low word
// after ADD supplies the position bits; ADC updates the persistent state.
unsigned advance_random(std::uint16_t &state, bool &carry) {
    const auto swapped = static_cast<std::uint16_t>((state >> 8) | (state << 8));
    const unsigned value = (swapped >> 1) | (unsigned(carry) << 15);
    const unsigned sum = value + state;
    const unsigned next = state + (sum & 65535) + (sum >> 16);
    carry = next > 65535;
    state = static_cast<std::uint16_t>(next);
    return sum & 65535;
}

// GSU01:BEF1/BF06 plot both endpoints and change the minor coordinate
// only when the original 16-bit error subtraction borrows.
bool plot_line(std::vector<RainPixel> &out, int x, int y, int end_x, int end_y) {
    const int dx = std::abs(end_x - x), dy = std::abs(end_y - y);
    const int x_step = end_x >= x ? 1 : -1, y_step = end_y >= y ? 1 : -1;
    const bool vertical = dy > dx;
    const int major = std::max(dx, dy), minor = std::min(dx, dy);
    int error = (major - minor) / 2;
    for (int i = 0; i <= major; ++i) {
        out.push_back({static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)});
        error -= minor;
        if (error < 0) {
            error += major;
            if (vertical)
                x += x_step;
            else
                y += y_step;
        }
        if (vertical)
            y += y_step;
        else
            x += x_step;
    }
    return error >= minor;
}
} // namespace

RainParticles::RainParticles(const Rom &rom) {
    // BE34..BE45 indexes the same camera reciprocals used for polygon projection.
    const auto start = Rom::lorom_offset(0x14896c) + 300 * 2;
    for (std::size_t z = 0; z < projection_.size(); ++z)
        projection_[z] = std::bit_cast<std::int16_t>(rom.u16(start + z * 2));
    reset();
}
void RainParticles::reset(std::uint16_t random_seed) {
    active_ = false;
    last_yaw_ = 0;
    wind_ = 0;
    random_ = random_seed; // Shared GSU seed at 70:01D0 in the original.
    pixels_.clear();
}
void RainParticles::step(bool raining, std::int16_t camera_yaw, std::int16_t forward_speed) {
    pixels_.clear();
    if (!raining) {
        active_ = false;
        return;
    }
    if (!active_) {
        // 01:BC35 initializes 80 ten-byte records and staggers their lifetimes.
        for (unsigned i = 0; i < drops_.size(); ++i)
            drops_[i] = {-int(i & 63)};
        wind_ = 0;
        last_yaw_ = 0;
        active_ = true;
        return;
    }
    bool carry = true;
    const auto roll = advance_random(random_, carry);
    if (!(roll & 31))
        wind_ += shift(int((roll >> 8) & 63) - 31 - wind_, 3);
    const auto delta = static_cast<std::int16_t>(static_cast<std::uint16_t>(camera_yaw - last_yaw_));
    const int drift = wind_ - shift(delta, 5);
    last_yaw_ = camera_yaw;
    if (forward_speed < 0)
        return;
    const int depth_delta = shift(-forward_speed, 3);
    carry = (shift(-forward_speed, 2) & 1) != 0;
    // ROR uses the carry left by the previous drop, including the final
    // subtraction after drawing a streak or rejecting its projection.
    for (auto &d : drops_) {
        if (d.life < 0) {
            ++d.life;
            carry = false;
            continue;
        }
        if (!d.life) {
            // First roll only advances the seed. Rain sets the depth register
            // from the loaded palette index 42 before the second roll picks X.
            advance_random(random_, carry);
            const auto x = advance_random(random_, carry);
            d = {30, int(x & 1023) - 511, -85, 42};
            carry = true;
            continue;
        }
        --d.life;
        d.x += drift;
        if (d.x >= 511 || d.x < -511) {
            d.x = d.x < 0 ? 510 : -511;
            d.previous_y = 0;
            carry = true;
            continue;
        }
        d.y += 40;
        if (d.y >= 255 || d.y < -255) {
            d.y = d.y < 0 ? 254 : -255;
            d.previous_y = 0;
            carry = true;
            continue;
        }
        d.z += depth_delta;
        if (d.z < 0 || d.z >= 511) {
            d.z = d.z < 0 ? 511 : 0;
            d.previous_y = 0;
            carry = true;
            continue;
        }
        const int x = 104 + ((d.x * projection_[d.z]) >> 15);
        const int y = 44 + ((d.y * projection_[d.z]) >> 15);
        if (x <= 0 || x > 205 || y <= 0 || y > 124) {
            carry = true;
            if (y > 124 && x > 0 && x <= 205) {
                d.life = 1;
                carry = false;
            }
            d.previous_y = 0;
            continue;
        }
        carry = d.previous_y ? plot_line(pixels_, x, y, d.previous_x, d.previous_y) : true;
        d.previous_x = x;
        d.previous_y = y;
    }
}
} // namespace srf
