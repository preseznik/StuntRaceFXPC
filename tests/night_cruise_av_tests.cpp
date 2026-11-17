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
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::night_cruise));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"Night Cruise sound package did not round trip");
    require(srf::sha256(bank.music)=="59633f75c8dcf9a4e9d26a36577c7ede035a6b1f8d1caee3fe47a8c3701ecbd1",
            "Night Cruise music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==27 && srf::sha256(samples)=="b3d8a399595b3c1c8647ac9c2f8e003e704c74dd7397d87b211844787977385d",
            "Night Cruise sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="49b6b12bf9fef78bbba1f98fb2e489dae0b3f8119f38520b77aa73134adf4a43",
            "Night Cruise retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{227,336,447,555,665,775,887,995,1105};
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
    require(srf::sha256(states)=="32b3d8c5354c23bbe0f3f2d994a709ed980bae4e941e2b377f2cac83698fcf22",
            "Night Cruise sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "0df0cceb2d2b104a63b4d75703b601a113840049c4733aa87cb4ae673f5590af","Night Cruise music rendering changed");
    // First complete SPC tick after each CPU event, sampled while driving.
    struct Effect {std::uint8_t port,channel,sample,left,right;std::uint16_t cursor,pitch;};
    constexpr Effect effects[]{{0x0e,6,35,18,18,48418,2700},{0x36,4,10,100,100,49019,802},
                               {0x54,4,6,48,48,50425,3030},{0x55,4,15,70,70,50194,675},
                               {0x1b,4,2,43,20,48954,802},{0x25,4,3,90,90,48995,954}};
    for(const auto ref:effects) {
        srf::MusicRenderer renderer(bank);
        srf::RaceAudioOutput hit;hit.engine_parameter=196;hit.effects.push_back(ref.port);
        renderer.update_race(hit,false,false,false,false);
        std::vector<std::int16_t> pcm(32000);
        renderer.render(std::span(pcm).first(1024));
        const auto voice=renderer.effect_states()[ref.channel];
        require(voice.id==ref.port-5 && voice.cursor==ref.cursor && voice.pitch==ref.pitch &&
                voice.sample==ref.sample && voice.left==ref.left && voice.right==ref.right,
                "Night Cruise sound effect differs from original SPC voice");
        renderer.render(std::span(pcm).subspan(1024));
        require(std::any_of(pcm.begin(),pcm.end(),[](auto sample){return sample!=0;}),
                "Night Cruise scene sound is silent");
    }
}

void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::night_cruise;
    const auto bg=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(bg.indices[0])=="ee704356edf2bc0ad3a1659efd7788e5cdb0632702dcce9ca43803cbcfb01ffc",
            "Night Cruise panorama differs from original map and glyphs");
    std::vector<std::uint8_t> cycle;
    for(unsigned phase=0;phase<22;++phase)
        for(unsigned index=12;index<16;++index)
            append(cycle,srf::backdrop_palette_target(bg,index,phase),2);
    require(srf::sha256(cycle)=="eecd8be16e49d75173346e55892366acd72022d2c4ba8ae538a65819e448e16c",
            "Night Cruise city lights differ from the original four palette tables");
    struct View {short yaw,pitch;unsigned weight,phase;int x,y,w,h;const char* hash;};
    constexpr View captures[]{
        {0,-3183,0,9,52,2,80,10,"97e0b7082c5a3f4c1bdb306a09a3b26ab5dc6a0d93d7598474ff839fca38934e"},
        {-16384,-2985,0,3,2,2,140,10,"945e5a3ffbe4d4fd07167a06845299e2a8b161cfce163b83b216857927c08c1e"},
        {-1008,-2927,12,21,14,2,140,10,"da847c4383f81e38ce5e8ad93c4394c9602740333f167e1bd5351cf1a4d9d8b7"},
        {-31760,-3011,18,8,0,11,48,10,"3365248327c782c8fd700ce19ae205a247a8ff475723a507dd274f3689427242"},
        {-32048,-2983,31,13,0,12,48,10,"915e4436bffb2567f904d4f22d15e1c7bd54dce7cbba8a50c8e6be857977a464"},
        {15376,-2966,31,1,114,12,20,10,"376370706fa7db892cb3e922682606cefa31929a0fcb5fa3328bf4571e669ec1"}};
    for(const auto& c:captures) {
        std::vector<std::uint8_t> pixels;
        const auto view=srf::backdrop_view(c.yaw,c.pitch);
        for(int y=c.y;y<c.y+c.h;++y)for(int x=c.x;x<c.x+c.w;++x) {
            const auto i=srf::backdrop_index(bg,0,view,x,y);
            const auto from=i==1?bg.gradients[0][view.gradient_y+y]:bg.palettes[0][i];
            const auto to=i==1?bg.gradients[1][view.gradient_y+y]:srf::backdrop_palette_target(bg,i,c.phase);
            append(pixels,srf::blend_environment_rgb555(from,to,c.weight),2);
        }
        require(srf::sha256(pixels)==c.hash,"Night Cruise skyline differs from original displayed race");
    }
    const auto bytes=srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course));
    const auto sprites=srf::decode_world_sprites(bytes);
    require(srf::encode_world_sprites(sprites)==bytes,"Night Cruise indexed sprite package did not round trip");
    struct Sprite {unsigned table,phase,address;const char* hash;};
    constexpr Sprite references[]{
        {0x9973,0,0x12c000,"d09ce583d37745f71f3f1b44ff89fbfa78f83ecdf34e3dc618cf748a3883515e"},
        {0x8f91,0xd5,0x13e020,"91ee97060f19181881e7c67092e8e39b45d74a2953ce238585ec0974f934e0cf"}};
    for(const auto& ref:references) {
        const auto found=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s) {
            return s.material_table==ref.table && s.phase==ref.phase;
        });
        require(found!=sprites.end() && found->source_address==ref.address && srf::sha256(found->palette_indices)==ref.hash,
                "Tree or grandstand texture differs from original ROM");
        for(std::size_t i=0;i<found->pixels.size();++i)
            require((found->pixels[i]>>24!=0)==((found->palette_indices[i]&15)!=0),"Original Night Cruise texture cutout differs");
    }
}

}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);
        std::cout<<"Night Cruise original music, effects, city lights, skyline and indexed sprites passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
