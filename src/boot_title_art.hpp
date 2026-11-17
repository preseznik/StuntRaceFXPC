#pragma once
#include "srf/backdrop.hpp"
#include "srf/bonus_hud_tiles.hpp"
#include "srf/boot_title.hpp"

namespace srf {
class BootTitleArt {
    static constexpr int width=256,height=224;
    std::array<std::uint16_t,256> palette_{};
    std::vector<std::uint8_t> checker_,cars_,matte_,logo_,copyright_,prompt_,splash_;

    static unsigned ink(std::span<const std::uint8_t> tiles,unsigned tile,unsigned bpp,unsigned x,unsigned y) {
        const unsigned at=tile*8*bpp+2*y,bit=7-x;
        unsigned value=0;
        for(unsigned p=0;p<bpp;++p)value|=((tiles[at+16*(p/2)+p%2]>>bit)&1)<<p;
        return value;
    }
    static std::vector<std::uint8_t> background(std::span<const std::uint8_t> tiles,
                                                 std::span<const std::uint8_t> map,unsigned bpp) {
        std::vector<std::uint8_t> indices(width*256);
        if(map.size()!=2048)throw std::runtime_error("Invalid title tilemap");
        for(unsigned ty=0;ty<32;++ty)for(unsigned tx=0;tx<32;++tx){
            const unsigned i=2*(ty*32+tx),entry=map[i]|(unsigned(map[i+1])<<8);
            const unsigned tile=entry&0x3ff,bank=(entry>>10)&7;
            if((tile+1)*8*bpp>tiles.size())throw std::runtime_error("Invalid title tile");
            for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
                const auto pen=ink(tiles,tile,bpp,entry&0x4000?7-x:x,entry&0x8000?7-y:y);
                if(pen)indices[(8*ty+y)*width+8*tx+x]=static_cast<std::uint8_t>((bank<<bpp)+pen);
            }
        }
        return indices;
    }
    static void sprite(std::vector<std::uint8_t>& layer,std::span<const std::uint8_t> tiles,
                       unsigned tile,unsigned pal,int left,int top) {
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
            const unsigned pen=ink(tiles,tile,4,x,y);
            if(pen)layer[(top+y)*width+left+x]=static_cast<std::uint8_t>(128+16*pal+pen);
        }
    }
    static unsigned scale(unsigned color,unsigned weight) {
        const auto component=[&](int shift){return ((color>>shift)&31)*weight/32;};
        return component(0)|(component(5)<<5)|(component(10)<<10);
    }
    static unsigned add(unsigned a,unsigned b) {
        const auto component=[&](int shift){return std::min(31u,((a>>shift)&31)+((b>>shift)&31));};
        return component(0)|(component(5)<<5)|(component(10)<<10);
    }
    static std::uint32_t rgba(unsigned color,unsigned brightness) {
        if(brightness==15)return hud_rgba(static_cast<std::uint16_t>(color));
        const auto component=[&](int shift){return ((((color>>shift)&31)*brightness+7)/15);};
        return hud_rgba(static_cast<std::uint16_t>(component(0)|(component(5)<<5)|(component(10)<<10)));
    }
public:
    explicit BootTitleArt(const Rom& rom):copyright_(width*height),prompt_(width*height),splash_(width*height) {
        for(unsigned i=0;i<256;++i)palette_[i]=rom.u16(0xe189c+2*i);
        const auto tiles=unpack_rom_graphics(rom,0x1cdc38);
        cars_=background(unpack_rom_graphics(rom,0x1cbabc),unpack_rom_graphics(rom,0x1cbe58),4);
        matte_=background(unpack_rom_graphics(rom,0x1cc664),unpack_rom_graphics(rom,0x1cc818),4);
        logo_=background(tiles,unpack_rom_graphics(rom,0x1cc9dc),4);
        checker_=background(rom.slice(0xe3e58,64),unpack_rom_graphics(rom,0x1cbecc),2);
        const auto nintendo=rom.slice(0xe5c8c,512);
        for(unsigned j=0;j<16;++j)sprite(splash_,nintendo,j,5,96+8*(j%8),104+8*(j/8));
        for(unsigned j=0;j<24;++j)sprite(copyright_,tiles,194+j,5,80+8*(j%12),198+8*(j/12));
        constexpr unsigned upper[]{182,183,184,185,184,186,187,188,186};
        constexpr unsigned lower[]{189,190,191,192,191,193,192,192,193};
        constexpr unsigned x[]{85,94,103,112,127,136,145,154,163};
        for(unsigned i=0;i<9;++i){
            sprite(prompt_,tiles,upper[i],3,x[i],128);
            sprite(prompt_,tiles,lower[i],3,x[i],136);
        }
    }
    void draw(std::span<std::uint32_t> out,const BootTitleFrame& f) const {
        if(out.size()!=width*height)throw std::invalid_argument("Boot title requires 256 by 224 pixels");
        if(!f.checker){
            std::fill(out.begin(),out.end(),0xff000000u);
            if(f.nintendo)for(unsigned y=104;y<120;++y)for(unsigned x=96;x<160;++x){
                const unsigned i=y*width+x;
                if(splash_[i])out[i]=hud_rgba(palette_[splash_[i]]);
            }
            return;
        }
        // BG1 adds the car inks to a checkerboard matte on BG2 while the
        // artwork fades in. The independent BG3 checker remains visible outside it.
        std::array<unsigned,256> car_inks{};
        for(unsigned i=64;i<128;++i)car_inks[i]=scale(palette_[i],f.car_weight);
        std::array<unsigned,16> checker_inks{};
        for(unsigned i=12;i<16;++i)checker_inks[i]=scale(palette_[i],std::min(32u,33-f.car_weight));
        for(int y=0;y<height;++y){
            const int logo_y=y+1+f.logo_scroll;
            for(int x=0;x<width;++x){
                const int i=y*width+x,bg=i+width;
                const auto base=checker_[bg];
                unsigned color=palette_[base?base:12];
                if(cars_[bg]){
                    const auto mask=matte_[bg];
                    color=add(car_inks[cars_[bg]],checker_inks[mask?mask:12]);
                }
                if(f.logo && y>=f.logo_top && logo_y>=0 && logo_y<256 && logo_[logo_y*width+x])
                    color=palette_[logo_[logo_y*width+x]];
                if(f.copyright && copyright_[i])color=palette_[copyright_[i]];
                if(f.prompt && prompt_[i])color=palette_[prompt_[i]];
                out[i]=rgba(color,f.brightness);
            }
        }
    }
};
}
