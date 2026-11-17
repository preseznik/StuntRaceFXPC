#include "srf/audio.hpp"
#include "srf/backdrop.hpp"
#include "srf/environment.hpp"
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
    const auto bytes=srf::encode_audio(srf::extract_course_audio(rom,srf::CourseId::night_owl));
    const auto bank=srf::decode_audio(bytes);
    require(srf::encode_audio(bank)==bytes,"Night Owl sound package did not round trip");
    require(srf::sha256(bank.music)=="59633f75c8dcf9a4e9d26a36577c7ede035a6b1f8d1caee3fe47a8c3701ecbd1",
            "Night Owl music differs from original SPC RAM");
    std::vector<std::uint8_t> samples;
    for(const auto& sample:bank.samples) {
        append(samples,sample.directory_index,1);append(samples,sample.original_start,2);
        append(samples,static_cast<unsigned>(sample.brr.size()),4);
        samples.insert(samples.end(),sample.brr.begin(),sample.brr.end());
    }
    require(bank.samples.size()==27 && srf::sha256(samples)=="b3d8a399595b3c1c8647ac9c2f8e003e704c74dd7397d87b211844787977385d",
            "Night Owl sample directory or BRR data differs from original");
    require(srf::sha256(bank.effect_noise_seed)=="aa6d2d635619f9c68259b5d41d100d4d34b0c95243457c005faf9e50e4312b69",
            "Night Owl retained sound data differs from original");
    // Original DSP dispatcher samples taken after complete music ticks.
    constexpr std::array ticks{225,336,445,560,665,779,888,998,1105,1215,1325,1435};
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
    require(srf::sha256(states)=="8bfbca070f209f78de6fe731450690eb17a54379d004e5844c4e728f189219bf",
            "Night Owl sequencer channels differ from original");
    srf::MusicRenderer renderer(bank);
    std::vector<std::int16_t> pcm(32000*2*120);
    renderer.render(pcm);
    require(srf::sha256({reinterpret_cast<const std::uint8_t*>(pcm.data()),pcm.size()*2})==
            "0df0cceb2d2b104a63b4d75703b601a113840049c4733aa87cb4ae673f5590af","Night Owl music rendering changed");
    renderer.reset();renderer.set_music_enabled(false);renderer.play_effect(0x52);
    auto flyover=std::span(pcm).first(32000);
    renderer.render(flyover);
    require(std::any_of(flyover.begin(),flyover.end(),[](auto s){return s!=0;}),"Night Owl flyover effect is silent");
}
void check_visuals(const srf::Rom& rom) {
    constexpr auto course=srf::CourseId::night_owl;
    const auto backdrop=srf::extract_course_backdrop(rom,course);
    require(srf::sha256(backdrop.indices[0])=="7285de15864681ffdf624fdaccae2041eba9efe98fb4ec7b097a1a660e72e18d",
            "Night Owl skyline differs from original tilemap and glyphs");
    const auto view=srf::backdrop_view(0,-3184);
    std::vector<std::uint8_t> sky;
    for(int y=4;y<13;++y)for(int x=0;x<170;++x) {
        const auto i=srf::backdrop_index(backdrop,0,view,x,y);
        append(sky,i==1?backdrop.gradients[0][view.gradient_y+y]:backdrop.palettes[0][i],2);
    }
    require(srf::sha256(sky)=="81073c05eaf8a811269fac5cde766e9353396262a25ecc9edb4203187ad64884",
            "Night Owl skyline differs from the original displayed grid view");
    const auto base=srf::extract_world_palette(rom,course),target=srf::extract_world_palette(rom,course,true);
    constexpr std::pair<unsigned,const char*> references[]{
        {0,"47c4fff38d8dc0bfab45f2ede9e656cccac337f8fb369b37858b2f6a308faf6c"},
        {16,"7aff4bee988a42a77361fe7ae6732643f6f6d5a121ca0471da42385714dda170"},
        {30,"df7ed18caf6ecbd7ea4e02cfa63b774daa0eab79aa2064596af0661ab7df2d95"}};
    for(const auto& [weight,expected]:references) {
        std::vector<std::uint8_t> palette;
        for(unsigned i=2;i<256;++i)append(palette,srf::blend_environment_rgb555(base[i],target[i],weight),2);
        require(srf::sha256(palette)==expected,"Night Owl world palette differs from original lighting blend");
    }
    const auto world=srf::extract_world(rom,srf::extract_course_scene(rom,course));
    std::vector<unsigned> selectors;
    for(const auto& placement:world.instances)if(placement.sign_phase)selectors.push_back(placement.sign_phase);
    require(selectors==std::vector<unsigned>{3,2,3},"Night Owl sign callbacks selected incorrect graphics");
    const auto sprites=srf::extract_vehicle_sprites(rom,course);
    const auto grandstand=std::find_if(sprites.begin(),sprites.end(),[](const auto& sprite) {
        return sprite.material_table==0x8f91 && sprite.phase==0xd5;
    });
    require(grandstand!=sprites.end() && grandstand->source_address==0x13e020 &&
            grandstand->width==64 && grandstand->height==32 &&
            grandstand->texcoords==std::array<std::array<std::uint8_t,2>,4>{{{0,0},{63,0},{63,31},{0,31}}},
            "Night Owl grandstand is missing or has incorrect original UVs");
    std::vector<std::uint8_t> rgba;
    for(auto pixel:grandstand->pixels)append(rgba,pixel,4);
    require(srf::sha256(rgba)=="db5dd93d2ad8e954ddcdd82d53341285f6b06d77fd54fa378d95546568478493",
            "Night Owl grandstand texels differ from the original material");
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        check_audio(rom);check_visuals(rom);
        std::cout<<"Night Owl original sound, skyline, palette and signs passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
