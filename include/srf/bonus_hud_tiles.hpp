#pragma once
#include "srf/rom.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace srf {
std::vector<std::uint8_t> extract_bonus_hud_tiles(const Rom& rom);
std::vector<std::uint8_t> extract_bonus_dashboard_tiles(const Rom& rom,unsigned icon);
std::vector<std::uint8_t> extract_bonus_course_map_tiles(const Rom& rom);
std::vector<std::uint8_t> extract_bonus_map_marker(const Rom& rom);

inline std::uint32_t hud_rgba(std::uint16_t rgb555) {
    const auto component=[](unsigned c){return (c<<3)|(c>>2);};
    return 0xff000000U|component(rgb555&31)|(component((rgb555>>5)&31)<<8)|(component((rgb555>>10)&31)<<16);
}

inline std::array<std::uint32_t,16> bonus_hud_palette(const Rom& rom,std::size_t offset) {
    std::array<std::uint32_t,16> result{};
    for(unsigned i=0;i<result.size();++i)result[i]=hud_rgba(rom.u16(offset+2*i));
    return result;
}

inline void draw_bonus_hud_tile(std::span<std::uint32_t> canvas,unsigned width,int x,int y,
                                std::span<const std::uint8_t> tiles,unsigned id,
                                std::span<const std::uint32_t,16> palette,bool flip_x=false,bool flip_y=false) {
    if(!width || canvas.size()%width || id>=tiles.size()/32)throw std::invalid_argument("Invalid HUD tile or canvas");
    const auto tile=tiles.subspan(32*id,32);
    const int height=static_cast<int>(canvas.size()/width);
    for(int row=std::max(0,-y);row<std::min(8,height-y);++row) {
        const int at=2*(flip_y?7-row:row);
        for(int col=std::max(0,-x);col<std::min(8,int(width)-x);++col) {
            const int bit=flip_x?col:7-col;
            const unsigned ink=((tile[at]>>bit)&1) | (((tile[at+1]>>bit)&1)<<1) |
                               (((tile[at+16]>>bit)&1)<<2) | (((tile[at+17]>>bit)&1)<<3);
            if(ink)canvas[(row+y)*width+col+x]=palette[ink];
        }
    }
}
}
