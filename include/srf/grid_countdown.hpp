#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace srf {

enum class GridCountdownRound { third, fourth, first };

namespace detail {
constexpr std::array<std::uint16_t,29> grid_display_ticks(std::uint16_t first,std::string_view dwell) {
    std::array<std::uint16_t,29> ticks{first};
    for(std::size_t i=1;i<ticks.size();++i)
        ticks[i]=static_cast<std::uint16_t>(ticks[i-1]+dwell[i-1]-'0');
    return ticks;
}
}

struct GridCountdownFrame {
    unsigned tick{}, scene_phase{}, display_phase{}, grant_seconds{}, initial_carry{}, remaining_carry{},
        clock_seconds{}, spares{}, brightness{}, light_stage{};
    int banner_step=-1, elapsed_x=212, boost_x=-83, map_x=-32, signal_x=-72, effect=-1;
    bool closing{}, clock_visible{}, marker_visible{}, signal_visible{}, driving{}, start_music{};
};

// Grid tick 0 is the first nonblack raster after the course map or retry.
// Camera and lettering advance on completed scene updates. OBJ strips slide in
// four pixels per VBlank. Input is locked by the original 07:E506 green edge.
class GridCountdown {
    struct Timing {
        std::array<std::uint16_t,29> present;      // First visible raster for each HUD phase 25..53.
        unsigned reference_green; // Origin of the music, clock and signal exit timings below.
        unsigned transfer_audio, flush_audio, grid_music, first_clock_decrement;
        unsigned elapsed_settled, boost_settled, map_settled, marker, signal_settled, signal_out;
    };
    // Original NTSC callback cadence by course, selected car and zero-bank drawing branch.
    // It changes slightly with render workload even for repeat visits to the same grid.
    static constexpr std::array<Timing,18> timings{{
        // Novice, COUPE, carried bank.
        {
            {104,109,114,119,125,131,137,143,149,155,160,166,172,178,183,189,195,200,206,212,217,223,229,234,240,245,250,255,259},
            541, 127,230,598,608,
            155,178,189,196,287,615
        },
        // Novice, COUPE, empty bank.
        {
            {104,108,112,117,121,126,131,136,141,146,150,155,160,165,169,174,179,183,187,192,196,200,205,209,213,218,222,226,230},
            526, 123,206,585,593,
            149,172,183,188,258,600
        },
        // Novice, 4WD, carried bank.
        {
            {104,109,114,120,126,132,137,143,149,155,161,167,173,179,185,191,197,202,208,214,220,226,232,238,244,250,255,260,265},
            558, 127,234,614,624,
            156,179,190,198,294,631
        },
        // Novice, 4WD, empty bank.
        {
            {104,108,113,117,122,127,132,137,141,146,151,156,161,166,171,175,180,185,190,194,199,203,208,213,217,222,227,231,236},
            526, 123,209,582,591,
            150,173,184,191,265,599
        },
        // Novice, F-TYPE, carried bank.
        {
            {104,108,114,119,125,131,137,143,149,154,160,166,172,178,183,189,195,201,206,212,218,224,229,235,241,246,252,256,261},
            554, 127,231,612,621,
            155,178,189,196,289,629
        },
        // Novice, F-TYPE, empty bank.
        {
            {103,108,112,117,121,126,131,136,141,146,150,155,160,165,169,174,179,183,188,192,197,201,205,210,214,219,223,227,232},
            535, 123,207,592,602,
            149,172,183,189,260,609
        },
        // Expert, COUPE, carried bank.
        {
            {96,101,107,112,118,124,131,137,143,148,154,160,166,172,178,184,190,196,202,208,215,221,227,234,240,246,252,257,262},
            557, 120,229,615,625,
            149,172,183,191,291,634
        },
        // Expert, COUPE, empty bank.
        {
            {96,101,105,110,115,120,125,130,135,140,145,149,154,159,164,169,174,179,184,188,193,198,203,209,214,219,224,228,233},
            535, 116,205,589,603,
            143,166,177,185,263,606
        },
        // Expert, 4WD, carried bank.
        {
            {96,101,107,112,118,124,131,137,143,148,154,160,167,173,179,185,192,198,204,210,217,224,230,237,244,250,256,261,267},
            563, 120,232,619,630,
            149,172,183,193,298,638
        },
        // Expert, 4WD, empty bank.
        {
            {96,101,105,110,115,120,125,130,135,140,145,150,155,160,165,170,176,181,186,191,196,201,207,212,217,223,228,233,239},
            532, 116,209,589,599,
            143,166,177,182,269,607
        },
        // Expert, F-TYPE, carried bank.
        {
            {96,101,107,112,118,124,131,137,143,148,154,160,166,172,178,184,191,197,203,209,215,222,228,235,241,248,253,258,264},
            566, 120,230,621,630,
            149,172,183,192,293,638
        },
        // Expert, F-TYPE, empty bank.
        {
            {96,101,105,110,115,120,125,130,135,140,145,149,155,160,165,170,175,179,184,189,194,199,204,210,215,220,225,229,235},
            525, 116,206,580,589,
            143,166,177,185,264,597
        },
        // Master, COUPE, carried bank.
        {
            {115,120,125,131,137,143,149,155,161,167,173,179,185,192,198,203,210,216,222,228,234,240,246,253,259,265,270,275,280},
            587, 138,248,642,652,
            167,190,201,211,310,660
        },
        // Master, COUPE, empty bank.
        {
            {115,119,124,128,133,138,143,148,153,159,164,169,174,179,184,189,194,198,203,208,213,218,223,228,232,237,242,247,252},
            546, 135,224,600,610,
            162,185,196,204,281,619
        },
        // Master, 4WD, carried bank.
        {
            {116,121,126,131,137,143,150,156,162,168,174,180,186,192,199,205,211,218,224,230,237,244,250,257,263,269,275,281,286},
            582, 139,252,638,650,
            168,191,202,212,317,658
        },
        // Master, 4WD, empty bank.
        {
            {116,120,124,129,134,139,144,149,154,159,164,169,174,180,185,190,195,200,205,210,216,221,226,231,236,242,247,252,257},
            551, 135,228,608,618,
            162,185,196,201,288,626
        },
        // Master, F-TYPE, carried bank.
        {
            {115,120,125,131,137,143,149,155,161,167,174,180,186,192,198,204,210,216,222,228,235,241,248,254,260,266,272,277,282},
            577, 138,249,633,643,
            168,191,202,211,312,651
        },
        // Master, F-TYPE, empty bank.
        {
            {115,119,124,128,133,138,143,148,154,159,164,169,174,179,184,189,194,199,204,208,213,218,223,228,233,238,243,248,253},
            546, 135,225,600,611,
            162,185,196,205,283,619
        },
    }};

