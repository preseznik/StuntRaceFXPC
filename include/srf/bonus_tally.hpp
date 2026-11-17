#pragma once
#include "srf/championship.hpp"
#include "srf/race_audio.hpp"
#include <algorithm>

namespace srf {
enum class BonusTallyStart { moving, parked };

struct BonusTallyFrame {
    unsigned tick{},flag_seconds{},earned_cars{},remaining_seconds{},bank_seconds{},palette_phase{};
    int row_step=-1,banner_step=-1;
    unsigned clip_top{},clip_bottom=224,brightness=15;
    bool start_music{};
};

class BonusTally {
    struct Stages {
        std::array<unsigned,16> row;
        std::array<unsigned,5> banner;
        unsigned opening_cue,count_cue,fade_command,shutter,black,end;
    };
    struct Timing : Stages {
        std::array<unsigned,37> transfers; // 99 seconds needs at most 37 transfers.
        std::array<std::uint8_t,37> display_delay;
    };
    // Original object updates finish at different VBlanks depending on the
    // scene workload. These are the three measured class runs, from timer zero.
    // The first two tables are visible framebuffer edges. Transfer entries are
    // CPU updates at 07:904A. display_delay places the result on the later
    // framebuffer edge, sampled through a full 99-second transfer for each class.
    static constexpr std::array<Timing,3> schedules{{
        {{{11,15,20,25,29,34,39,43,48,53,58,62,67,72,76,81},
          {81,86,92,98,104},3,105,389,440,474,474},
         {111,117,123,128,134,140,146,151,157,163,169,174,180,186,191,197,203,209,214,
          220,225,231,237,242,248,254,259,265,271,276,282,288,293,299,305,310,316},
         {5,5,5,6,5,5,5,6,5,5,5,6,5,5,6,5,5,5,5,5,6,5,5,6,5,5,6,5,5,5,5,5,5,5,5,5,5}},
        {{{8,13,18,22,27,32,36,41,46,51,56,61,65,70,75,80},
          {80,85,91,97,103},1,105,404,456,486,486},
         {111,117,123,129,135,141,147,153,159,165,171,177,183,189,195,201,207,213,219,
          225,231,237,243,249,255,261,267,273,279,285,291,297,303,309,315,321,327},
         {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,4,5,4,5,4,4,5,5,5,5,5,5,5,4,4,4,4,4,4,4}},
        {{{11,15,19,24,28,33,37,41,46,50,55,59,63,68,72,77},
          {77,82,88,93,99},3,100,387,439,468,469},
         {106,111,117,123,129,134,140,146,152,157,163,169,175,180,186,192,198,203,209,
          215,221,226,232,238,244,249,255,261,267,272,278,284,290,295,301,307,313},
         {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5}}
    }};
    // Without a lap spare the original skips the third line at 07:A3D3.
    // Less time spent drawing it shortens the object callbacks across the tally.
    static constexpr std::array<Timing,3> no_lap_schedules{{
        {{{10,15,20,24,29,33,38,42,47,51,56,61,65,70,74,79},
          {79,84,89,95,101},3,102,378,427,461,461},
         {108,114,119,125,131,136,142,147,153,159,164,170,175,181,186,192,197,203,208,
          214,219,225,231,236,242,247,253,258,264,269,275,280,286,291,297,302,308},
         {4,4,5,5,4,5,4,5,4,4,5,4,5,4,5,4,5,4,5,4,5,4,4,5,4,5,4,5,4,5,4,5,4,4,4,4,4}},
        {{{8,13,17,22,26,31,35,40,45,49,54,59,63,68,73,78},
          {78,83,88,94,100},1,102,394,445,479,479},
         {108,114,120,125,131,137,143,149,155,161,167,173,179,185,191,197,203,209,214,
          220,226,232,238,244,250,256,262,268,274,279,285,291,297,302,308,314,320},
         {4,4,4,5,5,5,5,4,4,4,4,4,4,4,4,4,4,4,5,5,5,4,4,4,4,4,4,4,4,5,5,4,4,5,5,4,4}},
        {{{10,15,19,23,27,32,36,40,44,49,53,57,61,66,70,75},
          {75,80,85,90,96},3,97,377,427,461,462},
         {103,108,114,120,125,131,136,142,148,153,159,164,170,176,181,187,192,198,204,
          209,215,220,226,232,237,243,248,254,260,265,271,276,282,288,293,299,304},
         {4,5,4,4,5,4,5,4,4,5,4,5,4,4,5,4,5,4,4,5,4,5,4,4,5,4,5,4,4,5,4,5,4,4,5,4,5}}
    }};
    // From green to zero with neutral input, the truck stays at its original
    // grid X/Z. The GSU workload there produces different display edges.
    // Rows index the class, columns use the selected championship car.
    static constexpr auto parked_stages=[] {
        std::array<std::array<Stages,3>,3> p{};
        p[0][0]={{9,14,19,23,28,32,37,42,46,51,56,60,65,70,74,79},
                 {79,85,90,96,102},2,104,404,458,488,488};
        p[0][1]={{9,14,18,23,27,32,37,41,46,51,55,60,65,69,74,79},
                 {79,84,90,96,102},1,104,403,457,488,488};
        p[0][2]=p[0][0];
        p[1][0]={{10,14,19,23,28,32,37,42,46,51,55,60,65,69,74,79},
                 {79,84,89,95,101},2,103,398,452,482,482};
        p[1][1]={{8,12,17,21,26,31,35,40,44,49,53,58,63,67,72,77},
                 {77,82,88,93,99},0,101,396,450,480,480};
        p[1][2]=p[1][1];
        p[1][2].fade_command=397;
        p[2][0]={{7,12,16,21,25,29,34,38,42,47,51,55,60,64,69,73},
                 {73,78,84,89,95},0,96,382,433,462,463};
        p[2][1]=p[2][0];
        p[2][2]={{8,13,17,21,26,30,34,39,43,47,52,56,61,65,69,74},
                 {74,79,85,90,96},1,97,382,434,469,469};
        return p;
    }();
    const Timing& timing_;
    const Stages& stages_;
    BonusTallyFrame frame_;
    unsigned zero_tick_,remaining_,bank_,next_transfer_{},publish_{};
public:
    BonusTally(SpeedClass league,unsigned flags,unsigned earned_cars,unsigned previous_carry,
               unsigned zero_video_tick=0,BonusTallyStart start=BonusTallyStart::moving,PlayerCar car=PlayerCar::four_wd)
        :timing_((earned_cars?schedules:no_lap_schedules).at(static_cast<unsigned>(league))),
         stages_(start==BonusTallyStart::parked && !flags && !earned_cars
                 ?parked_stages.at(static_cast<unsigned>(league)).at(static_cast<unsigned>(car))
                 :static_cast<const Stages&>(timing_)),zero_tick_(zero_video_tick),
         remaining_(flags%100),bank_(std::min(99u,previous_carry)) {
        frame_.flag_seconds=frame_.remaining_seconds=remaining_;
        frame_.earned_cars=earned_cars;
        frame_.bank_seconds=bank_;
        frame_.palette_phase=((zero_tick_+2)>>1)&15;
        frame_.start_music=stages_.opening_cue==0;
    }
    const BonusTallyFrame& frame() const {return frame_;}
    bool finished() const {return frame_.tick>=stages_.end;}
    bool driving_digits_visible() const {return frame_.tick<stages_.opening_cue+3;}
    bool driving_sprites_visible() const {return frame_.tick<stages_.opening_cue+4;}

