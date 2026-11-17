#pragma once

#include "srf/course.hpp"
#include "srf/rom.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace srf {

// Original BG2 panorama, decoded from the verified ROM. The original loader
// repeats a fifth 256-pixel page for scrolling; native sampling wraps at 1024.
struct CourseBackdrop {
    static constexpr unsigned width = 1024;
    static constexpr unsigned height = 256;
    std::array<std::vector<std::uint8_t>, 2> indices;
    std::array<std::array<std::uint16_t, 16>, 2> palettes{};
    // HDMA replaces CGRAM color 1 with this ramp on every visible scanline.
    std::array<std::array<std::uint16_t, 512>, 2> gradients{};
    // Night Cruise cycles four city lights in the alternate palette.
    std::optional<std::array<std::array<std::uint16_t,4>,22>> city_lights;
    // Harbor City writes the low byte of color 15 from one of two ROM cycles.
    std::optional<std::array<std::array<std::uint8_t,22>,2>> harbor_lights;
    // CPU03:EF1F reserves palette slots when weather blends toward white.
    std::array<std::uint16_t,16> fog_protected{};
    bool available() const { return !indices[0].empty(); }
};

// Supported panoramas have been compared against original tilemaps and glyphs.
CourseBackdrop extract_course_backdrop(const Rom& rom, CourseId course);
CourseBackdrop extract_easy_ride_demo_backdrop(const Rom& rom);
CourseBackdrop extract_speed_bonus_backdrop(const Rom& rom);
std::vector<std::uint8_t> extract_start_light_tiles(const Rom& rom);
// Reverse-bit graphics streams use an end pointer in the cartridge's LoROM address space.
std::vector<std::uint8_t> unpack_rom_graphics(const Rom& rom, std::uint32_t end_address);
std::uint16_t backdrop_palette_target(const CourseBackdrop& backdrop,unsigned index,unsigned skyline_phase);
std::uint16_t backdrop_color(const CourseBackdrop& backdrop, unsigned index,
                             unsigned palette_weight, unsigned skyline_phase,
                             unsigned skyline_blend_phase = 0);

struct BackdropView {
    int x = 0, y = 0, gradient_y = 0;
};

// Original 208x128 race view, whose top-left pixel is SNES (24,32).
// Angles are signed original camera words (65536 units per full turn).
// The optional course flag corresponds to CPU0B:8AAC's extra eight pixels.
BackdropView backdrop_view(std::int16_t yaw, std::int16_t pitch,
                           bool elevated_course_flag = false);
std::uint8_t backdrop_index(const CourseBackdrop& backdrop, unsigned mode,
                            const BackdropView& view, int x, int y);

}