    struct GridSignal {
        std::string_view scene_dwell;
        std::array<std::uint16_t,4> audio, visible;
    };
    static constexpr GridSignal signals_[18][24]{
#include "srf/grid_order_timing.inc"
    };
    struct MeasuredGrid {
        GridSignal signal;
        Timing timing;
        unsigned release_delay;
    };
    static constexpr MeasuredGrid first_[9]{
#include "srf/first_grid_timing.inc"
    };
    static constexpr MeasuredGrid fourth_[18][24]{
#include "srf/fourth_grid_timing.inc"
    };
    static unsigned grid_index(const std::array<std::uint16_t,4>& places) {
        std::array<std::uint16_t,4> order{4,3,2,1};
        unsigned index=0;
        while(order!=places) {
            if(!std::next_permutation(order.rbegin(),order.rend()))throw std::invalid_argument("Invalid starting grid");
            ++index;
        }
        return index;
    }
    static unsigned profile(unsigned league,unsigned car,unsigned carry) {
        return (std::min(league,2u)*3+std::min(car,2u))*2+unsigned(carry==0);
    }
    Timing timing_;
    GridSignal signal_;
    unsigned release_delay_;
    bool first_race_{};
    GridCountdownFrame frame_;
    unsigned next_camera_{},unit_{};
    unsigned after_green(unsigned reference_tick) const {return drive_tick()+(reference_tick-timing_.reference_green);}

    static int slide_left(unsigned t,unsigned settled,int target,int hidden) {
        if(t>=settled)return target;
        return std::min(hidden,target+4*static_cast<int>(settled-t));
    }
    static int slide_right(unsigned t,unsigned settled,int target,int hidden) {
        if(t>=settled)return target;
        return std::max(hidden,target-4*static_cast<int>(settled-t));
    }

