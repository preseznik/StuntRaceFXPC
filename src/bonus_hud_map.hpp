#pragma once
#include "srf/bonus_hud_tiles.hpp"
#include "srf/championship.hpp"
#include "srf/vehicle.hpp"

namespace srf {
class CourseHudMap {
    std::array<std::uint8_t,16*32> route{};
    std::vector<std::uint8_t> markers;
    std::array<std::uint32_t,16> palette;

public:
    CourseHudMap(const Rom& rom,unsigned course,std::size_t palette_offset=0xf8960)
        :markers(extract_bonus_map_marker(rom)),palette(bonus_hud_palette(rom,palette_offset)) {
        // CPU0B:8356 selects a 4x4 tile rectangle in the 16-tile-wide route atlas.
        const auto sheet=extract_bonus_course_map_tiles(rom);
        const unsigned index=rom.u16(Rom::lorom_offset(0x0b8396)+2*course);
        const auto first_tile=(index/4)*64+(index%4)*4;
        for(unsigned row=0;row<4;++row) {
            const auto source=std::span(sheet).subspan(32*(first_tile+16*row),4*32);
            std::copy(source.begin(),source.end(),route.begin()+row*4*32);
        }
    }

    void draw_route(std::span<std::uint32_t> art,int left) const {
        for(int row=0;row<4;++row)
            for(int col=0;col<4;++col)
                draw_bonus_hud_tile(art,208,left+8*col,80+8*row,route,4*row+col,palette);

    }

    void draw_marker(std::span<std::uint32_t> art,const Vehicle& vehicle,unsigned marker) const {
        // CPU02:CC4C and CPU0B:82E7. Z is negated as a 16-bit word before
        // the signed shift. Subtract (24,32) from original screen coordinates.
        const int x=static_cast<std::int16_t>(vehicle.body.word(0x1e));
        const int neg_z=static_cast<std::int16_t>(-vehicle.body.word(0x22));
        draw_bonus_hud_tile(art,208,18+(x>>11),92+(neg_z>>11),markers,marker,palette);
    }
};

class BonusHudMap {
    struct Entry { unsigned slide, marker; };
    static constexpr std::array<Entry,3> entries{{{234,258},{229,257},{229,255}}};
    Entry entry;
    CourseHudMap map;
public:
    BonusHudMap(const Rom& rom,SpeedClass league)
        :entry(entries.at(static_cast<unsigned>(league))),map(rom,20+static_cast<unsigned>(league)) {}

    void draw(std::span<std::uint32_t> art,const Vehicle& truck,unsigned video_tick) const {
        if(video_tick<entry.slide)return;
        const int left=video_tick>=entry.slide+10 ? 5 : 4*int(video_tick-entry.slide)-33;
        map.draw_route(art,left);
        if(video_tick>=entry.marker)map.draw_marker(art,truck,1);
    }
};
}
