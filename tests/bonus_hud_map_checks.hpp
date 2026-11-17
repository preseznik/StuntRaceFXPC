#pragma once
#include "../src/bonus_hud_map.hpp"

inline void check_bonus_hud_maps(const srf::Rom& rom) {
    // Lower-left map pixels from the original 208x128 view before the marker appears.
    constexpr std::array<std::uint32_t,3> sliding{0x34f9bdc5,0xa8f07e4b,0xf2cb5976};
    constexpr std::array<std::uint32_t,3> settled{0xff64370c,0xa89cfc70,0xb6592ef6};
    constexpr std::array<unsigned,3> first_marker{258,257,255};
    const auto cyan=srf::hud_rgba(0x7ee0);
    for(unsigned cls=0;cls<3;++cls) {
        const srf::BonusHudMap map(rom,static_cast<srf::SpeedClass>(cls));
        srf::Vehicle truck{};
        std::vector<std::uint32_t> art(208*128);
        const auto draw=[&](unsigned tick) {
            std::fill(art.begin(),art.end(),0);
            map.draw(art,truck,tick);
        };
        const auto fingerprint=[&](unsigned tick) {
            draw(tick);
            std::uint32_t hash=2166136261u;
            for(const auto pixel:art)hash=(hash^pixel)*16777619u;
            return hash;
        };
        if(fingerprint(235)!=sliding[cls] || fingerprint(246)!=settled[cls] ||
           fingerprint(first_marker[cls]-1)!=settled[cls])
            throw std::runtime_error("Original Bonus Trax course map or slide differs");

        // Original marker has 12 cyan texels. Check both axes at signed cell
        // boundaries, including the original 16-bit negation of Z=-32768.
        struct Pose { int x,z,cyan_x,cyan_y; };
        for(const auto pose:{Pose{-1,1,19,94},Pose{2048,-2048,21,96},Pose{-32768,-32768,4,79}}) {
            truck.body.word(0x1e)=static_cast<std::uint16_t>(pose.x);
            truck.body.word(0x22)=static_cast<std::uint16_t>(pose.z);
            draw(first_marker[cls]);
            if(std::count(art.begin(),art.end(),cyan)!=12 || art[208*pose.cyan_y+pose.cyan_x]!=cyan)
                throw std::runtime_error("Original Bonus Trax truck marker differs");
        }
    }
}