    void update() {
        auto& f=frame_;
        const unsigned t=f.tick;
        while(f.scene_phase<signal_.scene_dwell.size() && t>=next_camera_){
            ++f.scene_phase;
            if(f.scene_phase<signal_.scene_dwell.size())next_camera_+=signal_.scene_dwell[f.scene_phase]-'0';
        }
        const auto next=std::upper_bound(timing_.present.begin(),timing_.present.end(),t);
        f.clock_visible=next!=timing_.present.begin();
        f.display_phase=f.clock_visible?24u+unsigned(next-timing_.present.begin()):std::min(f.scene_phase,24u);
        if(next==timing_.present.end())f.display_phase=std::max(53u,f.scene_phase);
        const unsigned p=f.display_phase;
        f.closing=f.initial_carry && p>=49 && p<=52;
        f.banner_step=-1;
        if(f.initial_carry && f.clock_visible && p<=52)
            f.banner_step=f.closing?int(52-p):std::min(4,int(p)-25);

        // 07:8ED3 picks a BCD transfer amount once. 07:8F58 applies it on
        // each callback; 07:8F32 credits any leftover at the camera handoff.
        unsigned credited=0;
        if(p>=48)credited=f.initial_carry;
        else if(p>=31)credited=std::min(f.initial_carry,(p-30)*unit_);
        f.remaining_carry=f.initial_carry-credited;
        f.clock_seconds=f.grant_seconds+credited;
        f.driving=t>=drive_tick();
        if(t>=after_green(timing_.first_clock_decrement) && f.clock_seconds)--f.clock_seconds;
        f.effect=-1;
        if(f.initial_carry && t==timing_.transfer_audio)f.effect=0x0b;
        if(f.initial_carry && t==timing_.flush_audio)f.effect=0x27;
        for(unsigned i=0;i<4;++i)if(t==signal_.audio[i])f.effect=i==3?0x4a:0x49;
        f.light_stage=unsigned(std::upper_bound(signal_.visible.begin(),signal_.visible.end(),t)-signal_.visible.begin());
        f.start_music=t==after_green(timing_.grid_music);
        f.elapsed_x=slide_left(t,timing_.elapsed_settled,148,212);
        const int boost_remaining=std::max(0,int(timing_.boost_settled)-int(t));
        f.boost_x=std::max(-83,-(boost_remaining*251/64));
        f.map_x=std::min(5,slide_right(t,timing_.map_settled,7,-33));
        f.marker_visible=t>=timing_.marker;
        const auto signal_out=after_green(timing_.signal_out);
        f.signal_x=t<signal_out?slide_right(t,timing_.signal_settled,-8,-72):
            std::max(-72,-8-4*static_cast<int>(1+t-signal_out));
        // The entering first-race housing is still offscreen at -56 on
        // the cropped display. All pieces remain active during retraction.
        f.signal_visible=f.signal_x>-64 && (!first_race_ || f.driving || f.signal_x>=-52);
        constexpr std::array<unsigned,10> fade{1,1,2,3,4,6,8,11,14,15};
        f.brightness=fade[std::min<std::size_t>(t,fade.size()-1)];
    }
    static MeasuredGrid measured_grid(GridCountdownRound round,unsigned profile_index,unsigned order_index) {
        if(round==GridCountdownRound::first)return first_[profile_index/2];
        if(round==GridCountdownRound::fourth)return fourth_[profile_index][order_index];
        return {signals_[profile_index][order_index],timings[profile_index],0};
    }
    GridCountdown(const MeasuredGrid& grid,unsigned grant_seconds,unsigned carry_seconds,unsigned spares,bool first_race):
        timing_(grid.timing),signal_(grid.signal),release_delay_(grid.release_delay),first_race_(first_race) {
        frame_.grant_seconds=grant_seconds;
        frame_.initial_carry=std::min(carry_seconds,99u);
        frame_.spares=spares;
        const auto bank=frame_.initial_carry;
        unit_=bank<32?1:bank<48?2:bank<64?3:4;
        next_camera_=signal_.scene_dwell.front()-'0';
        update();
    }
public:
    GridCountdown(unsigned speed_class,unsigned car,unsigned grant_seconds,unsigned carry_seconds,unsigned spares,
                  std::array<std::uint16_t,4> places={1,4,3,2},GridCountdownRound round=GridCountdownRound::third):
        GridCountdown(measured_grid(round,profile(speed_class,car,carry_seconds),grid_index(places)),
                      grant_seconds,carry_seconds,spares,round==GridCountdownRound::first) {}
    const GridCountdownFrame& frame() const{return frame_;}
    unsigned drive_tick() const{return signal_.audio.back()+release_delay_;}
    bool finished() const{return frame_.tick>=after_green(std::max(timing_.grid_music,timing_.signal_out+15));}
    void advance(){if(!finished()){++frame_.tick;update();}}
};
}
