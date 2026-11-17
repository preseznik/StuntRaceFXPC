#include "srf/rom.hpp"
#include "srf/random.hpp"
#include "srf/mesh.hpp"
#include "srf/vehicle_math.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}
template<typename F> void rejects(F function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--speed-batch") {
            int values[6];
            while(std::cin >> values[0]) {
                for(unsigned i=1;i<6;++i) require(bool(std::cin >> values[i]),"Incomplete projection tuple");
                std::array<std::int16_t,3> velocity,coefficients;
                for(unsigned i=0;i<6;++i) {
                    require(values[i]>=-32768 && values[i]<=32767,"Projection value out of range");
                    (i<3?velocity[i]:coefficients[i-3])=static_cast<std::int16_t>(values[i]);
                }
                std::cout << srf::project_speed(velocity,coefficients) << '\n';
            }
            return std::cin.eof() ? 0 : 1;
        }
        if (argc == 2 && std::string(argv[1]) == "--prng-batch") {
            unsigned a,b,c,d;
            while (std::cin >> a >> b >> c >> d) {
                require(a<256 && b<256 && c<256 && d<256,"PRNG byte out of range");
                std::array<std::uint8_t,4> state{static_cast<std::uint8_t>(a),static_cast<std::uint8_t>(b),
                    static_cast<std::uint8_t>(c),static_cast<std::uint8_t>(d)};
                srf::advance_random(state);
                std::cout << unsigned(state[0]) << ' ' << unsigned(state[1]) << ' '
                          << unsigned(state[2]) << ' ' << unsigned(state[3]) << '\n';
            }
            return std::cin.eof() ? 0 : 1;
        }
        require(srf::sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "SHA-256 empty vector");
        const std::uint8_t abc[]{'a','b','c'};
        require(srf::sha256(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA-256 abc vector");
        require(srf::Rom::lorom_offset(0x02df79) == 0x15f79, "PRNG LoROM offset");
        require(srf::Rom::lorom_offset(0x82df79) == 0x15f79, "LoROM mirror offset");
        require(srf::Rom::lorom_offset(0x1fffff) == srf::rom_size - 1, "Last ROM address");
        rejects([] { srf::Rom::lorom_offset(0x7e8000); }, "WRAM must not alias ROM");
        rejects([] { srf::Rom::lorom_offset(0x007fff); }, "Lower memory must not alias ROM");
        rejects([] { srf::Rom::lorom_offset(0x1008000); }, "Oversized address must fail");
        rejects([] { srf::Rom::from_bytes({}); }, "Empty ROM must fail");
        rejects([] { srf::Rom::from_bytes(std::vector<std::uint8_t>(srf::rom_size)); }, "Wrong ROM must fail");
        // These arithmetic cases specifically exercise CLC's initial borrow
        // and propagation through the four SBC instructions. ROM-execution
        // differential tests provide the independent oracle.
        std::array<std::uint8_t,4> zero{};
        require(srf::advance_random(zero) == 254, "Initial CLC borrow lost");
        require(zero == std::array<std::uint8_t,4>{254,255,254,254}, "Zero state transition");
        std::array<std::uint8_t,4> seed{1,2,3,4};
        require(srf::advance_random(seed) == 245, "Chained borrow lost");
        require(seed == std::array<std::uint8_t,4>{245,254,250,246}, "Seed transition");
        require(srf::project_speed({9,0,0},{-32768,0,0})==-2,"Projection negative DIV2 rounding");
        require(srf::project_speed({1,0,0},{-32768,0,0})==0,"Projection DIV2 negative one exception");
        require(srf::project_speed({-32768,0,0},{-32768,0,0})==-4096,"Projection fixed-point wrap");
        require(srf::json_string("a\n\"\\") == "\"a\\n\\\"\\\\\"", "JSON escaping");
        const srf::Mesh triangle{0x50000, {{{0,0,0},{10,0,0},{0,10,0}}},
            {{{0,1,2},{0,0,0,0,0,127}}}};
        const auto encoded=srf::encode_meshes({triangle});
        const auto decoded=srf::decode_meshes(encoded);
        require(decoded.size()==1 && decoded[0].frames==triangle.frames &&
                decoded[0].polygons[0].indices==triangle.polygons[0].indices,"Mesh codec round trip");
        for(std::size_t n=0;n<encoded.size();++n)
            rejects([&]{srf::decode_meshes(std::span(encoded).first(n));},"Truncated mesh accepted");
        auto corrupted=encoded; corrupted.push_back(0);
        rejects([&]{srf::decode_meshes(corrupted);},"Trailing mesh bytes accepted");
        corrupted=encoded; corrupted[0]^=1;
        rejects([&]{srf::decode_meshes(corrupted);},"Invalid mesh magic accepted");
        const auto change_u32=[&](std::size_t offset,std::uint32_t value) {
            corrupted=encoded;
            for(unsigned byte=0;byte<4;++byte) corrupted[offset+byte]=static_cast<std::uint8_t>(value>>(byte*8));
        };
        for(const auto& [offset,value]:std::initializer_list<std::pair<std::size_t,std::uint32_t>>{
                {8,4097},{16,65},{20,257},{24,257},{28,32769},{74,3}}) {
            change_u32(offset,value);
            rejects([&]{srf::decode_meshes(corrupted);},"Invalid mesh count/coordinate/index accepted");
        }
        std::cout << "Native core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
