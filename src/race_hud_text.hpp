#pragma once
#include "srf/backdrop.hpp"
#include "srf/bonus_hud_tiles.hpp"

namespace srf {
// The results screen and race notices use the same cartridge alphabet.
class RaceHudText {
    std::vector<std::uint8_t> letters_;
    std::array<std::uint32_t,16> palette_,shadow_;
public:
    explicit RaceHudText(const Rom& rom):letters_(unpack_rom_graphics(rom,0x18c770)),
        palette_(bonus_hud_palette(rom,0xf8320)) {shadow_.fill(0xff242428);}
    void draw(std::span<std::uint32_t> pixels,std::string_view message,int y) const {
        int x=(208-8*int(message.size()))/2;
        for(char c:message){
            if(c!=' '){
                const unsigned glyph=c=='/'?43:c<='9'?c-'0':10+c-'A';
                draw_bonus_hud_tile(pixels,208,x+1,y+1,letters_,glyph,shadow_);
                draw_bonus_hud_tile(pixels,208,x,y,letters_,glyph,palette_);
            }
            x+=8;
        }
    }
};
}
