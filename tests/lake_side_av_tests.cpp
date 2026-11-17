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
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::lake_side));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"Lake Side sound package did not round trip");
    require(srf::sha256(bank.music)=="11daac82dd2dbebcedf54492634a4d55d0f44485c2334943e9b6e294d7774e36",
            "Lake Side music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==29 && srf::sha256(samples)=="86a55821f4c91a2ae7106674ac50cc22cced3b660a793df273fea2103d7acb55",
            "Lake Side sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="aa6d2d635619f9c68259b5d41d100d4d34b0c95243457c005faf9e50e4312b69",
            "Lake Side retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{225,336,445,555,670,775,885,999,1105};
    constexpr std::array<srf::MusicArticulation,8> initial{{
        {12,127,178},{12,153,229},{12,252,242},{6,229,114},{23,252,191},{6,252,178},{},{}}};
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
    require(srf::sha256(states)=="ac4b34e336749343f75deb1f6d0950e8765c461c9ab4adeafd2c4e7bc9812e7c",
            "Lake Side sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "95b2902ef590a67666fb44f600bb20c59ed3075f28202f3e6cd349606935d5d0","Lake Side music rendering changed");
    renderer.reset();
    renderer.set_music_enabled(false);
    auto clip=std::span(pcm).first(64000);
    renderer.sound_command(0x11);
    renderer.render(clip);
    require(renderer.echo_state().amount==30 && renderer.echo_state().stage==2,"Lake Side half echo failed to settle");
    renderer.sound_command(0x10);
    renderer.render(clip);
    require(renderer.echo_state().stage==0,"Lake Side echo failed to clear");
    renderer.sound_command(0x13);
    renderer.render(clip);
    const auto playing=renderer.effect_states();
    require(std::any_of(playing.begin(),playing.end(),[](const auto& ch){return ch.id==0x21;}) &&
            std::any_of(clip.begin(),clip.end(),[](auto s){return s!=0;}),"Lake Side bridge ambience is silent");
    renderer.sound_command(0x0e);
    renderer.render(clip);
    const auto stopped=renderer.effect_states();
    require(std::none_of(stopped.begin(),stopped.end(),[](const auto& ch){return ch.id==0x21;}),
            "Lake Side bridge ambience failed to clear");
    // First complete SPC tick after each CPU event, sampled while driving.
    struct Effect {std::uint8_t port,channel,sample,left,right;std::uint16_t cursor,pitch;};
    constexpr Effect effects[]{{0x32,6,36,0,100,48651,1350},{0x54,4,6,48,48,50425,3030},{0x41,4,3,0,0,49433,0},
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
                "Lake Side sound effect differs from original SPC voice");
        renderer.render(std::span(pcm).subspan(1024));
        require(std::any_of(pcm.begin(),pcm.end(),[](auto sample){return sample!=0;}),
                "Lake Side scene sound is silent");
    }
}

void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::lake_side;
    const auto bg=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(bg.indices[0])=="4edf8241d44bb38386d65286d23007b1852b45391f193ddeea6279be8ff8eebc",
            "Lake Side panorama differs from original map and glyphs");
    struct View {short yaw,pitch;unsigned fog;int x,y,w,h;const char* hash;};
    constexpr View captures[]{
        {0,-3184,0,0,17,48,10,"d154aa58dc0868f3a358c2ebf27f17647cbbceeedbeb1c23597e31419aa23384"},
        {-192,-2908,6,2,2,48,10,"2516c2eeca79af55b138aa3392e4830af09755bfdef80c60bff5f8d20a8f82f9"},
        {0,-2896,0,0,19,80,10,"7f2aafeb7ae23b70d238f11e3db0ed136a779c53b1ec72f21df6ca1e2074f3e2"},
        {0,-2855,6,2,3,48,10,"3eccbef1bc7aaf2497810b8adb3f4d6036bd822dd3e203c76cff585b33fbf71c"},
        {0,-2872,4,0,5,48,10,"fb2e6c3cc363e5ad7253cd0403ceb045aeb5ffa324024fc062c80089248fc3cc"},
        {0,-2931,1,0,13,48,10,"9160b2b690c3083764f0d639678b80c948f3dc33a326e3aefcc62a3ce0ae4205"},
        {-32288,-3023,0,10,14,20,4,"794380b0d23cc079acdb987a900c983748a6f93f71302ffe44ba678342fcec06"},
        {-23632,-3075,0,190,37,10,10,"d417b5b31fdebbd37915734675df598146f132fdc9603724ee2135f45417335b"},
        {0,-2935,5,0,8,48,10,"a79d38feee27bfde8cab26afb3fe1e744beb9d17c520f68bdb17a66f3074fec2"}};
    for(const auto& c:captures) {
        std::vector<std::uint8_t> pixels;
        const auto view=srf::backdrop_view(c.yaw,c.pitch);
        for(int y=c.y;y<c.y+c.h;++y)for(int x=c.x;x<c.x+c.w;++x) {
            const auto i=srf::backdrop_index(bg,0,view,x,y);
            const auto color=i==1?bg.gradients[0][view.gradient_y+y]:bg.palettes[0][i];
            append(pixels,srf::blend_environment_rgb555(color,0x0800,c.fog),2);
        }
        require(srf::sha256(pixels)==c.hash,"Lake Side skyline differs from original displayed race");
    }
    const auto world=srf::extract_world(rom,srf::extract_course_scene(rom,course));
    const auto count=[&](unsigned descriptor,unsigned phase,unsigned yaw) {
        return std::count_if(world.instances.begin(),world.instances.end(),[&](const auto& i) {
            return i.visual_descriptor==descriptor && i.sign_phase==phase && (yaw==256 || i.yaw==yaw);
        });
    };
    require(count(0xb5b0,2,256)==4 && count(0xb5b0,3,256)==8,"Lake Side sign selector differs from ROM");
    require(count(0xafc8,0,224)==1 && count(0xb038,0,160)==1 && count(0xb038,0,32)==1,
            "Lake Side callback heading differs from ROM");
    const auto bytes=srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course));
    const auto sprites=srf::decode_world_sprites(bytes);
    require(srf::encode_world_sprites(sprites)==bytes,"Lake Side indexed sprite package did not round trip");
    struct Sprite {unsigned table,phase,address;const char* hash;};
    constexpr Sprite references[]{
        {0x9ba3,2,0x12c0e0,"a35ed73dfedfa93da70b24ea2a847bffd553f2d01cd92a24db6fae9822839c1a"},
        {0x9ba3,3,0x12c0e0,"a35ed73dfedfa93da70b24ea2a847bffd553f2d01cd92a24db6fae9822839c1a"},
        {0x8f91,0xd5,0x13e020,"91ee97060f19181881e7c67092e8e39b45d74a2953ce238585ec0974f934e0cf"}};
    for(const auto& ref:references) {
        const auto found=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s) {
            return s.material_table==ref.table && s.phase==ref.phase;
        });
        require(found!=sprites.end() && found->source_address==ref.address && srf::sha256(found->palette_indices)==ref.hash,
                "Sign or grandstand texture differs from original ROM");
        for(std::size_t i=0;i<found->pixels.size();++i)
            require((found->pixels[i]>>24!=0)==((found->palette_indices[i]&15)!=0),"Original Lake Side texture cutout differs");
    }
}

}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);
        std::cout<<"Lake Side original music, effects, fog, skyline and indexed sprites passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