    // Some original class/car pairs enqueue the opening cue on the same
    // interrupt as timer zero, before the first subsequent call to advance().
    RaceAudioOutput opening_audio() const {
        RaceAudioOutput audio;
        if(frame_.tick==0 && frame_.start_music)audio.effects.push_back(0x15);
        return audio;
    }

    RaceAudioOutput advance() {
        RaceAudioOutput audio;
        if(finished())return audio;
        const auto t=++frame_.tick;
        frame_.start_music=t==stages_.opening_cue;
        if(frame_.start_music)audio.effects.push_back(0x15);
        frame_.palette_phase=((zero_tick_+t+2)>>1)&15;
        const auto rows=std::upper_bound(stages_.row.begin(),stages_.row.end(),t)-stages_.row.begin();
        frame_.row_step=rows?static_cast<int>(rows):-1;
        frame_.banner_step=static_cast<int>(std::upper_bound(stages_.banner.begin(),stages_.banner.end(),t)-stages_.banner.begin())-1;
        if(t==stages_.count_cue)audio.effects.push_back(0x40);
        // Present the previous completed transfer before calculating another.
        if(t==publish_) {
            frame_.remaining_seconds=remaining_;
            frame_.bank_seconds=bank_;
        }
        if(remaining_ && bank_<99 && next_transfer_<timing_.transfers.size() && t==timing_.transfers[next_transfer_]) {
            publish_=t+timing_.display_delay[next_transfer_++];
            const unsigned units=remaining_>=9?3:1;
            remaining_-=units;
            bank_=std::min(99u,bank_+units);
            if(!remaining_)audio.effects.push_back(0x44);
            if(bank_==99)audio.effects.push_back(0x44);
        }
        // CPU07:E04D sends command 4 ten scene callbacks before the shutter.
        if(t==stages_.fade_command)audio.commands.push_back(4);
        if(t>stages_.shutter) {
            const auto step=t-stages_.shutter;
            // Original window masks the screen above and below a shrinking
            // horizontal strip around scanline 120. The last strip is blanked.
            frame_.clip_top=step>=29?224:4*step-1;
            frame_.clip_bottom=step>=29?224:std::min(224u,242-4*step);
            // The Master viewport blanks one VBlank before the last bezel
            // scanlines. Novice holds the fully closed mask for five VBlanks.
            if(t>=stages_.black)frame_.brightness=0;
        }
        return audio;
    }
};
}
