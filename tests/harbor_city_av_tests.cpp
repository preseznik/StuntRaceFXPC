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
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::harbor_city));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"Harbor City sound package did not round trip");
    require(srf::sha256(bank.music)=="59633f75c8dcf9a4e9d26a36577c7ede035a6b1f8d1caee3fe47a8c3701ecbd1",
            "Harbor City music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==27 && srf::sha256(samples)=="b3d8a399595b3c1c8647ac9c2f8e003e704c74dd7397d87b211844787977385d",
            "Harbor City sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="aa6d2d635619f9c68259b5d41d100d4d34b0c95243457c005faf9e50e4312b69",
            "Harbor City retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{227,335,447,555,668,775,888,998,1106,1215,1326,1435};
    constexpr std::array<srf::MusicArticulation,8> initial{{
        {96,153,242},{24,153,229},{12,252,204},{96,252,252},{96,252,191},{12,252,153},{},{}}};
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
    require(srf::sha256(states)=="42181cfa97b4a1759f6ede00756c439f0217d97ce110e01d7143759aca1f8d4e",
            "Harbor City sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "0df0cceb2d2b104a63b4d75703b601a113840049c4733aa87cb4ae673f5590af","Harbor City music rendering changed");
    // First complete SPC tick after each CPU event. The repair command's
    // initial tick was captured at the grid, before its first pitch slide.
    struct Effect {std::uint8_t port,channel,sample,left,right;std::uint16_t cursor,pitch;};
    constexpr Effect effects[]{{0x23,5,36,127,127,49145,1202},{0x54,4,6,48,48,50425,3030},
                               {0x59,4,15,53,53,50343,1011},{0x41,4,3,0,0,49433,0},
                               {0x1b,4,2,43,20,48954,802},{0x25,4,3,90,90,48995,954}};
    for(const auto ref:effects) {
        srf::MusicRenderer renderer(bank);
        srf::RaceAudioOutput hit;hit.engine_parameter=176;hit.effects.push_back(ref.port);
        renderer.update_race(hit,false,false,false,false);
        std::vector<std::int16_t> pcm(32000);
        renderer.render(std::span(pcm).first(1024));
        const auto voice=renderer.effect_states()[ref.channel];
        require(voice.id==ref.port-5 && voice.cursor==ref.cursor && voice.pitch==ref.pitch &&
                voice.sample==ref.sample && voice.left==ref.left && voice.right==ref.right,
                "Harbor City sound effect differs from original SPC voice");
        renderer.render(std::span(pcm).subspan(1024));
        require(std::any_of(pcm.begin(),pcm.end(),[](auto sample){return sample!=0;}),
                "Harbor City scene sound is silent");
    }
}

void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::harbor_city;
    const auto bg=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(bg.indices[0])=="416f6f29f9cf1920bc5de02905aff6beaba623960cda2bd327e4ff0b11275cfd",
            "Harbor City panorama differs from original map and glyphs");
    require(bg.indices[1]==bg.indices[0],"Harbor City secondary map should reuse its sole panorama");
    const auto hash_gradient=[](const auto& colors) {
        std::vector<std::uint8_t> data;
        for(auto c:colors)append(data,c,2);
        return srf::sha256(data);
    };
    require(hash_gradient(bg.gradients[0])=="67e1b94c9192f9e64af51b230b7895011ea89f09cc7ac74d81c1f3292dc44680" &&
            hash_gradient(bg.gradients[1])=="97d2e41920dee9f9b8d60bd02019f9f1bb066db83cd7f4c082d6ee6c57b20384",
            "Harbor City source-specific primary or alternate gradient differs");
    struct View {short yaw,pitch; unsigned weight,phase,latch; int x,y,w,h; const char* hash;};
    constexpr View captures[]{
        {0,-3184,0,18,20,2,2,80,10,"484c07d3124e67b9cfc66b3b7792b39b4efe083fa899037f650d9e4c6bcce0b7"},
        {-112,-2940,0,11,20,34,24,10,7,"d88680ec94d4ba846dc5053c67b91aef8f3882a0fcebba5c45bfcf01b3238c96"},
        {-16112,-2972,0,15,20,0,24,48,10,"f7b038ad2fb877274639b121c6e081c75760324ff6ce5387f7a44f407029295a"},
        {15856,-2923,0,7,20,2,2,140,10,"e4bc224056b28271201e208afcf2c463ba5231bd712634a4aa8fc1d9439cb070"},
        {13232,-2958,4,10,8,34,23,20,10,"505e894d7f2f5ead17f8532c99fbf2cf9aab36e27c085a46ad7077b5223ca2c8"},
        {-720,-3003,31,2,4,40,2,10,2,"d5199e7420a48b2b689c793d054ea744e5ad4a1ca27583e7072613dc6af54937"},
    };
    for(const auto& c:captures) {
        std::vector<std::uint8_t> pixels;
        const auto view=srf::backdrop_view(c.yaw,c.pitch);
        for(int y=c.y;y<c.y+c.h;++y)for(int x=c.x;x<c.x+c.w;++x) {
            const auto i=srf::backdrop_index(bg,0,view,x,y);
            const auto color=i==1?srf::blend_environment_rgb555(bg.gradients[0][view.gradient_y+y],
                bg.gradients[1][view.gradient_y+y],c.weight):srf::backdrop_color(bg,i,c.weight,c.phase,c.latch);
            append(pixels,color,2);
        }
        require(srf::sha256(pixels)==c.hash,"Harbor City skyline differs from the original displayed race");
    }
    // Original 7E:FD83 after the palette job and subsequent animation writes.
    require(srf::backdrop_color(bg,15,0,19,17)==0x0133 &&
            srf::backdrop_color(bg,15,11,4,20)==0x09ff &&
            srf::backdrop_color(bg,15,12,9,6)==0x0dab &&
            srf::backdrop_color(bg,15,24,16,14)==0x19ef &&
            srf::backdrop_color(bg,15,31,21,4)==0x1e27,
            "Harbor City lighthouse flash or latched upper palette byte differs");
    const auto world=srf::extract_world(rom,srf::extract_course_scene(rom,course));
    const auto count=[&](unsigned phase) {
        return std::count_if(world.instances.begin(),world.instances.end(),[&](const auto& i) {
            return i.visual_descriptor==0xb5b0 && i.sign_phase==phase;
        });
    };
    require(count(2)==2 && count(3)==6,"Harbor City sign selectors differ from ROM");
    const auto bytes=srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course));
    const auto sprites=srf::decode_world_sprites(bytes);
    require(srf::encode_world_sprites(sprites)==bytes,"Harbor City indexed sprite package did not round trip");
    struct Sprite {unsigned table,phase,address;const char* hash;};
    constexpr Sprite references[]{
        {0x9ba3,2,0x12c0e0,"a35ed73dfedfa93da70b24ea2a847bffd553f2d01cd92a24db6fae9822839c1a"},
        {0x9ba3,3,0x12c0e0,"a35ed73dfedfa93da70b24ea2a847bffd553f2d01cd92a24db6fae9822839c1a"},
        {0x9b63,0,0x13a000,"6da4ea6eefd4d1c6215788e2f0cfd32f558c539963c2d2e6ffefe72f50c81ad7"},
        {0x9b63,1,0x13a020,"af4e3aab41a8f53bc9598a2840a79ccbd41415eea06afcebc2efb860e0eb16ef"},
        {0x9b63,2,0x13a040,"201a0ead906889afb618f9dcbf587ec4dd89f5d6241078d8851c64154771d064"},
        {0x9b63,3,0x13a040,"201a0ead906889afb618f9dcbf587ec4dd89f5d6241078d8851c64154771d064"},
        {0x9b79,0,0x13a060,"057573174abb6a5a121632dc3f09e40c143367fd902fd5b1631b62ca60cc4679"},
        {0x9b79,1,0x13a060,"057573174abb6a5a121632dc3f09e40c143367fd902fd5b1631b62ca60cc4679"},
        {0x9b79,2,0x13a060,"057573174abb6a5a121632dc3f09e40c143367fd902fd5b1631b62ca60cc4679"},
        {0x9b79,3,0x13a060,"057573174abb6a5a121632dc3f09e40c143367fd902fd5b1631b62ca60cc4679"},
        {0x8f91,0xd5,0x13e020,"91ee97060f19181881e7c67092e8e39b45d74a2953ce238585ec0974f934e0cf"}};
    for(const auto& ref:references) {
        const auto found=std::find_if(sprites.begin(),sprites.end(),[&](const auto& s) {
            return s.material_table==ref.table && s.phase==ref.phase;
        });
        require(found!=sprites.end() && found->source_address==ref.address && srf::sha256(found->palette_indices)==ref.hash,
                "Wet spray, sign or grandstand texture differs from original ROM");
        for(std::size_t i=0;i<found->pixels.size();++i)
            require((found->pixels[i]>>24!=0)==((found->palette_indices[i]&15)!=0),"Original Harbor City texture cutout differs");
    }
}

}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);
        std::cout<<"Harbor City original music, effects, skyline, animated lights and original spray/sign sprites passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
