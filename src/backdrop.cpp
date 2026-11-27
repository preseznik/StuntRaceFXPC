#include "srf/backdrop.hpp"
#include "srf/bonus_hud_tiles.hpp"
#include "srf/environment.hpp"
#include "srf/stunt_course.hpp"

#include <algorithm>
#include <stdexcept>

namespace srf {
namespace {
void need(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// Original GSU01:CF42 reverse-bit LZ asset stream; bounded to its ROM bank.
std::vector<std::uint8_t> unpack_backdrop(const Rom& rom,std::size_t end) {
    const auto source=rom.bytes();
    need(end>=8 && end<=source.size(),"Invalid compressed backdrop pointer");
    const auto lower=end&~std::size_t(0x7fff);
    need(end-8>=lower,"Compressed backdrop header crosses its ROM bank");
    const auto size=std::size_t(source[end-2])*256+source[end-1];
    need(size>=8 && size<=65536,"Invalid compressed backdrop size");
    std::vector<std::uint8_t> output(size);
    auto cursor=size, input=end-8;
    const auto word=[&](std::size_t at) {
        return (std::uint32_t(source[at])<<24)|(std::uint32_t(source[at+1])<<16)|
               (std::uint32_t(source[at+2])<<8)|source[at+3];
    };
    auto bits=word(input);
    need(bits!=0,"Compressed backdrop has no bit sentinel");
    const auto read=[&](unsigned count) {
        std::uint32_t value=0;
        for(unsigned i=0;i<count;++i) {
            auto bit=bits&1U; bits>>=1;
            if(!bits) {
                need(input>=lower+4,"Compressed backdrop source underflow");
                input-=4; const auto next=word(input);
                bit=next&1U; bits=0x80000000U|(next>>1);
            }
            value=(value<<1)|bit;
        }
        return value;
    };
    while(cursor) {
        auto literal=read(3);
        if(literal==7) {
            if(read(1)) { literal=read(10); if(!literal)literal=read(18); }
            else literal=read(4)+7;
        }
        need(literal<=cursor,"Compressed backdrop literal exceeds output");
        for(unsigned i=0;i<literal;++i)output[--cursor]=static_cast<std::uint8_t>(read(8));
        if(!cursor)break;
        const auto kind=read(2);
        unsigned length{},width{};
        if(kind==0) { length=2; width=8; }
        else if(kind==1) { length=3; width=read(1)?8:14; }
        else {
            if(kind==2)length=4;
            else {
                const auto extra=read(2);
                length=extra<2?extra+5:(extra==2?read(2)+7:read(8));
            }
            width=!read(1)?16:(read(1)?8:12);
        }
        const auto distance=read(width);
        need(length>0 && length<=cursor && distance>0 && distance<=size-cursor,
             "Compressed backdrop copy exceeds decoded output");
        for(unsigned i=0;i<length;++i) { --cursor; output[cursor]=output[cursor+distance]; }
    }
    return output;
}
std::uint32_t pointer(const Rom& rom, std::uint32_t table, unsigned index) {
    const auto bytes = rom.slice(Rom::lorom_offset(table) + index * 3, 3);
    return bytes[0] | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16);
}
std::vector<std::uint8_t> decode_image(std::span<const std::uint8_t> map,
                                      std::span<const std::uint8_t> tiles) {
    need(map.size() == 8192 && tiles.size() == 6144, "Invalid backdrop asset size");
    std::vector<std::uint8_t> pixels(CourseBackdrop::width * CourseBackdrop::height);
    // CPU03:DB83 expands four horizontal 32x32 pages, repeating page zero.
    // Native sampling wraps the four original pages instead of storing a fifth.
    for (unsigned page = 0; page < 4; ++page) {
        for (unsigned ty = 0; ty < 32; ++ty) {
            for (unsigned tx = 0; tx < 32; ++tx) {
                const auto offset = page * 2048 + (ty * 32 + tx) * 2;
                const unsigned entry = map[offset] | (unsigned(map[offset + 1]) << 8);
                const unsigned tile = entry & 0x3ff;
                need(tile >= 192 && tile < 384 && !(entry & 0x1c00),
                     "Unsupported backdrop tile or palette bank");
                for (unsigned y = 0; y < 8; ++y) {
                    const auto row = (entry & 0x8000) ? 7 - y : y;
                    const auto at = (tile - 192) * 32 + row * 2;
                    for (unsigned x = 0; x < 8; ++x) {
                        const unsigned bit = (entry & 0x4000) ? x : 7 - x;
                        const auto value = ((tiles[at] >> bit) & 1) |
                            (((tiles[at + 1] >> bit) & 1) << 1) |
                            (((tiles[at + 16] >> bit) & 1) << 2) |
                            (((tiles[at + 17] >> bit) & 1) << 3);
                        pixels[(ty * 8 + y) * CourseBackdrop::width + page * 256 + tx * 8 + x] =
                            static_cast<std::uint8_t>(value);
                    }
                }
            }
        }
    }
    return pixels;
}
void decode_palette(const Rom& rom, std::uint32_t address, unsigned run_offset,
                    std::array<std::uint16_t, 16>& palette,
                    std::array<std::uint16_t, 512>& gradient) {
    auto colors = Rom::lorom_offset(address);
    for (auto& color : palette) { color = rom.u16(colors); colors += 2; }
    // CPU03:ED34/ED46 expand run lengths into 512 scanline colors. Aqua's
    // alternate last run reaches 528, then CPU03:ED20 clears the extra 16.
    const auto runs = rom.slice(Rom::lorom_offset(0x1fb44e) + run_offset, 0x50);
    std::size_t cursor = 0;
    for (const auto count : runs) {
        need(count != 0, "Unterminated backdrop gradient");
        const auto color = rom.u16(colors); colors += 2;
        const auto length = std::min<std::size_t>(count, gradient.size() - cursor);
        std::fill_n(gradient.begin() + cursor, length, color);
        cursor += length;
        if (cursor == gradient.size()) return;
    }
    throw std::runtime_error("Incomplete backdrop gradient");
}
}

std::vector<std::uint8_t> unpack_rom_graphics(const Rom& rom, std::uint32_t end_address) {
    return unpack_backdrop(rom, Rom::lorom_offset(end_address));
}

static CourseBackdrop extract_indexed_backdrop(const Rom& rom,unsigned index,CourseId course) {
    CourseBackdrop result;
    if(course==CourseId::night_cruise) {
        // CPU04:E375..E3C2 changes slots 12..15 of the target palette on each
        // race update. The panorama itself stays fixed while its lights cycle.
        constexpr std::array tables{0x04e50fu,0x04e53bu,0x04e4b7u,0x04e4e3u};
        auto& cycle=result.city_lights.emplace();
        for(unsigned phase=0;phase<cycle.size();++phase)
            for(unsigned color=0;color<tables.size();++color)
                cycle[phase][color]=rom.u16(Rom::lorom_offset(tables[color])+2*phase);
    }
    if (course == CourseId::harbor_city) {
        constexpr std::array tables{0x04e407u, 0x04e433u};
        auto& cycles = result.harbor_lights.emplace();
        for (unsigned mode = 0; mode < cycles.size(); ++mode)
            for (unsigned phase = 0; phase < cycles[mode].size(); ++phase)
                cycles[mode][phase] = rom.bytes()[Rom::lorom_offset(tables[mode]) + 2 * phase];
    }
    if(course==CourseId::kings_forest || course==CourseId::white_land || course==CourseId::lake_side ||
       course==CourseId::sky_ramp || course==CourseId::harbor_city) {
        for(unsigned i=0;i<result.fog_protected.size();++i)
            result.fog_protected[i]=rom.u16(Rom::lorom_offset(0x03ef1f)+2*i);
    }
    auto tiles = unpack_backdrop(rom, Rom::lorom_offset(pointer(rom, 0x03f4f9, index)));
    need(tiles.size() == 0x1800, "Invalid backdrop tile extent");
    // CPU03:DA1E skips the alternate glyph DMA when the pointer is zero.
    // Sunset uses the same 192 glyphs for both maps.
    if (const auto address = pointer(rom, 0x03f54d, index)) {
        const auto alternate = unpack_backdrop(rom, Rom::lorom_offset(address));
        need(alternate.size() == 0x800, "Invalid alternate backdrop tile extent");
        std::copy(alternate.begin(), alternate.end(), tiles.begin() + 0x1000);
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        const auto address = pointer(rom, mode ? 0x03f5f5 : 0x03f5a1, index);
        // CPU03:DBBF skips the secondary map upload on Night Owl. Its
        // lighting transitions keep the primary city panorama selected.
        if (mode && !address) result.indices[mode] = result.indices[0];
        else result.indices[mode] = decode_image(unpack_backdrop(rom, Rom::lorom_offset(address)), tiles);
        // CPU03:ED34 selects the longer first band of Sky Ramp's primary gradient.
        unsigned run_offset = 0;
        if (!mode && course == CourseId::sky_ramp) run_offset = 0x28;
        if (mode && course == CourseId::aqua_tunnel) run_offset = 0x50;
        decode_palette(rom, pointer(rom, mode ? 0x03f745 : 0x03f6f1, index), run_offset,
                       result.palettes[mode], result.gradients[mode]);
    }
    return result;
}

CourseBackdrop extract_course_backdrop(const Rom& rom,CourseId course) {
    const auto index=static_cast<unsigned>(course_definition(asset_course(course)).id);
    if(course==CourseId::easy_ride)return {};
    auto sky=extract_indexed_backdrop(rom,index,course);
    if(course==CourseId::kasumi_loop) {
        // Keep the sky gradient above a distant, seamless ring of city blocks.
        for(unsigned layer=0;layer<sky.indices.size();++layer) {
            auto& pixels=sky.indices[layer];
            auto& palette=sky.palettes[layer];
            palette[2]=13|(17<<5)|(20<<10);
            palette[3]=10|(14<<5)|(18<<10);
            palette[4]=17|(20<<5)|(21<<10);
            for(unsigned x=0;x<sky.width;++x) {
                const unsigned block=x/16,side=x%16;
                const unsigned roof=209-(block*17+block/3)%21+(side==15?7:0);
                for(unsigned y=0;y<sky.height;++y)
                    pixels[y*sky.width+x]=y<roof?1:(y<218 && (y-roof)%4==2 && side%5<2?4:2+block%2);
            }
        }
    }
    return sky;
}

CourseBackdrop extract_speed_bonus_backdrop(const Rom& rom) {
    // Entries 20, 21, and 22 share both panorama maps, tiles and palettes.
    return extract_indexed_backdrop(rom,20,CourseId::easy_ride);
}

CourseBackdrop extract_local_backdrop(const Rom& rom,unsigned index) {
    return extract_indexed_backdrop(rom,index,CourseId::easy_ride);
}

CourseBackdrop extract_rock_field_backdrop(const Rom& rom,StuntCourse course) {
    return extract_indexed_backdrop(rom,stunt_rom_index(course),CourseId::easy_ride);
}

CourseBackdrop extract_easy_ride_demo_backdrop(const Rom& rom) {
    return extract_indexed_backdrop(rom,0,CourseId::easy_ride);
}

std::vector<std::uint8_t> extract_start_light_tiles(const Rom& rom) {
    auto tiles=unpack_backdrop(rom,Rom::lorom_offset(0x1cf2f2));
    need(tiles.size()==1024,"Invalid original start-light graphics");
    return tiles;
}

std::vector<std::uint8_t> extract_bonus_hud_tiles(const Rom& rom) {
    auto tiles=unpack_backdrop(rom,Rom::lorom_offset(0x18ffb8));
    need(tiles.size()==2048,"Invalid original race HUD graphics");
    return tiles;
}

std::vector<std::uint8_t> extract_bonus_course_map_tiles(const Rom& rom) {
    auto tiles=unpack_backdrop(rom,Rom::lorom_offset(0x1dd970));
    need(tiles.size()==0x3000,"Invalid original course map graphics");
    return tiles;
}

std::vector<std::uint8_t> extract_bonus_dashboard_tiles(const Rom& rom,unsigned icon) {
    auto tiles=unpack_backdrop(rom,Rom::lorom_offset(0x18f644));
    need(tiles.size()==2688,"Invalid original dashboard graphics");
    const auto icons=unpack_backdrop(rom,Rom::lorom_offset(0x1d9228));
    need(icons.size()==1024 && icon<4,"Invalid original spare car icon");
    tiles.insert(tiles.end(),icons.begin()+256*icon,icons.begin()+256*(icon+1));
    return tiles;
}

std::vector<std::uint8_t> extract_bonus_map_marker(const Rom& rom) {
    auto tiles=unpack_backdrop(rom,Rom::lorom_offset(0x1d8078));
    need(tiles.size()==0x120,"Invalid original course map marker graphics");
    return tiles;
}

std::uint16_t backdrop_palette_target(const CourseBackdrop& backdrop,unsigned index,unsigned skyline_phase) {
    if(backdrop.city_lights && index>=12 && index<16)
        return (*backdrop.city_lights)[skyline_phase%backdrop.city_lights->size()][index-12];
    return backdrop.palettes[1].at(index);
}

std::uint16_t backdrop_color(const CourseBackdrop& backdrop, unsigned index,
                             unsigned palette_weight, unsigned skyline_phase,
                             unsigned skyline_blend_phase) {
    auto from = backdrop.palettes[0].at(index);
    auto to = backdrop_palette_target(backdrop, index, skyline_phase);
    if (backdrop.harbor_lights && index == 15) {
        // CPU04:E3DC switches cycles at weight 24. Its three 8-bit stores
        // preserve the upper byte in each working palette and the display copy.
        const auto& cycle = (*backdrop.harbor_lights)[palette_weight >= 24];
        const auto low = cycle[skyline_phase % cycle.size()];
        // The palette task recalculates the high byte only when the weight
        // changes. At the 23 -> 24 transition its inputs still use the day cycle.
        const auto& blend_cycle = (*backdrop.harbor_lights)[palette_weight > 24];
        const auto latched = blend_cycle[skyline_blend_phase % blend_cycle.size()];
        from = (from & 0xff00) | latched;
        to = (to & 0xff00) | latched;
        const auto blended = blend_environment_rgb555(from, to, palette_weight);
        return (blended & 0xff00) | low;
    }
    return blend_environment_rgb555(from, to, palette_weight);
}

BackdropView backdrop_view(std::int16_t yaw, std::int16_t pitch, bool elevated_course_flag) {
    // CPU03:D306 converts camera angles, CPU02:C53C selects panorama rows.
    // Signed right shifts are arithmetic in C++20, matching CMP/ROR pairs.
    const int vertical = std::clamp(-(int(pitch) >> 6), -56, 232) + 152 +
                         (elevated_course_flag ? 8 : 0);
    // PPU background sampling adds one to the scanline. Gradient HDMA starts
    // at source +33 independently, as compared against original RGB output.
    return {(int(yaw) >> 6) & 1023, vertical + 1, std::min(vertical + 33, 384)};
}

std::uint8_t backdrop_index(const CourseBackdrop& backdrop, unsigned mode,
                            const BackdropView& view, int x, int y) {
    need(backdrop.available() && mode < 2, "Unavailable backdrop mode");
    const int row = view.y + y;
    // CPU02:C5DD fills out-of-range rows with tile 192: solid palette index1.
    if (row < 0 || row >= int(CourseBackdrop::height)) return 1;
    return backdrop.indices[mode][std::size_t(row) * CourseBackdrop::width +
                                  (std::uint32_t(view.x + x) & 1023U)];
}
}
