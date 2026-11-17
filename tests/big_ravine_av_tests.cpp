#include "srf/audio.hpp"
#include "srf/backdrop.hpp"
#include "srf/environment.hpp"
#include "srf/race_audio.hpp"
#include "srf/rain.hpp"
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
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::big_ravine));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"Big Ravine sound package did not round trip");
    require(srf::sha256(bank.music)=="0993cfdf06ef4d51400260256f4338f5f2d67c695dae91cd04b16bada8ad9286",
            "Big Ravine music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==27 && srf::sha256(samples)=="b3d8a399595b3c1c8647ac9c2f8e003e704c74dd7397d87b211844787977385d",
            "Big Ravine sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="aa6d2d635619f9c68259b5d41d100d4d34b0c95243457c005faf9e50e4312b69",
            "Big Ravine retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{247,335,450,555,674,781,885,1000,1105,1231,1325,1436,1547};
    constexpr std::array<srf::MusicArticulation,8> initial{{
        {48,252,178},{12,153,242},{12,252,242},{48,252,178},{48,252,178},{4,252,178},{},{}}};
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
    require(srf::sha256(states)=="7674624e0afca053d45a83ca25e10fed0034a2fd6bd000f54f52221e9fad52a7",
            "Big Ravine sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "45b8c764e62e893362ade68ab9a28ea699ace77dc9c2acdb82646f18ce70f094","Big Ravine music rendering changed");
    renderer.reset();
    renderer.set_music_enabled(false);
    auto clip=std::span(pcm).first(64000);
    renderer.sound_command(0x11);
    renderer.render(clip);
    require(renderer.echo_state().amount==30 && renderer.echo_state().stage==2,"Big Ravine half echo failed to settle");
    renderer.sound_command(0x10);
    renderer.render(clip);
    require(renderer.echo_state().stage==0,"Big Ravine echo failed to clear");
    renderer.sound_command(0x13);
    renderer.render(clip);
    const auto playing=renderer.effect_states();
    require(std::any_of(playing.begin(),playing.end(),[](const auto& ch){return ch.id==0x21;}) &&
            std::any_of(clip.begin(),clip.end(),[](auto s){return s!=0;}),"Big Ravine bridge ambience is silent");
    renderer.sound_command(0x0e);
    renderer.render(clip);
    const auto stopped=renderer.effect_states();
    require(std::none_of(stopped.begin(),stopped.end(),[](const auto& ch){return ch.id==0x21;}),
            "Big Ravine bridge ambience failed to clear");
    for(const auto effect:{0x36,0x53,0x5a,0x41,0x1b,0x25}) {
        srf::MusicRenderer effect_renderer(bank);
        effect_renderer.set_music_enabled(false);
        effect_renderer.play_effect(static_cast<std::uint8_t>(effect-5));
        effect_renderer.render(clip);
        require(std::any_of(clip.begin(),clip.end(),[](auto sample){return sample!=0;}),
                "Big Ravine scene effect is silent");
    }
}

void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::big_ravine;
    const auto bg=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(bg.indices[0])=="96013534b6360f35b42abb056aebbcb6deeb4fb35a621a5c6653304d4f738783" &&
            srf::sha256(bg.indices[1])=="9490e94ac68d894058ca607f1af6c8b41d02e6129dbce93b9b7efdcd65338a54",
            "Big Ravine original panorama tile or map changed");
    struct View {short yaw,pitch;unsigned layer,weight;int x,y,w,h;const char* hash;};
    constexpr View captures[]{
        {16384,-3184,0,0,2,2,80,10,"d5b5553457c62582806a3a798de44cbfc8e92d7bac361e3bc919fa77ad2bac32"},
        {0,-2656,0,0,128,3,10,4,"774d743129a0bcddf87e3a13de21aa53e38aa68bb3c8388fa97b07ec4190a028"},
        {-16384,-2931,1,0,2,2,20,10,"fd9b281fea71ffafde55e33d3cdc71c532a9bb040a4635e94757a1128c3cb7a2"},
        {16384,-2851,0,0,26,2,20,10,"88007f9655b582b55de2c3716155eaef3cd55e36672684e4c996bfd2f0f77ab5"},
        {20256,-3101,0,0,62,2,10,10,"1727e6adda7b7d37f9d0b94c8ae4eb92fe14542b93fa984af59edd4e5e3c37e4"},
        {16864,-2934,0,0,2,2,20,10,"e37f5f9f63a5509e11e6fa06bf77578992cbdffbf65a0c61da84beeec7ef4863"},
        {16736,-2931,0,4,52,6,10,7,"f97a5965d5cc4d04e929f40e6d1aaf9e9ddb5f0ab747d46d4f06581d3722cbf2"},
        {16624,-2931,0,7,2,2,20,10,"f853b46edb2fec352ecb2fbd11f25e2b7e34384e3501b5e3bee868751df9bde5"},
        {16624,-2931,0,11,88,2,10,7,"19e27dce2e2fc1eedd85b7de18d3484980fe95380f1fba128348f25ac22a02f3"},
        {4736,-3078,0,31,0,26,10,4,"c95fabb824744d79f75d0cc56258032e3d72506448e877d389d3f0de9094b46f"},
        {-18080,-3049,1,31,146,2,20,10,"79844a6fe80179588298f13848c1a546021b3213496d27d31085061df0b0b34c"}};
    for(const auto& c:captures) {
        std::vector<std::uint8_t> pixels;
        const auto view=srf::backdrop_view(c.yaw,c.pitch);
        for(int y=c.y;y<c.y+c.h;++y)for(int x=c.x;x<c.x+c.w;++x) {
            const auto i=srf::backdrop_index(bg,c.layer,view,x,y);
            const auto color=[&](int k) { return i==1?bg.gradients[k][view.gradient_y+y]:bg.palettes[k][i]; };
            append(pixels,srf::blend_environment_rgb555(color(0),color(1),c.weight),2);
        }
        require(srf::sha256(pixels)==c.hash,"Big Ravine skyline differs from original displayed race");
    }
    const auto world=srf::extract_world(rom,srf::extract_course_scene(rom,course));
    const auto count=[&](unsigned descriptor,unsigned phase) {
        return std::count_if(world.instances.begin(),world.instances.end(),[&](const auto& i) {
            return i.visual_descriptor==descriptor && i.sign_phase==phase;
        });
    };
    require(count(0xb5b0,2)==6 && count(0xb594,2)==2 && count(0xb578,2)==1 && count(0xb5b0,6)==2,
            "Big Ravine sign selector differs from original callbacks");
    const auto bytes=srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course));
    const auto sprites=srf::decode_world_sprites(bytes);
    require(srf::encode_world_sprites(sprites)==bytes,"Big Ravine indexed sprite package did not round trip");
    struct Sprite {unsigned table,phase,address;const char* hash;};
    constexpr Sprite references[]{
        {0x9ba3,2,0x12c0e0,"a35ed73dfedfa93da70b24ea2a847bffd553f2d01cd92a24db6fae9822839c1a"},
        {0x9ba3,6,0x13a0c0,"bbd3acc9e10ce4ab88b46c284f941246b38b1b6ea785023c14861a75d59b4933"},
        {0x8f91,0xd5,0x13e020,"91ee97060f19181881e7c67092e8e39b45d74a2953ce238585ec0974f934e0cf"}};
    for(const auto& ref:references) {
        const auto found=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s) {
            return s.material_table==ref.table && s.phase==ref.phase;
        });
        require(found!=sprites.end() && found->source_address==ref.address && srf::sha256(found->palette_indices)==ref.hash,
                "Sign or grandstand texture differs from original ROM");
        for(std::size_t i=0;i<found->pixels.size();++i)
            require((found->pixels[i]>>24!=0)==((found->palette_indices[i]&15)!=0),"Original Big Ravine texture cutout differs");
    }
}

