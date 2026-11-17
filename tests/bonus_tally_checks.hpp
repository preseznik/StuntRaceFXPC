#pragma once
#include "../src/bonus_tally_art.hpp"
#include "srf/audio.hpp"
#include "srf/bonus_tally.hpp"

inline void check_bonus_tally_art(const srf::Rom& rom) {
    // Original framebuffer comparisons cover the overlapping slide, first-frame
    // overdraw, expanding banner, two-lap award and the zero-lap layout.
    struct Capture {
        srf::SpeedClass league;
        unsigned flags,laps,bank,timeout,tick;
        std::uint32_t fingerprint;
    };
    constexpr Capture captures[]{
        {srf::SpeedClass::novice,2,1,83,3549,11,0x6ed627ee},
        {srf::SpeedClass::expert,3,2,87,3552,80,0x716aaeb3},
        {srf::SpeedClass::master,3,1,99,3552,90,0x0b8a4491},
        {srf::SpeedClass::expert,3,2,87,3552,180,0x7bdfd0e7},
        {srf::SpeedClass::expert,0,0,87,3552,180,0x3bf2ce24}
    };
    const srf::BonusTallyArt art(rom);
    for(const auto& capture:captures) {
        srf::BonusTally tally(capture.league,capture.flags,capture.laps,capture.bank,capture.timeout);
        while(tally.frame().tick<capture.tick)tally.advance();
        std::vector<std::uint32_t> pixels(208*128);
        art.draw(pixels,tally.frame());
        std::uint32_t hash=2166136261u;
        for(const auto pixel:pixels)hash=(hash^pixel)*16777619u;
        if(hash!=capture.fingerprint)throw std::runtime_error("Bonus Trax tally differs from original capture");
    }
}

