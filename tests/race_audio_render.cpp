#include "srf/audio.hpp"
#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"
#include "srf/environment.hpp"
#include "srf/scenery.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=4 && argc!=5)throw std::invalid_argument("Expected ROM, input CSV, output WAV and optional course slug");
        const auto course=argc==5?srf::parse_course(std::filesystem::path(argv[4]).string()):srf::CourseId::easy_ride;
        const auto rom=srf::Rom::load(argv[1]);
        const auto bank=srf::extract_course_audio(rom,course);
        const auto tuning=srf::extract_race_audio_tuning(rom.bytes());
        srf::NativeRace race(rom,srf::PlayerCar::four_wd,course);srf::RaceAudioState audio_state;
        srf::CourseEnvironment environment(rom,course);
        srf::CourseScenery scenery(rom,course);
        srf::MusicRenderer renderer(bank);renderer.set_music_enabled(false);
        std::ifstream input{std::filesystem::path(argv[2])};
        if(!input)throw std::runtime_error("Cannot open replay input");
        std::string line;std::getline(input,line);
        if(line!="phase,requested,clock_ticks")throw std::runtime_error("Expected ordinary player input replay columns");
        std::vector<std::int16_t> pcm;unsigned phase=0,effect_count=0,command_count=0,environment_effects=0,environment_commands=0,scenery_effects=0;
        while(std::getline(input,line)) {
            std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
            unsigned index,requested,ticks;
            if(!(row>>index>>requested>>ticks)||index!=phase||requested>65535||ticks>10)
                throw std::runtime_error("Invalid replay row");
            const auto previous=race.countdown_stage();
            const auto previous_lap=race.completion().acknowledged_laps;
            const auto player_before=race.racers()[race.player_index()].body;
            srf::RaceAudioOutput input_events;
            const auto controls=srf::prepare_player_input(audio_state,race.racers()[race.player_index()].body,
                static_cast<std::uint16_t>(requested),race.countdown_active(),input_events);
            race.step(controls,static_cast<std::uint8_t>(ticks));
            environment.advance_video(ticks);
            environment.step(player_before,previous_lap,race.elapsed_centiseconds());
            scenery.step(player_before,previous_lap);
            const auto& vehicle=race.racers()[race.player_index()];
            srf::RaceAudioInput audio_input;audio_input.prestart=race.countdown_active();
            audio_input.throttle=(requested&0x10)!=0;
            audio_input.counter=static_cast<std::uint16_t>(race.phase());
            if(course==srf::CourseId::sea_breeze || course==srf::CourseId::white_land || course==srf::CourseId::night_cruise ||
               course==srf::CourseId::lake_side || course==srf::CourseId::big_ravine || course==srf::CourseId::sky_ramp ||
               course==srf::CourseId::harbor_city)
                audio_input.terrain_flags=vehicle.body.word(0x8a);
            auto events=srf::update_race_audio(audio_state,tuning,vehicle,audio_input);
            events.effects.insert(events.effects.begin(),input_events.effects.begin(),input_events.effects.end());
            srf::append_countdown_audio(previous,race.countdown_stage(),events);
            const auto& surroundings=environment.events();
            events.commands.insert(events.commands.end(),surroundings.sound_commands.begin(),surroundings.sound_commands.end());
            events.effects.insert(events.effects.end(),surroundings.effects.begin(),surroundings.effects.end());
            environment_effects+=surroundings.effects.size();environment_commands+=surroundings.sound_commands.size();
            const auto scenery_sounds=scenery.effects();
            events.effects.insert(events.effects.end(),scenery_sounds.begin(),scenery_sounds.end());
            scenery_effects+=scenery_sounds.size();
            for(const auto sounds:{race.rock_effects(),race.cone_effects(),race.prop_effects(),race.pickup_effects(),race.breakable_effects(),race.bonus_effects()}) {
                events.effects.insert(events.effects.end(),sounds.begin(),sounds.end());
                scenery_effects+=sounds.size();
            }
            effect_count+=events.effects.size();command_count+=events.commands.size();
            renderer.update_race(events,audio_input.throttle,(vehicle.body.word(0x42)&0x400)!=0,
                                 !race.ended(),!race.countdown_active()&&!race.ended());
            const auto start=pcm.size();++phase;
            const auto end=std::size_t(phase)*32000/14*2;pcm.resize(end);
            renderer.render(std::span(pcm).subspan(start));
        }
        if(!race.completion().finished||race.failure()!=srf::RaceFailure::none)
            throw std::runtime_error("Audio replay did not complete a valid race");
        unsigned peak=0,clipped=0;std::uint64_t energy=0;
        for(auto sample:pcm) {
            peak=std::max(peak,unsigned(std::abs(int(sample))));
            clipped+=sample==32767||sample==-32768;energy+=std::uint64_t(int(sample)*int(sample));
        }
        if(!energy||!effect_count)throw std::runtime_error("Race sound output is empty");
        std::ofstream output{std::filesystem::path(argv[3]),std::ios::binary};
        const auto word=[&](std::uint32_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)output.put(char(value>>(i*8)));};
        output.write("RIFF",4);word(36+pcm.size()*2,4);output.write("WAVEfmt ",8);word(16,4);
        word(1,2);word(2,2);word(32000,4);word(128000,4);word(4,2);word(16,2);
        output.write("data",4);word(pcm.size()*2,4);
        output.write(reinterpret_cast<const char*>(pcm.data()),static_cast<std::streamsize>(pcm.size()*2));
        if(!output)throw std::runtime_error("Cannot write race audio");
        std::cout<<"{\"phases\":"<<phase<<",\"frames\":"<<pcm.size()/2<<",\"effects\":"<<effect_count
                 <<",\"commands\":"<<command_count<<",\"peak\":"<<peak<<",\"clipped_samples\":"<<clipped
                 <<",\"environment_effects\":"<<environment_effects<<",\"environment_commands\":"<<environment_commands
                 <<",\"scenery_effects\":"<<scenery_effects
                 <<",\"finished\":true,\"failure\":0}\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
