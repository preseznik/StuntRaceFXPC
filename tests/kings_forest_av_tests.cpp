#include "srf/audio.hpp"
#include "srf/backdrop.hpp"
#include "srf/environment.hpp"
#include "srf/race_audio.hpp"
#include "srf/world.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void append(std::vector<std::uint8_t>& bytes,unsigned value,unsigned length) {
    for(unsigned i=0;i<length;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));
}
void check_audio(const srf::Rom& rom) {
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::kings_forest));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"King's Forest sound package did not round trip");
    require(srf::sha256(bank.music)=="59e59490bf28650d2e8c367e47eb9e41cd6d728aead3021ff3c3d281083bb679",
            "King's Forest music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==30 && srf::sha256(samples)=="0134884182ae9828b549ce968e1085a8c7e5615163d53ad5c33960f0d5375b17",
            "King's Forest sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="aa6d2d635619f9c68259b5d41d100d4d34b0c95243457c005faf9e50e4312b69",
            "King's Forest retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{225,338,445,557,665,775,887,995,1105};
    srf::MusicSequence sequence(bank);
    std::vector<std::uint8_t> states;
    for(int i=0;i<=ticks.back();++i) {
        sequence.tick();
        if(!std::binary_search(ticks.begin(),ticks.end(),i))continue;
        for(const auto& ch:sequence.channels()) {
            append(states,ch.cursor,2);
            states.insert(states.end(),{ch.remaining,ch.duration,ch.gate,ch.volume,ch.repeat_count,ch.instrument});
        }
    }
    require(srf::sha256(states)=="8136a1d11f6da81a591165cf442f76d3333ef86401b77f98f6efbd03b9accee3",
            "King's Forest sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "5f9ea1b02a4f0f8a2c50bd2ff73d2365696bee1329be2bfc39f7db3b6a6bf306","King's Forest music rendering changed");
    renderer.reset();renderer.set_music_enabled(false);renderer.sound_command(0x13);
    auto clip=std::span(pcm).first(8192);
    renderer.render(clip);
    const auto playing=renderer.effect_states();
    require(std::any_of(playing.begin(),playing.end(),[](const auto& ch){return ch.id==0x21;}) &&
            std::any_of(clip.begin(),clip.end(),[](auto sample){return sample!=0;}),"Forest bridge control failed to start effect 21");
    renderer.sound_command(0x0e);renderer.render(clip);
    const auto stopped=renderer.effect_states();
    require(std::none_of(stopped.begin(),stopped.end(),[](const auto& ch){return ch.id==0x21;}),"Forest bridge control failed to stop effect 21");
    renderer.reset();
    srf::RaceAudioOutput collision;
    collision.effects.push_back(0x58); // CPU tree contact dispatches SPC sequence 53.
    renderer.update_race(collision,false,false,false,false);
    renderer.render(clip);
    require(std::any_of(clip.begin(),clip.end(),[](auto sample){return sample!=0;}),"Forest tree collision is silent");
}

void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::kings_forest;
    const auto bg=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(bg.indices[0])=="4adaed2106367d4aabf1060d38a9f61a5fd84a8884c250287090c7bb07c79a5e",
            "Forest panorama differs from original map and glyphs");
    struct View {short yaw,pitch;unsigned fog,weather;int x,y,w,h;const char* hash;};
    constexpr View captures[]{
        {0,-3184,15,0,0,4,60,9,"6c7b077f729caa23e8f7286e934470b424beab746ff9027d1599a88114fe359c"},
        {32528,-2971,9,0,58,4,96,9,"b19066d964ca4c8f5ae517ffbcffde31d5b760d3d787d67186860d9ab991ab49"},
        {16624,-2928,0,16,76,4,30,9,"ddb8188b0cb49dc3b8f37ca98bc6e8efc0357f77e5d54d2bde4df6f0de0e7c93"},
        {-240,-2899,0,31,40,4,96,9,"4c4bb1ca4012d96e8718b5455fe0a5a0d4f6fd975df82f3637c71bcd300ea341"}};
    for(const auto& c:captures) {
        std::vector<std::uint8_t> pixels;
        const auto view=srf::backdrop_view(c.yaw,c.pitch);
        for(int y=c.y;y<c.y+c.h;++y)for(int x=c.x;x<c.x+c.w;++x) {
            const auto i=srf::backdrop_index(bg,0,view,x,y);
            const auto color=[&](unsigned mode){return i==1?bg.gradients[mode][view.gradient_y+y]:bg.palettes[mode][i];};
            const auto sky=srf::blend_environment_rgb555(color(0),color(1),c.weather);
            append(pixels,srf::blend_environment_rgb555(sky,0x7fff,c.fog),2);
        }
        require(srf::sha256(pixels)==c.hash,"Forest skyline differs from original displayed race");
    }
    const auto bytes=srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course));
    const auto sprites=srf::decode_world_sprites(bytes);
    require(srf::encode_world_sprites(sprites)==bytes,"Forest indexed sprite package did not round trip");
    struct Sprite {unsigned table,phase,address;const char* hash;};
    constexpr Sprite references[]{
        {0x9971,0,0x12a060,"50a4d167e49e0ee0bbc98bbb540c49d3a2f01ee081fabaa44f1e409ad65dbda6"},
        {0x9973,0,0x12c000,"d09ce583d37745f71f3f1b44ff89fbfa78f83ecdf34e3dc618cf748a3883515e"},
        {0x8f91,0xd5,0x13e020,"91ee97060f19181881e7c67092e8e39b45d74a2953ce238585ec0974f934e0cf"}};
    for(const auto& ref:references) {
        const auto found=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s) {
            return s.material_table==ref.table && s.phase==ref.phase;
        });
        require(found!=sprites.end() && found->source_address==ref.address && srf::sha256(found->palette_indices)==ref.hash,
                "Tree or grandstand texture differs from original ROM");
        for(std::size_t i=0;i<found->pixels.size();++i)
            require((found->pixels[i]>>24!=0)==((found->palette_indices[i]&15)!=0),"Original forest texture cutout differs");
    }
}

}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);
        std::cout<<"King's Forest original music, bridge sound, fog, skyline and indexed sprites passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