inline void check_bonus_tally(const srf::Rom& rom) {
    const auto require=[](bool value,const char* message) {
        if(!value)throw std::runtime_error(message);
    };
    struct Reference {
        unsigned flags,laps,previous,carry,pending,song,row,banner,shutter,black,end;
    };
    constexpr Reference runs[]{
        {2,1,83,85,0,3,11,81,440,474,474},
        {3,2,87,90,0,1,8,80,456,486,486},
        {3,1,136,99,3,3,11,77,439,468,469}
    };
    for(unsigned c=0;c<std::size(runs);++c) {
        const auto ref=runs[c];
        srf::BonusTally tally(static_cast<srf::SpeedClass>(c),ref.flags,ref.laps,ref.previous,3552);
        std::vector<std::uint8_t> effects,commands;
        for(unsigned t=1;t<=ref.end;++t) {
            require(!tally.finished(),"Original results screen ended early");
            const auto audio=tally.advance();
            effects.insert(effects.end(),audio.effects.begin(),audio.effects.end());
            commands.insert(commands.end(),audio.commands.begin(),audio.commands.end());
            const auto& frame=tally.frame();
            require(frame.start_music==(t==ref.song) && frame.palette_phase==(((3552+t+2)>>1)&15),
                    "Incorrect Bonus results song or palette clock");
            if(t<=ref.row)require(frame.row_step==(t==ref.row?1:-1),"Original results lettering entered at the wrong time");
            if(t==ref.banner)require(frame.banner_step==0 && frame.row_step==16,"Incorrect EXTRA TIME banner entry");
            if(t==ref.shutter+12)require(frame.clip_top==47 && frame.clip_bottom==194,"Incorrect results screen shutter");
            require(frame.brightness==(t>=ref.black?0u:15u),"Results screen blanked at the wrong time");
        }
        std::vector<std::uint8_t> expected_effects{0x15,0x40};
        if(ref.pending==0)expected_effects.push_back(0x44);
        require(effects==expected_effects && commands==std::vector<std::uint8_t>{4},"Original tally sound cues changed");
        require(tally.finished() && tally.frame().remaining_seconds==ref.pending &&
                tally.frame().bank_seconds==ref.carry && tally.frame().earned_cars==ref.laps,
                "Original Bonus Trax reward transfer changed");
        tally.advance();
        require(tally.frame().tick==ref.end,"Completed tally continued ticking");
    }
    // 07:904A consumes three seconds per pass at nine or above, even if
    // only one slot remains. At 99 it leaves the rest on the right display.
    srf::BonusTally cap(srf::SpeedClass::expert,24,2,98);
    while(!cap.finished())cap.advance();
    require(cap.frame().remaining_seconds==21 && cap.frame().bank_seconds==99,"Incorrect capped three-second transfer");
    srf::BonusTally count(srf::SpeedClass::expert,9,2,0);
    while(count.frame().tick<116)count.advance();
    require(count.frame().bank_seconds==3 && count.frame().remaining_seconds==6,"Nine-second tally threshold changed");
    while(count.frame().tick<122)count.advance();
    require(count.frame().bank_seconds==4 && count.frame().remaining_seconds==5,"Single-second tally transfer changed");
    // The NO LAPS branch omits a drawn line and reaches the banner sooner.
    constexpr std::array<unsigned,3> no_laps_banner{79,78,75};
    for(unsigned c=0;c<no_laps_banner.size();++c) {
        srf::BonusTally empty(static_cast<srf::SpeedClass>(c),0,0,38);
        while(empty.frame().tick+1<no_laps_banner[c])empty.advance();
        require(empty.frame().banner_step<0,"No-lap tally entered the banner early");
        empty.advance();
        require(empty.frame().banner_step==0 && empty.frame().earned_cars==0,"No-lap banner handoff changed");
        while(!empty.finished())empty.advance();
        require(empty.frame().bank_seconds==38 && empty.frame().remaining_seconds==0,"Empty tally changed the bank");
    }

    // Entirely neutral driving in the original, for Coupe, 4WD and F-Type.
    // The shutter can leave gray scanlines between the last scene and full black.
    struct ParkedReference { unsigned cue,first_row,banner,last_scene,black_view,end; };
    constexpr ParkedReference parked[3][3]{
        {{2,9,79,486,488,488},{1,9,79,485,488,488},{2,9,79,486,488,488}},
        {{2,10,79,480,482,482},{0,8,77,478,480,480},{0,8,77,478,480,480}},
        {{0,7,73,461,462,463},{0,7,73,461,462,463},{1,8,74,462,469,469}}
    };
    for(unsigned c=0;c<3;++c)for(unsigned car=0;car<3;++car) {
        const auto ref=parked[c][car];
        srf::BonusTally run(static_cast<srf::SpeedClass>(c),0,0,83,3553,srf::BonusTallyStart::parked,
                            static_cast<srf::PlayerCar>(car));
        auto cues=run.opening_audio().effects;
        for(unsigned t=0;t<=ref.end;++t) {
            if(t) {
                const auto audio=run.advance();
                cues.insert(cues.end(),audio.effects.begin(),audio.effects.end());
            }
            const auto& f=run.frame();
            require(f.start_music==(t==ref.cue) && f.tick==t,"Incorrect parked music cue");
            require(run.driving_digits_visible()==(t<ref.cue+3) && run.driving_sprites_visible()==(t<ref.cue+4),
                    "Incorrect parked driving HUD clear");
            if(t<=ref.first_row)require(f.row_step==(t==ref.first_row?1:-1),"Incorrect parked row entry");
            if(t<=ref.banner)require(f.banner_step==(t==ref.banner?0:-1),"Incorrect parked banner entry");
            require((f.clip_top<f.clip_bottom)==(t<=ref.last_scene) && (f.brightness==0)==(t>=ref.black_view),
                    "Incorrect parked shutter or black frame");
            require(run.finished()==(t==ref.end),"Incorrect parked handoff");
        }
        require(cues==std::vector<std::uint8_t>{0x15,0x40} && run.frame().bank_seconds==83,
                "Parked tally changed award or repeated a cue");
    }

    check_bonus_tally_art(rom);
    const auto bank=srf::extract_speed_bonus_audio(rom);
    srf::MusicRenderer control(bank),exit(bank);
    exit.sound_command(4);
    std::vector<std::int16_t> control_sound(8192),exit_sound(8192);
    control.render(control_sound);
    exit.render(exit_sound);
    require(control_sound==exit_sound,"Original exit command 4 changed the SPC mix");
}
