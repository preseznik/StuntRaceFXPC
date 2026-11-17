#include "srf/speed_bonus.hpp"
#include "srf/audio.hpp"
#include "srf/backdrop.hpp"
#include "../src/bonus_hud_boost.hpp"
#include "bonus_hud_map_checks.hpp"
#include "bonus_tally_checks.hpp"
#include "course_map_checks.hpp"
#include "course_map_art_checks.hpp"
#include "grid_countdown_checks.hpp"
#include "grid_countdown_art_checks.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void boost_gauge(const srf::Rom& rom) {
    srf::BonusHudBoost gauge(rom,1);
    srf::VehicleState truck;
    truck.word(0x6a)=384;
    std::vector<std::uint32_t> pixels(208*128);
    // Original Expert/Coupe: Y held through green, released on video tick 831.
    // The observed fill pixel follows the palette ramp, saturation and decay.
    constexpr std::array<std::pair<unsigned,std::uint16_t>,10> samples{{
        {544,0x7e63},{545,0x7fe6},{549,0x7f85},{659,0x7ec4},{665,0x7d41},
        {724,0x7f25},{818,0x7e02},{832,0x7ec4},{860,0x7fe6},{930,0x7e63}}};
    unsigned tick=0;
    for(const auto [sample,color]:samples) {
        while(tick<sample){++tick;gauge.advance(tick<831,tick>=544);}
        gauge.draw(pixels,tick,truck);
        require(pixels[118*208+26]==srf::hud_rgba(color),"Boost palette differs from original video");
    }
    // Original body +6A advances the bar by one pixel at each multiple of 8.
    for(unsigned reserve=0;reserve<=384;++reserve) {
        truck.word(0x6a)=static_cast<std::uint16_t>(reserve);
        gauge.draw(pixels,tick,truck);
        for(unsigned x=0;x<48;++x)
            require(pixels[118*208+7+x]==srf::hud_rgba(x<reserve/8?0x7e63:0x0821),"Wrong partial boost segment");
    }
}
void loading_audio(const srf::Rom& rom,const srf::SoundBank& bank) {
    const auto intro=srf::extract_trailer_loading_music(rom);
    srf::MusicSequence score(bank);
    score.select_track(intro);
    // The six active channels in the US Rev 1 Expert loading shot, then
    // their final cursors in the silence before the start signal.
    constexpr std::array<unsigned,6> cue{0xcb71,0xcbab,0xcbec,0xcc33,0xcc7c,0xccaf};
    constexpr std::array<unsigned,6> end{0xcb84,0xcbbc,0xcbfe,0xcc5c,0xcc8f,0xccc2};
    auto matches=[&](const auto& reference) {
        for(unsigned i=0;i<reference.size();++i)if(score.channels()[i].cursor!=reference[i])return false;
        return true;
    };
    bool heard_cue=false;
    for(unsigned i=0;i<2000 && score.playing();++i) {score.tick();heard_cue|=matches(cue);}
    require(heard_cue && !score.playing() && matches(end),"Loading fanfare differs from the original sequence");
    score.select_track({});
    require(score.playing() && score.ticks()==0 && score.channels()[0].cursor==0xefa6,"Driving song did not restart");
    srf::MusicRenderer renderer(bank);
    renderer.play_music(intro);
    std::vector<std::int16_t> samples(32000*2*6);
    renderer.render(samples);
    require(std::any_of(samples.begin(),samples.end(),[](auto s){return s>100;}),"Silent loading fanfare");
    renderer.play_music();
    renderer.render(samples);
    require(std::any_of(samples.begin(),samples.end(),[](auto s){return s>100;}),"Silent bonus driving music");
    // DSP voices 6/7 continue with the idling tractor after the fanfare ends.
    renderer.play_music(intro,true);
    renderer.set_engine(0,false,false);
    std::vector<std::int16_t> idle(32000*2*8);
    renderer.render(idle);
    require(std::any_of(idle.end()-32000,idle.end(),[](auto s){return s>100;}),"Silent truck during the countdown");
    require(bank.engine_profiles[0].pitch_base==6 && bank.engine_profiles[1].volume_base==32,"Wrong trailer engine profile");
}
void tally_music(const srf::Rom& rom,const srf::SoundBank& bank) {
    const auto track=srf::extract_bonus_tally_music(rom);
    srf::MusicSequence score(bank);
    score.select_track(track);
    std::array<bool,8> voices{};
    while(score.ticks()<2000 && score.playing())
        for(const auto& event:score.tick())
            if(event.command>=0x80 && event.command<0xc8)voices.at(event.channel)=true;
    require(!score.playing() && score.ticks()==193 && std::count(voices.begin(),voices.end(),true)==6,
            "Incomplete six-channel Bonus Trax results music");
    constexpr std::array<unsigned,8> end{0xc949,0xc97f,0xc9e1,0xca14,0xca43,0xca74,0,0};
    for(unsigned i=0;i<end.size();++i)require(score.channels()[i].cursor==end[i],"Wrong tally song channel ending");
    srf::MusicRenderer renderer(bank);
    renderer.play_music(track);
    renderer.set_engine(0,false,false,false);
    std::vector<std::int16_t> samples(32000*2*6);
    renderer.render(samples);
    require(std::any_of(samples.begin(),samples.end(),[](auto s){return s>100;}),"Silent Bonus Trax results song");
}
void course_map_audio(const srf::Rom& rom,const srf::SoundBank& bonus_bank) {
    // The original SPC channel cursors at tally black and during the board's first phrase.
    const auto reaches=[](srf::MusicSequence& sequence,std::array<unsigned,8> cursors) {
        for(unsigned tick=0;tick<2000 && sequence.playing();++tick) {
            sequence.tick();
            if(std::equal(cursors.begin(),cursors.end(),sequence.channels().begin(),
                          [](auto cursor,const auto& voice){return cursor==voice.cursor;}))return true;
        }
        return false;
    };
    const auto outro=srf::extract_bonus_tally_outro(rom);
    srf::MusicSequence tally(bonus_bank);
    tally.select_track(outro);
    require(reaches(tally,{0xc79f,0xc7fa,0xc8a6,0xc8bb,0,0,0xc8cd,0}),"Wrong tally outro score");
    const auto menu=srf::extract_course_map_audio(rom);
    srf::MusicSequence board(menu);
    require(menu.music.size()==855 && reaches(board,{0xee96,0xef08,0xefb9,0xf043,0,0,0xf0a7,0xf11c}),
            "Wrong course map song");
    require(reaches(board,{0xeeac,0xef27,0xefce,0xf059,0,0,0xf0bd,0xf13a}) &&
            reaches(board,{0xee96,0xef08,0xefb9,0xf043,0,0,0xf0a7,0xf11c}),"Course map song failed to loop");
    srf::MusicRenderer renderer(menu);
    renderer.set_engine(0,false,false,false);
    std::vector<std::int16_t> buffer(32000*2);
    for(auto cue:{0x2f,0x2d,0x24,0x46}) {
        renderer.play_effect(cue-5);
        renderer.render(buffer);
        require(std::any_of(buffer.begin(),buffer.end(),[](auto s){return s>100;}),"Silent course map audio");
    }
    renderer.sound_command(5);
    for(int second=0;second<3;++second)renderer.render(buffer);
    require(std::none_of(buffer.begin(),buffer.end(),[](int s){return std::abs(s)>8;}),"Map music continued after bank fade");
    srf::MusicRenderer results(bonus_bank);
    results.set_engine(0,false,false,false);
    results.play_music(srf::extract_bonus_tally_music(rom),false,outro);
    for(int second=0;second<7;++second)results.render(buffer);
    require(std::any_of(buffer.begin(),buffer.end(),[](auto s){return s>100;}),"Tally music did not continue into the board intro");
}
void parked_trailer(const srf::Rom& rom,srf::SpeedClass league) {
    for(const bool drive_away:{false,true}) {
        srf::SpeedBonus event(rom,league);
        for(unsigned phase=0;phase<800 && !event.ended();++phase) {
            // Steer at rest, then optionally tap the accelerator for one phase.
            const unsigned buttons=phase==80 && drive_away?0x10:0;
            event.step(buttons,4,phase<60?16000:0);
        }
        require(event.ended() && event.reward().extra_seconds==0 && event.reward().extra_cars==0 &&
                event.truck().body.word(0x4a)==0,"Expected a stopped zero-award Trailer");
        require(event.stayed_parked()==!drive_away,"A stopped or steering Trailer selected the wrong tally");
    }
}
void drive(const srf::Rom& rom,srf::SpeedClass league) {
    srf::SpeedBonus run(rom,league);
    const auto cls=static_cast<unsigned>(league);
    constexpr std::array<unsigned,3> nodes{19,13,13};
    require(run.clock().remaining_seconds==50 && run.gates().size()==22 && run.scene().points.size()==nodes[cls],
            "Incorrect original bonus course or starting clock");
    require(run.truck().body.word(0x66)==0x8669 && run.truck().body.word(0x6e)==0x86dd,"Incorrect trailer tuning");
    unsigned previous=0;
    // In the USA Rev 1 reference, holding throttle on Expert first drops two flag
    // pairs with the truck center at Z=6090 and 17653. Allow one native phase.
    for(unsigned i=0;i<800 && !run.ended();++i) {
        run.step(0x10,4);
        const auto reward=run.reward();
        if(cls==1 && reward.extra_seconds>previous && previous<2) {
            const auto expected=previous==0?6090:17653;
            const auto z=static_cast<std::int16_t>(run.truck().body.word(0x22));
            require(std::abs(int(z)-expected)<=220,"Flag did not fall at the original crossing");
        }
        if(!run.ended())require(run.collected_reward().extra_seconds==0 && run.collected_reward().extra_cars==reward.extra_cars,
                                "Voluntary retirement banked flags or removed a lap spare");
        previous=reward.extra_seconds;
    }
    require(run.ended() && run.reward().extra_seconds>0,"Timer failed to end the bonus or gates could not score");
    require(run.collected_reward().extra_seconds==run.reward().extra_seconds,"Timeout did not bank flag rewards");
    if(cls==0)require(run.reward().extra_cars>0,"Completed lap did not award a spare car");
    const auto stopped=run.truck().body.words;
    run.step(0x10,4);
    require(run.truck().body.words==stopped,"Expired bonus continued driving");
    const auto banked=run.collected_reward();
    for(unsigned phase=0;phase<35;++phase) {
        run.coast_after_timeout();
        require((run.truck().body.word(0x42)&0x0430)==0,"Results coast accepted throttle, boost or reverse");
        require(run.ended() && run.collected_reward().extra_seconds==banked.extra_seconds &&
                run.collected_reward().extra_cars==banked.extra_cars,"Results coast changed a banked reward");
    }
}
}
int wmain(){
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        boost_gauge(rom);
        check_bonus_hud_maps(rom);
        check_bonus_tally(rom);
        check_course_map();
        check_course_map_art(rom);
        check_grid_countdown();
        check_grid_countdown_art(rom);
        for(auto league:{srf::SpeedClass::novice,srf::SpeedClass::expert,srf::SpeedClass::master}) {
            drive(rom,league);
            parked_trailer(rom,league);
        }
        const auto audio=srf::extract_speed_bonus_audio(rom);
        require(audio.music.size()==1803,"Wrong Bonus Trax music bank");
        loading_audio(rom,audio);
        tally_music(rom,audio);
        course_map_audio(rom,audio);
        require(srf::extract_speed_bonus_backdrop(rom).available(),"Bonus panorama unavailable");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
