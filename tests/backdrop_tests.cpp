#include "srf/backdrop.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void append(std::vector<std::uint8_t>& bytes, std::uint16_t color) {
    bytes.push_back(static_cast<std::uint8_t>(color));
    bytes.push_back(static_cast<std::uint8_t>(color >> 8));
}
template<std::size_t N>
std::string color_hash(const std::array<std::uint16_t, N>& colors) {
    std::vector<std::uint8_t> bytes;
    for (const auto color : colors) append(bytes, color);
    return srf::sha256(bytes);
}
}

int wmain() {
    try {
        const auto path = _wgetenv(L"SRF_ROM");
        if (!path) return 77;
        const auto rom = srf::Rom::load(path);
        const auto backdrop = srf::extract_course_backdrop(rom, srf::CourseId::aqua_tunnel);
        require(backdrop.available(), "Missing original Aqua backdrop");
        require(!srf::extract_course_backdrop(rom, srf::CourseId::easy_ride).available(),
                "Unvalidated course backdrop enabled");

        // Independent ROM decode matched every original expanded WRAM map byte
        // (20,480 total), VRAM glyph byte (6,144), and gradient byte (2,048).
        // Provenance and raw local-only evidence: build/aqua-backdrop-assets.
        constexpr std::array index_hashes{
            "b75783562473d6defd4a0bf04bc276e1e292e478f31a490146041d095cbed0b4",
            "0fab8ff864ecfdf82e9555effd56b1fafe94151e7fefa2ed0724b100700374d8"};
        constexpr std::array gradient_hashes{
            "30f7d722502634b8c6a8ffca4ba78ba374527f780c59f17f7a0a648061cffc0f",
            "58644efa794f99d85708d3546f8851b8b477944e168c60c601703a21c36bf044"};
        constexpr std::array palette_hashes{
            "74bb346cb1472fdd6ff6a985f390be55c48a1c387ecdf86afe051e1663e0f9b1",
            "9f440381480fc612a0be4af9f58dc52cb87d336c8cb64a65b35b12b907bd3687"};
        for (unsigned mode = 0; mode < 2; ++mode) {
            require(backdrop.indices[mode].size() == 1024 * 256, "Wrong panorama dimensions");
            require(srf::sha256(backdrop.indices[mode]) == index_hashes[mode],
                    "Panorama differs from original glyph/map reconstruction");
            require(color_hash(backdrop.gradients[mode]) == gradient_hashes[mode],
                    "Scanline ramp differs from original WRAM");
            require(color_hash(backdrop.palettes[mode]) == palette_hashes[mode],
                    "Backdrop palette differs from original ROM");
            require(std::all_of(backdrop.indices[mode].begin(), backdrop.indices[mode].end(),
                               [](auto index) { return index > 0 && index < 16; }),
                    "Unexpected transparent or out-of-range backdrop pixels");
        }

        // Original screenshot after twenty neutral input frames from the Aqua
        // reference state: unobstructed SNES x24..187,y36..49. Native sampling
        // must reproduce all 2,296 RGB555 pixels, including cloud/island edges.
        const auto view = srf::backdrop_view(0x4000, -3184);
        require(view.x == 256 && view.y == 203 && view.gradient_y == 235,
                "Original camera projection differs");
        std::vector<std::uint8_t> reference_band;
        for (int y = 4; y < 18; ++y) {
            for (int x = 0; x < 164; ++x) {
                const auto index = srf::backdrop_index(backdrop, 0, view, x, y);
                append(reference_band, index == 1 ? backdrop.gradients[0][view.gradient_y + y]
                                                  : backdrop.palettes[0][index]);
            }
        }
        require(srf::sha256(reference_band) ==
                "10ec87093c3ccb0022c380139106c3dd6e0d81bd875600f24e8924e97e95026f",
                "Backdrop sampling differs from original rendered pixels");
        const auto wrapped = srf::backdrop_view(-1, 0);
        require(wrapped.x == 1023 && wrapped.y == 153 && wrapped.gradient_y == 185,
                "Signed camera angle wrap differs");
        require(srf::backdrop_index(backdrop, 0, wrapped, 1, 0) ==
                srf::backdrop_index(backdrop, 0, wrapped, -1023, 0), "Horizontal wrap seam");
        require(srf::backdrop_index(backdrop, 0, view, 0, 1000) == 1,
                "Out-of-range rows must use original solid gradient tile");
        require(srf::backdrop_view(0, -32768).gradient_y == 384 &&
                srf::backdrop_view(0, 32767).y == 97 &&
                srf::backdrop_view(0, 0, true).y == 161, "Camera limits/course offset differ");
        std::cout << "Aqua backdrop: original assets, gradients, camera wrap and 2296 rendered pixels matched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
