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
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::white_land));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"White Land sound package did not round trip");
    require(srf::sha256(bank.music)=="fe4cb855ededd4f6af3be67a1b4c54e0b2af7e3b93b848245ec71f78d253b76b",
            "White Land music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==29 && srf::sha256(samples)=="bfd542f0c86ab029873d2c30d8c4c5393d1d7d399abaf060d55452d2fd3b6109",
            "White Land sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="49b6b12bf9fef78bbba1f98fb2e489dae0b3f8119f38520b77aa73134adf4a43",
            "White Land retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{225,336,445,556,666,775};
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
    require(srf::sha256(states)=="1d6262603945f88a4f3da4928656efcad1c722bc7b31c37574d85936a343aa89",
            "White Land sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "63fe8ee560075a584ec60064206cd30bf1d030c38b6823f4af258fbdc86e54d7","White Land music rendering changed");
    // First complete SPC tick after each CPU event, sampled while driving.
    struct Effect {std::uint8_t port,channel,sample,left,right;std::uint16_t cursor,pitch;};
    constexpr Effect effects[]{{0x10,6,18,0,0,48572,802},{0x11,6,10,50,50,48458,378},
                               {0x56,4,15,70,70,50194,675},{0x1b,4,2,43,20,48954,802},
                               {0x25,4,3,90,90,48995,954}};
    for(const auto ref:effects) {
        srf::MusicRenderer renderer(bank);
        srf::RaceAudioOutput hit;hit.engine_parameter=196;hit.effects.push_back(ref.port);
        renderer.update_race(hit,false,false,false,false);
        std::vector<std::int16_t> pcm(32000);
        renderer.render(std::span(pcm).first(1024));
        const auto voice=renderer.effect_states()[ref.channel];
        require(voice.id==ref.port-5 && voice.cursor==ref.cursor && voice.pitch==ref.pitch &&
                voice.sample==ref.sample && voice.left==ref.left && voice.right==ref.right,
                "White Land sound effect differs from original SPC voice");
        renderer.render(std::span(pcm).subspan(1024));
        require(std::any_of(pcm.begin(),pcm.end(),[](auto sample){return sample!=0;}),
                "White Land scene sound is silent");
    }
}

void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::white_land;
    const auto bg=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(bg.indices[0])=="746b62efd39ff62944e21d4b43a0bdae0ec72ede67401e14b4464f4d99dc60f8",
            "White Land panorama differs from original map and glyphs");
    struct View {short yaw,pitch;unsigned fog,weather;int x,y,w,h;const char* hash;};
    constexpr View captures[]{
        {0,-3184,10,0,52,2,20,10,"9cbc2d35e0a99d5e19ffe9294b50228e95617079dbb414f04dbd7258964d0295"},
        {-10368,-2964,1,0,2,2,48,10,"869c9b1496d977c4a834c1e684fffe66bd05865a4890ed5e942e9e18f31737b9"},
        {192,-2849,0,4,2,2,80,10,"a08a3336c1d3782dd7753fac56824e9753494a30ca59f46da972120fc5515263"},
        {11920,-2981,0,30,110,24,48,7,"1e47389c44a028d57661ab915a00924f4ba73d1f7ab34c2fe83e4c63bc48ae1e"},
        {-240,-2924,0,31,2,2,48,10,"fda7f3826407455e5022de4d37ddd08d1ae1fc1a887153e807e784a7d38f0bf0"}};
    for(const auto& c:captures) {
        std::vector<std::uint8_t> pixels;
        const auto view=srf::backdrop_view(c.yaw,c.pitch);
        for(int y=c.y;y<c.y+c.h;++y)for(int x=c.x;x<c.x+c.w;++x) {
            const auto i=srf::backdrop_index(bg,0,view,x,y);
            const auto color=[&](unsigned mode){return i==1?bg.gradients[mode][view.gradient_y+y]:bg.palettes[mode][i];};
            const auto sky=srf::blend_environment_rgb555(color(0),color(1),c.weather);
            append(pixels,srf::blend_environment_rgb555(sky,0x7fff,c.fog),2);
        }
        require(srf::sha256(pixels)==c.hash,"White Land skyline differs from original displayed race");
    }
    const auto bytes=srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course));
    const auto sprites=srf::decode_world_sprites(bytes);
    require(srf::encode_world_sprites(sprites)==bytes,"White Land indexed sprite package did not round trip");
    struct Sprite {unsigned table,phase,address;const char* hash;};
    constexpr Sprite references[]{
        {0x996d,0,0x12c080,"8be31fb712a95a8e6ef9a94d58814962f52bdec22229cd509b6df3be09df34d6"},
        {0x996b,0,0x139060,"21f0d997fcd62ae25e1822c6d9cbda6602b661e82407cf85fc3ed97c673d0d40"},
        {0x8f91,0xd5,0x13e020,"91ee97060f19181881e7c67092e8e39b45d74a2953ce238585ec0974f934e0cf"}};
    for(const auto& ref:references) {
        const auto found=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s) {
            return s.material_table==ref.table && s.phase==ref.phase;
        });
        require(found!=sprites.end() && found->source_address==ref.address && srf::sha256(found->palette_indices)==ref.hash,
                "Ice or grandstand texture differs from original ROM");
        for(std::size_t i=0;i<found->pixels.size();++i)
            require((found->pixels[i]>>24!=0)==((found->palette_indices[i]&15)!=0),"Original White Land texture cutout differs");
    }
}

}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);
        std::cout<<"White Land original music, effects, fog, skyline and indexed sprites passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
