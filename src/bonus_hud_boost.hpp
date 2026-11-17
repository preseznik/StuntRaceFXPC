#pragma once
#include "srf/bonus_hud_tiles.hpp"
#include "srf/environment.hpp"
#include "srf/vehicle.hpp"

namespace srf {
class BonusHudBoost {
    std::vector<std::uint8_t> tiles_;
    std::array<std::uint32_t,16> palette_;
    std::array<std::uint32_t,13> pulse_;
    std::uint32_t idle_,next_color_;
    unsigned arrival_,tick_{},age_{},phase_=6;
public:
    BonusHudBoost(const Rom& rom,unsigned league,std::uint8_t fog_weight=0)
        :tiles_(extract_bonus_hud_tiles(rom)),palette_(bonus_hud_palette(rom,0xf89c0)),
         idle_(hud_rgba(rom.u16(Rom::lorom_offset(0x0291da)))),next_color_(idle_),
         arrival_(league==0?233u:228u) {
        for(unsigned i=0;i<pulse_.size();++i)
            pulse_[i]=hud_rgba(rom.u16(Rom::lorom_offset(0x029210)+2*i));
        palette_[3]=idle_;
        // The four gray pixels on the end caps share OBJ palette 6 with
        // course scenery. The blue fill is set separately by the NMI.
        palette_[11]=hud_rgba(blend_environment_rgb555(rom.u16(0xf89c0+22),0x7fff,fog_weight));
    }

    // CPU02:9146 updates the boost ink on each video interrupt. The next
    // interrupt uploads it to CGRAM, even when no vehicle update falls between.
    void advance(bool boost_held,bool driving) {
        ++tick_;
        palette_[3]=next_color_;
        unsigned period=1,step=1;
        if(driving && boost_held) {
            age_=std::min(age_+1,255u);
            step=age_>=180?2:1;
            period=age_<60?4:age_<120?2:1;
        } else if(age_) {
            const auto previous=age_;
            age_-=previous>=120?5:previous>=60?4:previous>=30?3:1;
            step=previous>=120?2:1;
            period=previous<30?4:previous<60?2:1;
        } else {
            next_color_=idle_;
            return;
        }
        if(tick_%period==0) {
            phase_=(phase_+step)%pulse_.size();
            next_color_=pulse_[phase_];
        }
    }

    void draw(std::span<std::uint32_t> art,unsigned tick,const VehicleState& truck) const {
        // OAM slots 75..86 enter together from the left. Their final positions
        // are relative to the playfield's (24,32) crop in the original image.
        const int remaining=static_cast<int>(arrival_-std::min(tick,arrival_));
        draw_at(art,-remaining*251/64,truck);
    }

    void draw_at(std::span<std::uint32_t> art,int x,const VehicleState& vehicle) const {
        if(x+83<=0)return;
        const auto tile=[&](unsigned id,int left,bool flip=false) {
            draw_bonus_hud_tile(art,208,x+left,115,tiles_,id,palette_,flip);
        };
        const int fill=std::min<int>(48,vehicle.word(0x6a)>>3);
        for(int i=0;i<6;++i)tile(i<std::max(1,fill/8)?0x1f:0x1e,7+8*i);
        // Slot 75 moves one tile across the last eight pixels to expose a
        // partial segment. The first segment is masked; later ones extend.
        tile(fill<8?0x1e:0x1f,7+std::max(0,fill/8-1)*8+fill%8);
        tile(0x1d,-1);
        tile(0x1d,55,true);
        for(int i=0;i<3;++i)tile(0x1a+i,59+8*i);
    }
};
}
