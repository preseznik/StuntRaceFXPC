#pragma once
#include "bonus_dashboard.hpp"
#include "bonus_hud_boost.hpp"
#include "bonus_hud_map.hpp"
#include "bonus_tally_art.hpp"
#include "race_hud_text.hpp"
#include "srf/grid_countdown.hpp"

namespace srf {
inline void draw_start_signal(std::span<std::uint32_t> out,int x,unsigned stage,
    std::span<const std::uint8_t> tiles,std::span<const std::uint32_t,16> palette) {
    const auto obj=[&](unsigned id,int left,int y) {
        for(unsigned row=0;row<2;++row)for(unsigned col=0;col<2;++col)
            draw_bonus_hud_tile(out,208,left+8*int(col),y+8*int(row),
                                tiles,id+16*row+col,palette);
    };
    for(unsigned col=0;col<4;++col) {
        const unsigned id=col==0?4:col==3?6:5;
        obj(id,x+16*int(col),0);
        obj(id+4,x+16*int(col),16);
    }
    for(unsigned col=0;col<std::min(3u,stage);++col) {
        // The OAM writer carries one into the middle X when it clips
        // the first lamp off the physical screen, left of the 24px crop.
        const int carry=col==1 && x+10 < -24;
        obj(stage==4?2:0,x+10+14*int(col)+carry,8);
    }
}

inline void draw_race_stopwatch(std::span<std::uint32_t> out,int x,unsigned time,
    std::span<const std::uint8_t> tiles,std::span<const std::uint32_t,16> palette) {
    // OAM slots 116..127. Minutes use one digit, elapsed
    // seconds and hundredths two each. Punctuation uses a single tile.
    const auto tile=[&](unsigned id,int at,int y) {
        draw_bonus_hud_tile(out,208,x+at,y,tiles,id,palette);
    };
    const auto digit=[&](unsigned n,int at) {
        tile(2*n,at,108);
        tile(2*n+1,at,116);
    };
    digit(time/6000%10,0);
    tile(0x14,8,108);
    digit(time/1000%6,14);digit(time/100%10,22);
    tile(0x15,30,108);
    digit(time/10%10,38);digit(time%10,46);
}


class GridCountdownArt {
    BonusTallyArt lettering_;
    CourseHudMap map_;
    BonusHudBoost boost_;
    BonusDashboard dash_;
    std::vector<std::uint8_t> tiles_,signal_;
    RaceHudText notices_;
    std::array<std::uint32_t,16> palette_;

    GridCountdownArt(const Rom& rom,CourseId course,PlayerCar car,std::size_t palette_offset)
        :lettering_(rom,palette_offset),map_(rom,static_cast<unsigned>(course),palette_offset),
         boost_(rom,static_cast<unsigned>(course)/4,course==CourseId::kings_forest?15:course==CourseId::white_land?10:0),
         dash_(rom,car),tiles_(extract_bonus_hud_tiles(rom)),
         signal_(extract_start_light_tiles(rom)),notices_(rom),palette_(bonus_hud_palette(rom,palette_offset)) {}

public:
    // Both the Super FX lettering and OBJ palette 3 use the Harbor City bank.
    GridCountdownArt(const Rom& rom,CourseId course,PlayerCar car)
        :GridCountdownArt(rom,course,car,course==CourseId::harbor_city?0xf8760:0xf8960) {}

    void draw(std::span<std::uint32_t> out,const GridCountdownFrame& f,const NativeRace& race,bool instruments=true) const {
        if(out.size()!=208*128)throw std::invalid_argument("Invalid race grid overlay size");
        std::fill(out.begin(),out.end(),0);
        if(!instruments) {
            if(f.signal_visible)draw_start_signal(out,f.signal_x,f.light_stage,signal_,palette_);
            return;
        }
        if(f.clock_visible) {
            const bool transfer=f.initial_carry!=0;
            const int stage=transfer?f.banner_step:std::clamp(int(f.display_phase)-25,0,4);
            // The grid still presents its completed GSU buffer after green,
            // including the first decrement before the signal leaves.
            lettering_.draw_grid_clock(out,f.clock_seconds,f.remaining_carry,stage,f.closing,transfer);
            if(!transfer)lettering_.draw_grid_laps(out,f.display_phase);
        }
        if(f.map_x>-32)map_.draw_route(out,f.map_x);
        if(f.marker_visible) {
            // CPU03:DDA4/DDD5 use the entrant's color in the small (AI)
            // or large (player) marker bank. The player's OAM index wins.
            for(unsigned i=0;i<race.racers().size();++i) {
                const auto& car=race.racers()[i];
                map_.draw_marker(out,car,car.body.word(0x96)+(i==race.player_index()?0:5));
            }
        }
        boost_.draw_at(out,f.boost_x,race.racers()[race.player_index()].body);
        if(f.elapsed_x<208)draw_race_stopwatch(out,f.elapsed_x,race.elapsed_centiseconds(),tiles_,palette_);
        if(f.signal_visible)draw_start_signal(out,f.signal_x,f.light_stage,signal_,palette_);
    }

    void driving(std::span<std::uint32_t> pixels,const NativeRace& race,bool paused,bool show) const {
        GridCountdownFrame f{};
        f.clock_visible=f.marker_visible=true;
        f.display_phase=48;f.clock_seconds=race.remaining_seconds();
        f.map_x=5;f.elapsed_x=148;f.boost_x=0;
        f.signal_visible=race.countdown_active();
        f.signal_x=72;f.light_stage=race.countdown_stage();
        draw(pixels,f,race,show);
        std::string message;
        if(paused)message="PAUSED";
        else if(race.completion().finished)message="FINISH  R RESTART";
        else if(race.failure()==RaceFailure::wrecked)message="WRECKED  R RESTART";
        else if(race.failure()==RaceFailure::out_of_bounds)message="OUT OF COURSE  R RESTART";
        else if(race.failure()==RaceFailure::time_out || race.time_over_pending())message="TIME OVER";
        else if(const auto award=race.time_award();show && award)
            message=std::string(award->checkpoint?"CHECKPOINT":"LAP EXTEND")+"  "+std::to_string(award->seconds)+" SEC";
        notices_.draw(pixels,message,44);
    }

    std::span<const std::uint32_t> dashboard(const GridCountdownFrame& f,const NativeRace& race) {
        const auto i=race.player_index();
        const auto& vehicle=race.racers()[i].body;
        const auto rank=race.standings()[i];
        return dash_.draw(static_cast<std::int16_t>(vehicle.word(0x4a)),vehicle.word(0x82),
                          f.spares,rank?rank:1);
    }
};
}
