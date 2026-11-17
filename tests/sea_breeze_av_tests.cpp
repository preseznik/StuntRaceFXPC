#include "srf/audio.hpp"
#include "srf/backdrop.hpp"
#include "srf/race_audio.hpp"
#include "srf/world.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool pass,const char* message) {
    if(!pass)throw std::runtime_error(message);
}
void append(std::vector<std::uint8_t>& out,unsigned value,unsigned bytes) {
    for(unsigned i=0;i<bytes;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));
}
void check_audio(const srf::Rom& rom) {
    const auto bank=srf::decode_audio(srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::sea_breeze)));
    require(srf::sha256(bank.music)=="71ed4c9b3ef9a35954ed7f4a12e2dfbcbefa3ba7bb8ae665897a6b6fd133b120",
            "Sea Breeze original SPC music differs");
    require(srf::sha256(bank.effect_noise_seed)=="49b6b12bf9fef78bbba1f98fb2e489dae0b3f8119f38520b77aa73134adf4a43",
            "Expert round two inherited the wrong sound-effect noise seed");
    std::vector<std::uint8_t> instruments,samples;
    for(const auto& i:bank.instruments)
        instruments.insert(instruments.end(),{i.sample,i.adsr1,i.adsr2,i.gain,
                static_cast<std::uint8_t>(i.pitch_multiplier>>8),static_cast<std::uint8_t>(i.pitch_multiplier)});
    require(srf::sha256(instruments)=="0fb2e66a22062722cece1742d0badf50cf4ff796f4fd45f52189d34d7dbfb1c7",
            "Sea Breeze original SPC instrument table differs");
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==29 && srf::sha256(samples)=="6b68524168114b356d958df5721aaf8c9aaf99f7ec0c778028d86b8635fbc536",
            "Sea Breeze original BRR samples differ");
    // Every active voice has supplied its own articulation by tick 225.
    constexpr std::array ticks{225,335,445,555,668,775};
    srf::MusicSequence sequence(bank);
    std::vector<std::uint8_t> states;
    for(int i=0;i<=ticks.back();++i) {
        sequence.tick();
        if(!std::binary_search(ticks.begin(),ticks.end(),i))continue;
        for(const auto& voice:sequence.channels()) {
            append(states,voice.cursor,2);
            states.insert(states.end(),{voice.remaining,voice.duration,voice.gate,voice.volume,voice.repeat_count,voice.instrument});
        }
    }
    require(srf::sha256(states)=="dee7e95d089a22c4dc3639af0e4eaa69c731236f00af639d9e41853704fb5eca",
            "Sea Breeze native song sequence diverged from original");
    // First complete SPC tick after each CPU event, sampled while driving.
    struct Effect {std::uint8_t port,channel,sample,left,right;std::uint16_t cursor,pitch;};
    constexpr Effect effects[]{{0x23,5,36,127,127,49145,1202},{0x2e,6,37,4,32,49407,3211},{0x59,4,15,57,57,50343,1011},
                               {0x25,4,3,90,90,48995,954},{0x1b,4,2,43,20,48954,802}};
    for(const auto ref:effects) {
        srf::MusicRenderer renderer(bank);
        srf::RaceAudioOutput hit;hit.engine_parameter=196;hit.effects.push_back(ref.port);
        renderer.update_race(hit,false,false,false,false);
        std::vector<std::int16_t> pcm(32000);
        renderer.render(std::span(pcm).first(1024));
        const auto voice=renderer.effect_states()[ref.channel];
        require(voice.id==ref.port-5 && voice.cursor==ref.cursor && voice.pitch==ref.pitch &&
                voice.sample==ref.sample && voice.left==ref.left && voice.right==ref.right,
                "Sea Breeze sound effect differs from original SPC voice");
        renderer.render(std::span(pcm).subspan(1024));
        require(std::any_of(pcm.begin(),pcm.end(),[](auto sample){return sample!=0;}),
                "Sea Breeze scene sound is silent");
    }
}

void check_art(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::sea_breeze;
    const auto backdrop=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(backdrop.indices[0])=="8441a8d07884b6464d21ca1dcc0563c7c26af700a1f0d7df3c481bbdac04b593",
            "Sea Breeze source panorama differs from original VRAM and WRAM");
    require(backdrop.indices[0]==backdrop.indices[1] && backdrop.palettes[0]==backdrop.palettes[1] &&
            backdrop.gradients[0]==backdrop.gradients[1],"Sea Breeze has one original panorama");
    struct View {short yaw,pitch;int x,width;const char* hash;};
    constexpr View views[]{
        {0,-3184,52,80,"a46ab6b4c38007cd5190ddc428a0ef25becaf4957869955061655f608aebd4e0"},
        {-1248,-3071,18,24,"273259a90237560200feadde42b46dddc80e9850b16b3569d16954ec61f05915"},
        {-16496,-3091,116,24,"e163670f8dab2590e9cb42e11e231d2e5cde1e88119bc47ae8abfc35e15e3532"}};
    for(auto capture:views) {
        const auto view=srf::backdrop_view(capture.yaw,capture.pitch);
        std::vector<std::uint8_t> pixels;
        for(int y=1;y<=12;++y)for(int x=capture.x;x<capture.x+capture.width;++x) {
            const auto index=srf::backdrop_index(backdrop,0,view,x,y);
            append(pixels,index==1?backdrop.gradients[0][view.gradient_y+y]:backdrop.palettes[0][index],2);
        }
        require(srf::sha256(pixels)==capture.hash,"Sea Breeze skyline differs from original driving view");
    }
    const auto sprites=srf::decode_world_sprites(srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course)));
    struct Sprite {unsigned table,phase,address;const char* hash;};
    auto check_sprite=[&](const Sprite& ref) {
        const auto s=std::find_if(sprites.begin(),sprites.end(),[&](const auto& sprite) {
            return sprite.material_table==ref.table && sprite.phase==ref.phase;
        });
        require(s!=sprites.end() && s->source_address==ref.address &&
                srf::sha256({reinterpret_cast<const std::uint8_t*>(s->pixels.data()),s->pixels.size()*4})==ref.hash,
                "Sea Breeze texture differs from original");
    };
    check_sprite({0x9ba3,2,0x12c0e0,"7633fb0be7c63fc5093aa361d68b8da0c1f374c0d7f8f1a36211bfa252e0385a"});
    check_sprite({0x8f91,0xd5,0x13e020,"db5dd93d2ad8e954ddcdd82d53341285f6b06d77fd54fa378d95546568478493"});
    constexpr std::array spray_hash{"d1c7d5af7414fd0bb6962e5d7564cf9ca648f9110611823d3964b45794600b12",
        "cd34705f30ca540a71c1000a73a167b9ea8ab6aebcedcc90b8523d1c29f21012",
        "ee377f4472ed184c99c8711dee295d7992cba2d8ff8b4b1082b81e20660b426b"};
    for(unsigned phase=0;phase<4;++phase) {
        const auto image=std::min(phase,2u);
        check_sprite({0x9b63,phase,0x13a000+image*32,spray_hash[image]});
        check_sprite({0x9b79,phase,0x13a060,"6bb5562563a59ef2406b6f4c305abca30399f7e4fe237c5d059809e016540097"});
    }
    const auto world=srf::extract_world(rom,srf::extract_course_scene(rom,course));
    require(std::count_if(world.instances.begin(),world.instances.end(),[](const auto& i) {
            return i.visual_descriptor==0xb5b0 && i.sign_phase==2;})==4,"Sea Breeze sign placements differ");
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_art(rom);
        std::cout<<"Sea Breeze original music, effects, skyline and placed textures passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
