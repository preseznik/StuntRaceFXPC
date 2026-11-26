#pragma once
#include "srf/bonus_hud_tiles.hpp"
#include "srf/bonus_tally.hpp"
#include <array>

namespace srf {
class BonusTallyArt {
    using Inks=std::array<std::uint32_t,4>;
    const Rom& rom;
    std::array<std::uint32_t,16> palette;
    Inks white,gold,pink,cyan,lap;

    Inks inks(unsigned shade,unsigned fill) const {return {0,palette[15],palette[shade],palette[fill]};}

    // GSU 07:F15B/F280/F329 and FA35/FA5A share the 128-texel-wide 2bpp
    // atlas. The scalable sprites step their source coordinates in 8.8 units.
    void blit(std::span<std::uint32_t> art,unsigned glyph,int x,int y,int width,int height,
              const Inks& colors,int step_x=256,int step_y=256) const {
        const auto source=rom.bytes().subspan(0xa8000+(glyph&0x1fff));
        for(int row=0;row<height;++row) {
            const unsigned source_row=32*((row*step_y)>>8);
            for(int col=0;col<width;++col) {
                const unsigned u=(col*step_x)>>8;
                const unsigned ink=(source[source_row+u/4]>>(2*(u%4)))&3;
                if(ink)art[(y+row)*208+x+col]=colors[ink];
            }
        }
    }

    void digits(std::span<std::uint32_t> art,unsigned value,int x,int height,const Inks& colors) const {
        for(const unsigned digit:{value/10%10,value%10}) {
            blit(art,(digit/8)*0x400+(digit%8)*4,x,40-height,16,height,colors);
            x+=16;
        }
    }

    void rows(std::span<std::uint32_t> art,const BonusTallyFrame& f) const {
        // CPU07:DE93 initializes the three result lines. Each character has its
        // own destination; the originals overlap and arrive in drawing order.
        const unsigned phase=f.palette_phase&15;
        const unsigned cycle=std::min(phase,15-phase);
        const Inks flashing{0,palette[15],hud_rgba(rom.u16(0x11432+2*cycle)),palette[6]};
        for(unsigned i=0;i<(f.earned_cars?24u:15u);++i) {
            int x=std::min<int>(rom.u16(Rom::lorom_offset(0x07df72)+2*i),56+11*f.row_step);
            auto glyph=rom.u16(Rom::lorom_offset(0x07dfa2)+2*i);
            if(i==1 || i==2)glyph=0x1400+2*(i==1?f.flag_seconds/10%10:f.flag_seconds%10);
            if(f.earned_cars<2) {
                // NO LAPS and ONE LAP replace the table's TWO LAPS.
                if(i==8)glyph=f.earned_cars?0x9610:0x960e;
                if(i==9)glyph=f.earned_cars?0x960e:0x9610;
                if(i==10)glyph=f.earned_cars?0x941c:0;
                if(!f.earned_cars && i>=11)x-=8;
                if(f.earned_cars && i==14)glyph=0;
                if(f.earned_cars && i==23)glyph=0x980e;
            }
            const auto& colors=i>=15?flashing:(i==1 || i==2 || (i>=8 && i<=10))?gold:white;
            if(glyph)blit(art,glyph,x,i<7?52:i<15?76:96,8,16,colors);
        }
        // CPU07:A158/A27D place the two 24x24 icons over the entering letters.
        for(int row=0;row<6;++row)for(int col=0;col<3;++col) {
            const auto& colors=row>=3?lap:row>0?white:col==2?cyan:pink;
            blit(art,0x1a1a+0x100*row+2*col,56+8*col,48+8*row,8,8,colors);
        }
    }

    void banner(std::span<std::uint32_t> art,unsigned stage,bool right=false,bool closing=false) const {
        // CPU07:92B5/9364 and 9486/9533 place mirrored cap/arrow tiles
        // around the common stretched center (9660) and wordmark (9718).
        const auto word=[&](unsigned address){return rom.u16(Rom::lorom_offset(address)+2*stage);};
        const int width=word(0x079806),x=right && !closing?55:143-width,y=6;
        const int cap=right?x-8:x+width;
        blit(art,right?0x1a10:0x1a12,cap,y,8,8,pink);
        blit(art,right?0x1a18:0x1c18,cap,y+8,8,16,pink,256,128);
        blit(art,right?0x1b10:0x1b12,cap,y+24,8,8,pink);
        const int stride=word(0x0797ca);
        blit(art,0x1b18,x,y,width,8,pink,stride);
        blit(art,0x1a55,x,y+8,width,16,pink,stride,128);
        blit(art,0x1d18,x,y+24,width,8,pink,stride);
        const int arrow=right?x+width:x-8,tip=right?arrow+8:arrow-8;
        const unsigned head=right?0x1a0e:0x1a0c;
        blit(art,head,arrow,y,8,8,pink);
        blit(art,head+8,arrow,y+8,8,8,pink);
        blit(art,head+0x108,arrow,y+16,8,8,pink);
        blit(art,head+0x100,arrow,y+24,8,8,pink);
        blit(art,head,tip,y+8,8,8,pink);
        blit(art,head+0x100,tip,y+16,8,8,pink);
        if(stage) {
            const int step_x=word(0x0797fa);
            blit(art,0x1c00,x,y+8,(88*256+step_x-1)/step_x,16,gold,step_x);
        }
    }

public:
    explicit BonusTallyArt(const Rom& source,std::size_t palette_offset=0xf8960)
        :rom(source),palette(bonus_hud_palette(rom,palette_offset)),white(inks(3,14)),gold(inks(12,13)),
         pink(inks(7,6)),cyan(inks(10,9)),lap(inks(11,14)) {}

