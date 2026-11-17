#include "srf/audio.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
}
int wmain(int argc,wchar_t** argv) {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path) { std::cout<<"SRF_ROM is required\n"; return 77; }
        const auto rom=srf::Rom::load(path);
        const auto easy=srf::extract_easy_ride_audio(rom);
        require(srf::sha256(srf::encode_audio(easy))==
            "377b5be1ab6631f2cde97f779d42ed6253cddeb706706d630ad4fe4b19497850",
            "Easy Ride audio changed");
        srf::MusicRenderer easy_renderer(easy);
        std::vector<std::int16_t> easy_pcm(60*32000*2);easy_renderer.render(easy_pcm);
        require(srf::sha256({reinterpret_cast<const std::uint8_t*>(easy_pcm.data()),easy_pcm.size()*2})==
            "a2ea4fa3e262250c1c4b041c8dc2c47c0e4e001099e98df3d1b9288dbcbb8a3f",
            "Easy Ride rendered audio changed");
        const auto aqua=srf::extract_course_audio(rom,srf::CourseId::aqua_tunnel);
        require(aqua.music.size()==4361 && srf::sha256(aqua.music)==
            "71ed4c9b3ef9a35954ed7f4a12e2dfbcbefa3ba7bb8ae665897a6b6fd133b120",
            "Aqua Tunnel music differs from original sound-memory capture");
        const auto encoded=srf::encode_audio(aqua);
        require(srf::encode_audio(srf::decode_audio(encoded))==encoded,"Aqua audio roundtrip differs");
        auto truncated=encoded;truncated.pop_back();
        bool rejected=false;
        try { srf::decode_audio(truncated); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"Truncated Aqua audio accepted");
        srf::MusicSequence sequence(aqua);
        unsigned notes=0;
        for(unsigned tick=0;tick<20000;++tick)
            for(const auto& event:sequence.tick())if(event.command>=0x80 && event.command<0xc8)++notes;
        require(sequence.playing() && notes>500,"Aqua music failed to sequence multiple loops");
        srf::MusicRenderer renderer(aqua);
        std::vector<std::int16_t> whole(60*32000*2),chunked(whole.size());
        renderer.render(whole);renderer.reset();
        for(std::size_t p=0;p<chunked.size();p+=514)
            renderer.render(std::span(chunked).subspan(p,std::min<std::size_t>(514,chunked.size()-p)));
        require(whole==chunked,"Aqua audio changed with callback size or restart");
        require(std::any_of(whole.begin(),whole.end(),[](auto value){return value!=0;}),"Aqua audio is silent");
        // Optional development oracle: no state or sound image is distributed.
        if(argc==2) {
            std::ifstream stream(std::filesystem::path(argv[1]),std::ios::binary);
            std::vector<std::uint8_t> original(65536);
            require(bool(stream.read(reinterpret_cast<char*>(original.data()),original.size())),
                    "Cannot read original sound-memory capture");
            require(std::equal(aqua.music.begin(),aqua.music.end(),original.begin()+aqua.music_base),
                    "Aqua music differs from captured original bytes");
            for(const auto& sample:aqua.samples)
                require(std::equal(sample.brr.begin(),sample.brr.end(),original.begin()+sample.original_start),
                        "Aqua sample differs from captured original bytes");
            // Aqua uploads instruments0..25. Slot26 is not used by its music;
            // the original retains unrelated preceding-song bytes there.
            for(unsigned i=0;i<26;++i) {
                const auto& instrument=aqua.instruments[i];const auto at=0x590+i*6;
                require(instrument.sample==original[at] && instrument.adsr1==original[at+1] &&
                        instrument.adsr2==original[at+2] && instrument.gain==original[at+3] &&
                        instrument.pitch_multiplier==((original[at+4]<<8)|original[at+5]),
                        "Aqua instrument differs from original sound-memory capture");
            }
        }
        std::cout<<"Aqua audio extraction, original-byte parity, sequence loops, callback independence and restart passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
