#pragma once
#include "bonus_dashboard.hpp"

namespace srf {
// The attract screen leaves the map, clock and lights out of the playfield.
// The original window is symmetric around (127.5,95.5) and clips its corners.
class DemoDashboard {
    BonusDashboard instruments_;
    std::array<std::uint32_t,16> palette_;
    std::vector<std::uint32_t> surround_,pixels_;

    static int surround_ink(int x,int y) {
        if(y<17 || y>=208)return -1;
        if(y<24 || y>167)return 2;
        const int row=std::min(y-24,167-y),edge=std::min(x,255-x);
        if(row==0)return edge<11?2:1;
        if(row==1)return edge<10?2:edge<12?1:15;
        if(edge<9)return 2;
        if(row==2)return edge<11?1:edge==11?15:1;
        if(edge==10)return 15;
        if(row<7)return 1;
        constexpr int inner[]{28,26,25,24,24,23};
        constexpr int scene[]{128,28,26,25,25,24};
        const auto i=std::min(row-7,5);
        if(edge>=scene[i])return 0;
        return edge>=inner[i]?15:1;
    }
    static std::uint32_t dim(std::uint32_t col,unsigned light) {
        const auto channel=[&](unsigned i){return ((((col>>i)&255)*light+7)/15)<<i;};
        return 0xff000000u|channel(0)|channel(8)|channel(16);
    }
public:
    explicit DemoDashboard(const Rom& rom):instruments_(rom,PlayerCar::coupe),
        palette_(bonus_hud_palette(rom,0xf8960)),surround_(256*224),pixels_(256*224) {
        for(int y=0;y<224;++y)for(int x=0;x<256;++x) {
            const int ink=surround_ink(x,y);
            surround_[256*y+x]=ink<0?0xff000000u:ink==0?0:palette_[ink];
        }
    }
    std::span<const std::uint32_t> draw(unsigned tick,unsigned brightness,std::int16_t speed) {
        // CGRAM entry E9 falls from RGB(30,30,30) to black in ten steps.
        constexpr unsigned change[]{14,22,27,35,40,48,53,61,66,74};
        const unsigned faded=unsigned(std::upper_bound(std::begin(change),std::end(change),tick)-std::begin(change));
        const unsigned alpha=255-(255*brightness*faded+75)/150;
        const unsigned white=(247*brightness*(10-faded)+75)/150;
        const std::uint32_t aperture=alpha<<24 | white*0x010101u;
        for(std::size_t i=0;i<pixels_.size();++i) {
            const auto ink=surround_[i];
            pixels_[i]=!ink?aperture:brightness==15?ink:dim(ink,brightness);
        }
        const auto dash=instruments_.draw(speed,0,0,0);
        auto out=pixels_.begin()+168*256;
        if(brightness==15)std::copy(dash.begin(),dash.end(),out);
        else std::transform(dash.begin(),dash.end(),out,[&](auto p){return dim(p,brightness);});
        return pixels_;
    }
};
}
