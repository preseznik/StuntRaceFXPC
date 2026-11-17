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
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::sky_ramp));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"Sky Ramp sound package did not round trip");
    require(srf::sha256(bank.music)=="d112b7de2aec38808e863a33b4eb5178538276b2f280a106805ec7229e059bd2",
            "Sky Ramp music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==30 && srf::sha256(samples)=="0134884182ae9828b549ce968e1085a8c7e5615163d53ad5c33960f0d5375b17",
            "Sky Ramp sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="aa6d2d635619f9c68259b5d41d100d4d34b0c95243457c005faf9e50e4312b69",
            "Sky Ramp retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{225,339,446,556,665,777,886,996,1105,1215};
    constexpr std::array<srf::MusicArticulation,8> initial{{
        {6,252,165},{24,204,229},{6,252,216},{12,178,153},{12,178,153},{24,252,165},{},{}}};
    srf::MusicSequence sequence(bank,initial);
    std::vector<std::uint8_t> states;
    for(int i=0;i<=ticks.back();++i) {
        sequence.tick();
        if(!std::binary_search(ticks.begin(),ticks.end(),i))continue;
        for(const auto& ch:sequence.channels()) {
            append(states,ch.cursor,2);
            states.insert(states.end(),{ch.remaining,ch.duration,ch.gate,ch.volume,ch.repeat_count,ch.instrument});
        }
    }
    require(srf::sha256(states)=="a99a45bd85815985a535a299fbce984860599af63adbcbbb4452102aeb462a5e",
            "Sky Ramp sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "c9a8120e860bb266f399a8a5d0421a070764dfc71b00fa188e52a85cd918f833","Sky Ramp music rendering changed");
    // First complete SPC tick after each CPU event, sampled while driving.
    struct Effect {std::uint8_t port,channel,sample,left,right;std::uint16_t cursor,pitch;};
    constexpr Effect effects[]{{0x0e,6,35,18,18,48418,2700},{0x59,4,15,69,69,50343,1011},
                               {0x41,4,3,0,0,49433,0},{0x1b,4,2,43,20,48954,802},{0x25,4,3,90,90,48995,954}};
    for(const auto ref:effects) {
        srf::MusicRenderer renderer(bank);
        srf::RaceAudioOutput hit;hit.engine_parameter=252;hit.effects.push_back(ref.port);
        renderer.update_race(hit,false,false,false,false);
        std::vector<std::int16_t> pcm(32000);
        renderer.render(std::span(pcm).first(1024));
        const auto voice=renderer.effect_states()[ref.channel];
        require(voice.id==ref.port-5 && voice.cursor==ref.cursor && voice.pitch==ref.pitch &&
                voice.sample==ref.sample && voice.left==ref.left && voice.right==ref.right,
                "Sky Ramp sound effect differs from original SPC voice");
        renderer.render(std::span(pcm).subspan(1024));
        require(std::any_of(pcm.begin(),pcm.end(),[](auto sample){return sample!=0;}),
                "Sky Ramp scene sound is silent");
    }
}

void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::sky_ramp;
    const auto bg=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(bg.indices[0])=="4b28b20fdb1c3a5055701a71efdd90f4bdfa97a4b8b4658fc4b4596f46fd6a2c",
            "Sky Ramp panorama differs from original map and glyphs");
    require(bg.indices[1]==bg.indices[0],"Sky Ramp secondary map should reuse its sole panorama");
    const auto hash_gradient=[](const auto& colors) {
        std::vector<std::uint8_t> data;
        for(auto c:colors)append(data,c,2);
        return srf::sha256(data);
    };
    require(hash_gradient(bg.gradients[0])=="a9d8cff9781f751cd0341e3898d0eaa65100d1c88c90d2654431f14760881308" &&
            hash_gradient(bg.gradients[1])=="ed46bc52dcf995203e48a9913f32ab8111fb83c3f0dd7df0808418c70308815b",
            "Sky Ramp source-specific primary or alternate gradient differs");
    struct View {short yaw,pitch;unsigned fog;int x,y,w,h;const char* hash;};
    constexpr View captures[]{
        {0,-3184,0,2,2,140,10,"86c77d8cb3c555c54edd8e317b068d39ee4843afe55377d1c7cb93f48b976742"},
        {16576,-5704,5,154,27,10,4,"e7170c3ad44e7b97d63b9a1a092d02fc499e46f7228051c8d11425744e9ae739"},
        {16576,-5026,13,126,38,48,4,"7e076124f14ec2a1b8db4c69ccf9caf22c917e1d22eb420a8a973233928b44c4"},
        {-31360,-3054,0,74,2,80,10,"79389c4653cd3c3b4553d6d840f62bab9713f6d7b008f6c58ba4b5350189fc0e"},
        {0,-2913,0,2,2,140,10,"02bd507a85aa656da2bddff1f1a9dd2c3199b04a09b6fffe9f9234b7312898d0"},
        {-16384,-6005,6,20,22,10,2,"b4bab0f26f6b8593cb45c8379963ee06593101e91fbb30fb802f98fa7f4638d3"}};
    for(const auto& c:captures) {
        std::vector<std::uint8_t> pixels;
        const auto view=srf::backdrop_view(c.yaw,c.pitch);
        for(int y=c.y;y<c.y+c.h;++y)for(int x=c.x;x<c.x+c.w;++x) {
            const auto i=srf::backdrop_index(bg,0,view,x,y);
            const auto color=i==1?bg.gradients[0][view.gradient_y+y]:bg.palettes[0][i];
            append(pixels,srf::blend_environment_rgb555(color,0x7fff,c.fog),2);
        }
        require(srf::sha256(pixels)==c.hash,"Sky Ramp skyline differs from original displayed race");
    }
    const auto world=srf::extract_world(rom,srf::extract_course_scene(rom,course));
    const auto count=[&](unsigned phase) {
        return std::count_if(world.instances.begin(),world.instances.end(),[&](const auto& i) {
            return i.visual_descriptor==0xb5b0 && i.sign_phase==phase;
        });
    };
    require(count(2)==6 && count(3)==10,"Sky Ramp sign selectors differ from ROM");
    const auto bytes=srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course));
    const auto sprites=srf::decode_world_sprites(bytes);
    require(srf::encode_world_sprites(sprites)==bytes,"Sky Ramp indexed sprite package did not round trip");
    struct Sprite {unsigned table,phase,address;const char* hash;};
    constexpr Sprite references[]{
        {0x9ba3,2,0x12c0e0,"a35ed73dfedfa93da70b24ea2a847bffd553f2d01cd92a24db6fae9822839c1a"},
        {0x9ba3,3,0x12c0e0,"a35ed73dfedfa93da70b24ea2a847bffd553f2d01cd92a24db6fae9822839c1a"},
        {0x997c,0,0x128000,"daa278629ccad655a35769a3c1d64e934f5ca9cf203da72eefe2f92c74dbca2c"},
        {0x997c,1,0x128020,"b1cb8bbf6281609cc3537b8739851b3f75e790e977f84d0b0e478f53761f3da2"},
        {0x997c,2,0x128040,"ead25668f053f0b7a23a62331b3d40f4524d83305135761dc91afb01f54c25d3"},
        {0x997c,3,0x128060,"e8f47d96b476674708568e711d0d267572476c6bda0809beb2a98564d2aa77cf"},
        {0x997c,4,0x128080,"00bbac99fd09034eaf998b4bce46b4921c1405487d7b366a63ac64e6b9db4706"},
        {0x997c,5,0x1280a0,"7967a34512ac94789992cd84a312e49c11f0da14c552cc2435b2a87aafb2a95c"},
        {0x8f91,0xd5,0x13e020,"91ee97060f19181881e7c67092e8e39b45d74a2953ce238585ec0974f934e0cf"}};
    for(const auto& ref:references) {
        const auto found=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s) {
            return s.material_table==ref.table && s.phase==ref.phase;
        });
        require(found!=sprites.end() && found->source_address==ref.address && srf::sha256(found->palette_indices)==ref.hash,
                "Smoke, sign or grandstand texture differs from original ROM");
        for(std::size_t i=0;i<found->pixels.size();++i)
            require((found->pixels[i]>>24!=0)==((found->palette_indices[i]&15)!=0),"Original Sky Ramp texture cutout differs");
    }
}

}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);
        std::cout<<"Sky Ramp original music, effects, fog, skyline and indexed smoke/sign sprites passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