    void draw(std::span<std::uint32_t> art,const BonusTallyFrame& f) const {
        if(art.size()!=208*128)throw std::invalid_argument("Invalid Bonus Trax tally canvas");
        if(f.row_step<0)return;
        rows(art,f);
        // On the last slide update CPU07:E036 draws the newly latched bank
        // before the first banner update draws the remaining flags on top.
        if(f.banner_step==0)digits(art,f.bank_seconds,172,32,cyan);
        digits(art,f.remaining_seconds,std::min(172,4+11*f.row_step),32,cyan);
        if(f.banner_step>=0) {
            const unsigned stage=std::min(4,f.banner_step);
            digits(art,f.bank_seconds,4,rom.u16(Rom::lorom_offset(0x07918f)+2*stage),cyan);
            banner(art,stage);
        }
    }

    void draw_stunt_clock(std::span<std::uint32_t> art,unsigned clock,unsigned bank,bool show_bank) const {
        const auto clock_inks=clock<10?pink:clock<20?gold:inks(5,11);
        if(clock<10)blit(art,clock/8*0x400+4*(clock%8),188,8,16,32,clock_inks);
        else digits(art,clock,172,32,clock_inks);
        if(show_bank)digits(art,bank,4,32,cyan);
    }
    void draw_stunt_timeout(std::span<std::uint32_t> art) const {
        constexpr unsigned glyphs[]{0xc14,0x808,0x818,0x418,0xc18,0xc04};
        constexpr int x[]{24,48,72,96,136,160};
        for(unsigned i=0;i<6;++i)blit(art,glyphs[i],x[i],40,24,40,inks(5,11),170,204);
    }

    void draw_grid_clock(std::span<std::uint32_t> art,unsigned clock,unsigned reserve,
                         int stage,bool closing,bool transfer) const {
        const int height=stage<0?32:rom.u16(Rom::lorom_offset(0x07918f)+2*stage);
        const auto green=inks(5,11);
        const int clock_height=closing?32:height;
        if(clock>=100)blit(art,4,158,40-clock_height,16,clock_height,green);
        digits(art,clock,172,clock_height,green);
        if(transfer && stage>=0) {
            digits(art,reserve,4,height,pink);
            banner(art,stage,true,closing);
        }
    }

    void draw_goal(std::span<std::uint32_t> art,unsigned tick) const {
        constexpr unsigned glyphs[]{0x800,0xc00,0x408,0x814};
        for(int i=0;i<4;++i) {
            const int t=std::clamp(int(tick)-4*i,0,30);
            const int rise=(30-t)*(30-t)/18;
            blit(art,glyphs[i],64+20*i,32+rise-3*i,16,32,pink);
        }
    }
    void draw_finish_time(std::span<std::uint32_t> art,unsigned seconds,unsigned transferred,unsigned stage) const {
        const auto green=inks(5,11);
        digits(art,seconds-transferred,172,32,green);
        digits(art,transferred,4,32,green);
        banner(art,std::min(stage,4u));
    }

    void draw_grid_laps(std::span<std::uint32_t> art,unsigned phase) const {
        if(phase<25 || phase>=44)return;
        // CPU07:8388..8498 scrolls each glyph through the same 32-row
        // window. On exit its source advances five rows per scene update.
        const int offset=phase<30?5*(30-int(phase)):phase>38?5*(38-int(phase)):0;
        const auto letter=[&](unsigned glyph,int x) {
            blit(art,glyph+32*std::max(0,-offset),x,40+std::max(0,offset),
                 16,32-std::abs(offset),cyan);
        };
        letter(0x00c,16); // 3
        constexpr unsigned text[]{0x814,0x408,0xc04,0xc10,0,0xc14,0xc00,0,0x800,0xc00}; // LAPS TO GO
        for(unsigned i=0;i<std::size(text);++i)if(text[i])letter(text[i],36+16*int(i));
    }
};
}
