#include "srf/audio.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void advance(srf::MusicRenderer& renderer,unsigned frames) {
    std::vector<std::int16_t> pcm(frames*2);renderer.render(pcm);
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path){std::cout<<"SRF_ROM is required\n";return 77;}
        const auto bank=srf::extract_course_audio(srf::Rom::load(path),srf::CourseId::aqua_tunnel);
        srf::MusicRenderer renderer(bank);renderer.set_music_enabled(false);
        std::ifstream reference(std::filesystem::path(SRF_TEST_FIXTURES)/"aqua_tunnel_echo_reference.csv");
        require(bool(reference),"Missing original SPC tunnel reference");
        std::string line;std::getline(reference,line);unsigned rows=0;
        while(std::getline(reference,line)) {
            std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
            unsigned tick,command,flags,amount,stage,mask,feedback,left,right;
            require(bool(row>>tick>>command>>flags>>amount>>stage>>mask>>feedback>>left>>right) && tick==rows,
                    "Malformed original SPC tunnel reference");
            if(command)renderer.sound_command(static_cast<std::uint8_t>(command));
            advance(renderer,512);
            const auto signed_byte=[](unsigned value){return value<128?int(value):int(value)-256;};
            require(renderer.echo_state()==srf::EchoState{mask,amount,stage,signed_byte(feedback),signed_byte(left),signed_byte(right)},
                    "Native tunnel reverb differs from original SPC handler trace");
            ++rows;
        }
        require(rows==145,"Incomplete original SPC reference");
        renderer.reset();renderer.set_music_enabled(false);
        // SPC15C4..16B0 with initially silent echo. Entry takes two control
        // ticks before the first coefficient change, then holds at half reverb.
        advance(renderer,511);renderer.sound_command(0x11);
        require(renderer.echo_state()==srf::EchoState{},"Command must wait for sound timer");
        advance(renderer,1);
        require(renderer.echo_state()==srf::EchoState{0,0,1,0,0,0},"Entry arm at512 samples");
        advance(renderer,512);
        require(renderer.echo_state()==srf::EchoState{0,1,1,0,0,0},"Entry saves original echo before ramp");
        advance(renderer,29*512);
        require(renderer.echo_state()==srf::EchoState{0xc0,30,2,50,50,50},"Half reverb sends both engine voices");
        renderer.sound_command(0x10);advance(renderer,512);
        require(renderer.echo_state().stage==3 && renderer.echo_state().amount==30,"Exit arms before decrement");
        advance(renderer,30*512);
        require(renderer.echo_state()==srf::EchoState{0xc0,0,3,0,0,0},"Exit reaches original coefficient before restoring mask");
        advance(renderer,512);
        require(renderer.echo_state()==srf::EchoState{},"Exit restores original echo state");

        const auto sequence=[&](unsigned chunk) {
            renderer.reset();renderer.set_music_enabled(false);renderer.set_engine(120,true,false);
            std::vector<std::int16_t> output;
            for(unsigned segment=0;segment<4;++segment) {
                if(segment==0 || segment==2)renderer.sound_command(0x11);
                else renderer.sound_command(0x10);
                // Includes reversal during entry and during exit, plus SFX.
                if(segment==2)renderer.play_effect(0x2e - 5);
                const unsigned frames=segment==0?7*512:32000;
                for(unsigned at=0;at<frames;) {
                    const auto count=std::min(chunk,frames-at);
                    const auto start=output.size();
                    output.resize(start+count*2);
                    renderer.render(std::span(output).subspan(start));at+=count;
                }
            }
            return output;
        };
        const auto whole=sequence(32000);
        require(whole==sequence(257),"Tunnel sound changes with audio callback size or reset");
        require(std::any_of(whole.begin(),whole.end(),[](auto sample){return sample!=0;}),"Tunnel engine/effects are silent");
        srf::MusicRenderer dry(bank),wet(bank);
        for(auto* sound:{&dry,&wet}){sound->set_music_enabled(false);sound->set_engine(120,true,false);}
        wet.sound_command(0x11);
        std::vector<std::int16_t> dry_pcm(64000),wet_pcm(64000);
        dry.render(dry_pcm);wet.render(wet_pcm);
        require(dry_pcm!=wet_pcm,"Tunnel reverb must affect rendered engine samples");
        // A tiny authored sequence schedules F5 on the same timer boundary as
        // the ramp. Original08:089C..08B8 runs tunnel control before music.
        auto scheduled=bank;scheduled.music.assign(64,0);
        const auto pointer=[&](unsigned at,unsigned target) {
            scheduled.music[at]=static_cast<std::uint8_t>(target);
            scheduled.music[at+1]=static_cast<std::uint8_t>(target>>8);
        };
        pointer(0,scheduled.music_base+4);pointer(4,scheduled.music_base+20);
        const std::vector<std::uint8_t> commands{0xf5,63,50,40,0xf7,1,192,0,1,0xc9,0xc9,0xf5,7,25,35,0xc9,0};
        std::copy(commands.begin(),commands.end(),scheduled.music.begin()+20);
        srf::MusicRenderer ordered(scheduled);ordered.sound_command(0x11);
        advance(ordered,3*512);
        require(ordered.echo_state()==srf::EchoState{7,2,1,-59,25,35},"Song echo command must follow coincident tunnel control");
        advance(ordered,512);
        require(ordered.echo_state()==srf::EchoState{0xc0,3,1,-56,52,52},"Tunnel ramp resumes on next control tick");
        ordered.sound_command(0x10);advance(ordered,70*512);
        require(ordered.echo_state()==srf::EchoState{63,0,0,-64,50,40},"Tunnel exit must restore signed feedback and asymmetric song volumes");
        renderer.reset();require(renderer.echo_state()==srf::EchoState{},"Restart retains tunnel state");
        std::cout<<"145 original SPC snapshots, tunnel entry/exit, engine send, reversal, sample scheduling and reset passed\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
