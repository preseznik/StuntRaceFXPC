#include "srf/audio.hpp"
#include "srf/backdrop.hpp"
#include "srf/environment.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void append(std::vector<std::uint8_t>& bytes, unsigned value, unsigned length) {
    for (unsigned i = 0; i < length; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void check_sound(const srf::Rom& rom) {
    const auto bank = srf::extract_course_audio(rom, srf::CourseId::sunset_valley);
    const auto package = srf::encode_audio(bank);
    const auto decoded = srf::decode_audio(package);
    require(srf::encode_audio(decoded) == package, "Sunset audio package round trip failed");
    require(srf::sha256(decoded.music) == "0993cfdf06ef4d51400260256f4338f5f2d67c695dae91cd04b16bada8ad9286",
            "Sunset music differs from the original SPC memory");
    require(srf::sha256(decoded.effect_noise_seed) ==
            "aa6d2d635619f9c68259b5d41d100d4d34b0c95243457c005faf9e50e4312b69",
            "Sunset noise seed differs from the original SPC memory");
    std::vector<std::uint8_t> samples;
    for (const auto& sample : decoded.samples) {
        append(samples, sample.directory_index, 1);
        append(samples, sample.original_start, 2);
        append(samples, static_cast<unsigned>(sample.brr.size()), 4);
        samples.insert(samples.end(), sample.brr.begin(), sample.brr.end());
    }
    require(decoded.samples.size() == 27 && srf::sha256(samples) ==
            "b3d8a399595b3c1c8647ac9c2f8e003e704c74dd7397d87b211844787977385d",
            "Sunset sample directory or BRR bytes differ from the original SPC memory");

    srf::MusicRenderer renderer(decoded);
    std::vector<std::int16_t> music(120 * 32000 * 2);
    renderer.render(music);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(music.data()), music.size()*2}) ==
            "45b8c764e62e893362ade68ab9a28ea699ace77dc9c2acdb82646f18ce70f094",
            "Sunset music loop rendering changed");
    // Original object 09:E097 plays effect 5A using sample 36 from the prior
    // effect upload. Direct course launch must have that sample before driving.
    renderer.reset();
    renderer.set_music_enabled(false);
    renderer.play_effect(0x5a);
    auto effect = std::span(music).first(32000);
    renderer.render(effect);
    require(std::any_of(effect.begin(), effect.end(), [](auto sample) { return sample != 0; }),
            "Sunset scenery effect is silent");
}

void check_backdrop(const srf::Rom& rom) {
    const auto backdrop = srf::extract_course_backdrop(rom, srf::CourseId::sunset_valley);
    constexpr std::array index_hash{
        "5ccd2fa76b3121ee3de236340a8b96abee14cf1a2d07012065f287d79ee54928",
        "95342ef36909ed440c6e39b5e67ef968edeefe47c25b8f2c824860ab3b4853af"};
    constexpr std::array gradient_hash{
        "bf7b30beb3896dfe3d2e5034b4fb4ad46624e6f61c677dc7df5a260cf8f816b8",
        "e2e47c58cd6343587c9f2221ab60fdd520596e22e181e0b935bc2356f222b38b"};
    for (unsigned mode = 0; mode < 2; ++mode) {
        require(srf::sha256(backdrop.indices[mode]) == index_hash[mode],
                "Sunset panorama differs from original tilemap/glyph reconstruction");
        std::vector<std::uint8_t> gradient;
        for (const auto color : backdrop.gradients[mode]) append(gradient, color, 2);
        require(srf::sha256(gradient) == gradient_hash[mode], "Sunset sky gradient differs from original WRAM");
    }

    // Each original-frame digest covers two unobstructed regions of the race
    // window. The reference held camera position and changed the original
    // game’s sunset trigger registers and map flag between captures.
    struct Capture { unsigned mode; std::uint8_t weight; const char* hash; };
    constexpr std::array captures{
        Capture{0, 0, "0129bfbd24952f55057730a979c4408fc885684fd8592b610a337ad848354ac6"},
        Capture{0, 31, "31955af91d9da4fac91048f2ab9b6b2c5a7633bd0a8521254feff574c32294d3"},
        Capture{1, 31, "8efa87829e2b25c3ad732b81a5730b159f51d2f47ee6971ab44083e68c30320f"}};
    struct Region { int x, y, width, height; };
    constexpr std::array regions{Region{0, 4, 28, 12}, Region{81, 4, 40, 10}};
    const auto view = srf::backdrop_view(-16384, -3232);
    for (const auto& capture : captures) {
        std::vector<std::uint8_t> pixels;
        for (const auto& region : regions) {
            for (int y = region.y; y < region.y + region.height; ++y) {
                for (int x = region.x; x < region.x + region.width; ++x) {
                    const auto index = srf::backdrop_index(backdrop, capture.mode, view, x, y);
                    const auto color = [&](unsigned mode) {
                        return index == 1 ? backdrop.gradients[mode][view.gradient_y + y] : backdrop.palettes[mode][index];
                    };
                    append(pixels, srf::blend_environment_rgb555(color(0), color(1), capture.weight), 2);
                }
            }
        }
        require(srf::sha256(pixels) == capture.hash, "Sunset clipped panorama differs from original displayed pixels");
    }
}
}

int wmain() {
    try {
        const auto path = _wgetenv(L"SRF_ROM");
        if (!path) return 77;
        const auto rom = srf::Rom::load(path);
        check_sound(rom);
        check_backdrop(rom);
        std::cout << "Sunset music, samples, scenery effect and three original panorama comparisons passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
