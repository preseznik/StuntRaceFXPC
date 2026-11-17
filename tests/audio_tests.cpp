#include "srf/audio.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool pass,const char* message) { if(!pass) throw std::runtime_error(message); }
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    require(bool(stream),"Cannot open audio package"); const auto size=stream.tellg();
    require(size>=0 && size<=4*1024*1024,"Invalid audio package size");
    stream.seekg(0); std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    require(bool(stream.read(reinterpret_cast<char*>(bytes.data()),size)),"Cannot read audio package"); return bytes;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==2 && std::wstring(argv[1])==L"--brr-batch") {
            int a,b; std::string hex;
            while(std::cin>>a>>b>>hex) {
                require(a>=-16384 && a<16384 && b>=-16384 && b<16384 && hex.size()==18,"Invalid BRR case");
                std::array<std::uint8_t,9> block;
                for(unsigned i=0;i<9;++i) {
                    std::size_t consumed=0; auto value=std::stoul(hex.substr(i*2,2),&consumed,16);
                    require(consumed==2,"Invalid BRR hex"); block[i]=static_cast<std::uint8_t>(value);
                }
                srf::BrrHistory h{static_cast<std::int16_t>(a),static_cast<std::int16_t>(b)};
                for(auto value:srf::decode_brr_block(block,h)) std::cout<<value<<' ';
                std::cout<<h.last<<' '<<h.previous<<'\n';
            }
            return std::cin.eof()?0:1;
        }
        if((argc==4 || argc==5) && std::wstring(argv[1])==L"--trace") {
            const auto bytes=read(argv[2]); const auto bank=srf::decode_audio(bytes);
            require(srf::encode_audio(bank)==bytes,"Audio codec roundtrip differs");
            std::array<srf::MusicArticulation,8> articulation{};
            if(argc==5) {
                require(std::wstring(argv[4])==L"--seed-stdin","Invalid trace seed option");
                for(auto& a:articulation) {
                    unsigned duration,gate,volume;
                    require(bool(std::cin>>duration>>gate>>volume) && duration<256 && gate<256 && volume<256,"Invalid articulation seed");
                    a={static_cast<std::uint8_t>(duration),static_cast<std::uint8_t>(gate),static_cast<std::uint8_t>(volume)};
                }
            }
            srf::MusicSequence music(bank,articulation); const auto ticks=std::stoul(argv[3]);
            require(ticks<=100000,"Trace tick limit exceeded");
            for(unsigned i=0;i<ticks && music.playing();++i) {
                const auto events=music.tick(); std::cout<<"{\"tick\":"<<i<<",\"channels\":[";
                for(unsigned j=0;j<8;++j) {
                    const auto& ch=music.channels()[j]; if(j) std::cout<<',';
                    std::cout<<'['<<ch.cursor<<','<<unsigned(ch.remaining)<<','<<unsigned(ch.duration)<<','
                             <<unsigned(ch.gate)<<','<<unsigned(ch.volume)<<','<<unsigned(ch.repeat_count)<<','
                             <<unsigned(ch.instrument)<<']';
                }
                std::cout<<"],\"events\":[";
                for(unsigned j=0;j<events.size();++j) {
                    const auto& ev=events[j]; if(j) std::cout<<',';
                    std::cout<<'['<<unsigned(ev.channel)<<','<<unsigned(ev.command);
                    for(unsigned k=0;k<ev.parameter_count;++k) std::cout<<','<<unsigned(ev.parameters[k]);
                    std::cout<<']';
                }
                std::cout<<"]}\n";
            }
            return 0;
        }
        if(argc==3 && std::wstring(argv[1])==L"--validate") {
            const auto bytes=read(argv[2]); srf::decode_audio(bytes); return 0;
        }
        if(argc==5 && std::wstring(argv[1])==L"--effect-trace") {
            const auto bank=srf::decode_audio(read(argv[2]));srf::MusicRenderer renderer(bank);
            renderer.set_music_enabled(false);renderer.play_effect(static_cast<std::uint8_t>(std::stoul(argv[3],nullptr,16)));
            renderer.set_engine(144,false,false,false);
            const auto ticks=std::stoul(argv[4]);require(ticks<=10000,"Effect tick limit exceeded");
            std::array<std::int16_t,1024> block{};
            for(unsigned tick=0;tick<ticks;++tick) {
                renderer.render(block);std::cout<<"{\"tick\":"<<tick<<",\"channels\":[";
                const auto states=renderer.effect_states();
                for(unsigned i=0;i<states.size();++i) {
                    if(i)std::cout<<',';const auto& s=states[i];
                    std::cout<<'['<<unsigned(s.id)<<','<<s.cursor<<','<<unsigned(s.remaining)<<','<<unsigned(s.left)<<','
                             <<unsigned(s.right)<<','<<s.pitch<<','<<unsigned(s.sample)<<','<<unsigned(s.adsr1)<<','<<unsigned(s.adsr2)<<']';
                }
                std::cout<<"]}\n";
            }
            return 0;
        }
        if(argc==5 && std::wstring(argv[1])==L"--render") {
            const auto bank=srf::decode_audio(read(argv[2]));
            const unsigned seconds=std::stoul(argv[4]);require(seconds>0 && seconds<=180,"Audio duration limit exceeded");
            std::vector<std::int16_t> samples(seconds*32000*2);
            srf::MusicRenderer renderer(bank);renderer.render(samples);
            // Audio callbacks may request arbitrary block sizes. The result
            // must be identical across those boundaries and after restart.
            renderer.reset();std::vector<std::int16_t> chunked(samples.size());
            for(std::size_t p=0;p<chunked.size();p+=514)
                renderer.render(std::span(chunked).subspan(p,std::min<std::size_t>(514,chunked.size()-p)));
            require(samples==chunked,"Audio depends on callback block size or reset state");
            std::ofstream out(std::filesystem::path(argv[3]),std::ios::binary);
            auto word=[&](std::uint32_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)out.put(char(value>>(i*8)));};
            out.write("RIFF",4);word(36+samples.size()*2,4);out.write("WAVEfmt ",8);word(16,4);
            word(1,2);word(2,2);word(32000,4);word(128000,4);word(4,2);word(16,2);
            out.write("data",4);word(samples.size()*2,4);
            out.write(reinterpret_cast<const char*>(samples.data()),static_cast<std::streamsize>(samples.size()*2));
            require(bool(out),"Cannot write audio rendering");
            std::cout<<"{\"frames\":"<<samples.size()/2<<",\"music_ticks\":"<<renderer.ticks()
                     <<",\"chunk_and_restart_identical\":true}\n";return 0;
        }
        require(argc==1,"Unknown audio test arguments");
        std::array<std::uint8_t,9> block{}; srf::BrrHistory history{};
        for(auto sample:srf::decode_brr_block(block,history)) require(sample==0,"Silent BRR block");
        block[0]=0xC0; block[1]=0x78;
        const auto result=srf::decode_brr_block(block,history);
        require(result[0]==28672 && result[1]==-32768,"Signed BRR nibbles");
        block[0]=0xD0; history={};
        const auto invalid_range=srf::decode_brr_block(block,history);
        require(invalid_range[0]==0 && invalid_range[1]==-4096,"BRR hardware invalid-range behavior");
        std::cout<<"Native audio tests passed\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