void check_rain(const srf::Rom& rom) {
    // Original consecutive camera headings and vehicle speeds from the first
    // 55 wet updates. The GSU seed was 0x62F6 on entering the rain zone.
    struct Input { std::int16_t yaw, speed; };
    constexpr Input inputs[]{
        {-2512,234},{-3152,246},{-3728,253},{-4288,256},{-4800,258},{-5456,261},{-6048,263},
        {-6576,267},{-7232,271},{-7824,276},{-8544,282},{-9136,288},{-9808,295},{-10416,302},
        {-11104,317},{-11680,328},{-12128,340},{-12336,317},{-12576,282},{-12944,247},{-13232,225},
        {-13792,241},{-14336,252},{-14848,258},{-15344,265},{-15856,271},{-16464,277},{-17072,282},
        {-17712,251},{-18304,225},{-18976,238},{-19616,251},{-20160,255},{-20672,259},{-21344,263},
        {-21904,266},{-22608,269},{-23184,274},{-23872,280},{-24480,259},{-25184,241},{-25776,255},
        {-26336,262},{-27008,270},{-27616,277},{-28272,285},{-28864,293},{-29584,300},{-30176,311},
        {-30848,327},{-31440,365},{-31888,373},{-32208,380},{-32528,388},{-32768,395}
    };
    srf::RainParticles rain(rom);
    rain.reset(0x62f6);
    std::vector<std::uint8_t> trace;
    for(const auto& input:inputs) {
        rain.step(true,input.yaw,input.speed);
        append(trace,static_cast<unsigned>(rain.pixels().size()),2);
        for(const auto pixel:rain.pixels())trace.insert(trace.end(),{pixel.x,pixel.y});
    }
    require(srf::sha256(trace)=="a00d7e8a1e5288a5e7ac00e16a8570000d6ee491888fe894e76bb532e170e97c",
            "Big Ravine rain differs from original GSU pixel positions");
    rain.step(false,inputs[0].yaw,inputs[0].speed);
    require(rain.pixels().empty(),"Big Ravine rain persists in a dry section");
    rain.step(true,inputs[0].yaw,inputs[0].speed);
    require(rain.pixels().empty(),"Big Ravine rain did not initialize on re-entry");
}

}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);check_rain(rom);
        std::cout<<"Big Ravine original music, effects, rain, skyline and indexed sprites passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
