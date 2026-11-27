#include "srf/audio.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace srf {
namespace {
constexpr std::array<std::uint8_t,8> magic{'S','R','F','A','U','D','0','3'};
constexpr auto no_loop=UINT32_MAX;
void need(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
// Original GSU01:CF42..D11B data decompressor. Source bits and destination
// bytes run backwards; a sentinel marks each 32-bit source refill. This reads
// a bounded asset stream, never console instructions or a reference snapshot.
std::vector<std::uint8_t> unpack_music(const Rom& rom,std::size_t end) {
    const auto source=rom.bytes();
    need(end>=8 && end<=source.size(),"Invalid compressed music pointer");
    const auto lower=end&~std::size_t(0x7fff);
    need(end-8>=lower,"Compressed music header crosses its ROM bank");
    const auto size=std::size_t(source[end-2])*256+source[end-1];
    need(size>=8 && size<=65536,"Invalid compressed music size");
    std::vector<std::uint8_t> output(size);
    auto cursor=size, input=end-8;
    const auto word=[&](std::size_t at) {
        return (std::uint32_t(source[at])<<24)|(std::uint32_t(source[at+1])<<16)|
               (std::uint32_t(source[at+2])<<8)|source[at+3];
    };
    auto bits=word(input);
    need(bits!=0,"Compressed music has no bit sentinel");
    const auto read=[&](unsigned count) {
        std::uint32_t value=0;
        for(unsigned i=0;i<count;++i) {
            auto bit=bits&1U; bits>>=1;
            if(!bits) {
                need(input>=lower+4,"Compressed music source underflow");
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
        need(literal<=cursor,"Compressed music literal exceeds output");
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
             "Compressed music copy exceeds decoded output");
        for(unsigned i=0;i<length;++i) { --cursor; output[cursor]=output[cursor+distance]; }
    }
    return output;
}
void upload_sound(std::span<const std::uint8_t> source,std::size_t offset,
                  std::array<std::uint8_t,65536>& ram,std::array<std::uint8_t,65536>& loaded) {
    for(unsigned block=0;block<64;++block) {
        need(offset<=source.size() && source.size()-offset>=4,"Truncated sound upload header");
        const auto size=std::size_t(source[offset])|(std::size_t(source[offset+1])<<8);
        const auto address=std::size_t(source[offset+2])|(std::size_t(source[offset+3])<<8);
        offset+=4;
        if(!size) { need(address==0x800,"Unexpected sound upload entry point"); return; }
        need(size<=ram.size()-address && size<=source.size()-offset,"Sound upload exceeds source or sound RAM");
        std::copy_n(source.begin()+offset,size,ram.begin()+address);
        std::fill_n(loaded.begin()+address,size,1); offset+=size;
    }
    throw std::runtime_error("Unterminated sound upload");
}
std::int16_t signed16(std::int32_t value) {
    const auto low=static_cast<std::uint16_t>(value);
    return static_cast<std::int16_t>(low<32768?static_cast<int>(low):static_cast<int>(low)-65536);
}
void u32(std::vector<std::uint8_t>& out,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) out.push_back(static_cast<std::uint8_t>(value>>(8*i)));
}
struct Reader {
    std::span<const std::uint8_t> data; std::size_t cursor=0;
    std::uint8_t byte() { need(cursor<data.size(),"Truncated sound package"); return data[cursor++]; }
    std::uint32_t integer() {
        std::uint32_t result=0;
        for(unsigned i=0;i<4;++i) result|=std::uint32_t(byte())<<(i*8);
        return result;
    }
};
bool supported_music_extent(std::size_t bytes) {
    return bytes==4532 || bytes==4361 || bytes==2434 || bytes==3427 || bytes==2273 || bytes==2691 || bytes==2297 || bytes==2408 || bytes==4020 || bytes==3985 || bytes==1803 || bytes==855 || bytes==4510;
}
void validate(const SoundBank& bank) {
    need(bank.instruments.size()==27 || bank.instruments.size()==29,"Invalid instrument count");
    need(!bank.samples.empty() && bank.samples.size()<=40,"Invalid sample count");
    need(bank.music_base==0xEE40 && supported_music_extent(bank.music.size()),
         "Invalid supported course music range");
    need(bank.effects.size()==0xC735-0xBCA0,"Invalid effect sequence extent");
    for(auto pointer:bank.effect_pointers)
        need(pointer>=0xBCA0 && pointer<0xC735,"Invalid effect sequence pointer");
    std::array<bool,40> used{};
    for(const auto& sample:bank.samples) {
        need(sample.directory_index<40 && !used[sample.directory_index],"Invalid or repeated sample index");
        used[sample.directory_index]=true;
        need(!sample.brr.empty() && sample.brr.size()%9==0 &&
             std::size_t(sample.original_start)+sample.brr.size()<=65536,"Invalid BRR length");
        const auto flags=sample.brr[sample.brr.size()-9]&3;
        need((flags&1)!=0,"BRR sample has no terminator");
        for(std::size_t p=0;p+9<sample.brr.size();p+=9)
            need((sample.brr[p]&1)==0,"Early BRR terminator");
        need((flags&2)?sample.loop_byte_offset<sample.brr.size() && sample.loop_byte_offset%9==0:
                       sample.loop_byte_offset==no_loop,"Invalid BRR loop");
    }
    for(const auto& instrument:bank.instruments)
        // Instrument19 exists in the original table while its sample slot is
        // deliberately unloaded for Easy Ride. A voice must reject that slot.
        need(instrument.sample<40,"Instrument sample index is out of range");
}
}

std::array<std::int16_t,16> decode_brr_block(std::span<const std::uint8_t,9> block,BrrHistory& h) {
    std::array<std::int16_t,16> output{};
    const unsigned shift=block[0]>>4, filter=(block[0]>>2)&3;
    for(unsigned i=0;i<16;++i) {
        int value=(block[1+i/2]>>(i%2?0:4))&15;
        if(value>=8) value-=16;
        value=shift<=12?(value*static_cast<int>(1U<<shift))>>1:(value<0?-2048:0);
        const int p1=h.last, p2=h.previous;
        if(filter==1) value+=p1+((-p1)>>4);
        if(filter==2) value+=2*p1+((-3*p1)>>5)-p2+(p2>>4);
        if(filter==3) value+=2*p1+((-13*p1)>>6)-p2+((3*p2)>>4);
        value=std::clamp(value,-32768,32767);
        output[i]=signed16(value*2);
        h.previous=h.last; h.last=static_cast<std::int16_t>(output[i]>>1);
    }
    return output;
}

SoundBank extract_easy_ride_audio(const Rom& rom) {
    return extract_course_audio(rom,CourseId::easy_ride);
}
static SoundBank extract_indexed_audio(const Rom& rom,unsigned index,CourseId course);
static SoundBank extract_sound_bank(const Rom& rom,std::array<std::uint32_t,6> uploads,unsigned engine_profile,CourseId course);
SoundBank extract_course_audio(const Rom& rom,CourseId course) {
    return extract_indexed_audio(rom,static_cast<unsigned>(course_definition(asset_course(course)).id),course);
}
SoundBank extract_speed_bonus_audio(const Rom& rom) {
    // All three Bonus Trax entries select the same song, instruments and effects.
    return extract_indexed_audio(rom,20,CourseId::easy_ride);
}
SoundBank extract_local_audio(const Rom& rom,unsigned index) {
    return extract_indexed_audio(rom,index,CourseId::easy_ride);
}
SoundBank extract_stunt_audio(const Rom& rom,StuntCourse course) {
    return extract_indexed_audio(rom,stunt_rom_index(course),CourseId::easy_ride);
}
MusicTrack extract_trailer_loading_music(const Rom& rom) {
    // Song 5 in SPC table 07F4 points to CB10. This complete sequence is a
    // separate upload in the common sound bank at CPU 19:F252.
    const auto header=Rom::lorom_offset(0x19f252);
    const auto size=rom.u16(header),base=rom.u16(header+2);
    need(base==0xcb10 && size==0x1b3,"Invalid trailer music upload");
    const auto bytes=rom.slice(header+4,size);
    return {base,{bytes.begin(),bytes.end()}};
}
MusicTrack extract_bonus_tally_music(const Rom& rom) {
    // Song 2 in the common SPC bank. The last voice ends at CA74.
    const auto bytes=rom.slice(Rom::lorom_offset(0x19f030),0xca75-0xc8e2);
    return {0xc8e2,{bytes.begin(),bytes.end()}};
}
MusicTrack extract_bonus_tally_outro(const Rom& rom) {
    // The sound driver follows results song 2 with song 3 in the shared bank.
    const auto bytes=rom.slice(Rom::lorom_offset(0x19ee83),0xc8e2-0xc735);
    return {0xc735,{bytes.begin(),bytes.end()}};
}
SoundBank extract_course_map_audio(const Rom& rom) {
    // CPU03:F0FE loads the board song, instruments and menu effects. The
    // two other sample uploads are present in the board's original SPC image.
    return extract_sound_bank(rom,{0x19c27c,0x1ee049,0x1bb8a0,0x19b530,0x1ae885,0x1bd5fc},4,CourseId::easy_ride);
}
SoundBank extract_title_audio(const Rom& rom) {
    // CPU07:EC10..EC4E. Game Selection and Car Selection continue this song.
    return extract_sound_bank(rom,{0x19c27c,0x1ef3b5,0x1b85f8,0,0,0x199a98},4,CourseId::easy_ride);
}
static SoundBank extract_indexed_audio(const Rom& rom,unsigned index,CourseId course) {
    // Original CPU04:D18A selects these 24-bit tables by course index.
    const auto pointer=[&](std::uint32_t table) {
        const auto offset=Rom::lorom_offset(table)+index*3;
        return std::uint32_t(rom.u16(offset))|(std::uint32_t(rom.slice(offset+2,1)[0])<<16);
    };
    return extract_sound_bank(rom,{0x19C27C,pointer(0x04D322),pointer(0x04D376),0x198D4C,0x1ADB39,pointer(0x04D3CA)},
                              index==20?4:1,course);
}
static SoundBank extract_sound_bank(const Rom& rom,std::array<std::uint32_t,6> uploads,unsigned engine_profile,CourseId course) {
    // A null course upload retains the effects bank loaded before the race.
    const bool retain_menu_bank=uploads[5]==0;
    if(retain_menu_bank)uploads[5]=0x1bd5fc; // CPU03:F146
    std::array<std::uint8_t,65536> ram{},loaded{};
    if(retain_menu_bank || course==CourseId::sea_breeze || course==CourseId::harbor_city || uploads[1]==0x1e91bc) {
        // Bytes beyond the new song's end survive in the SPC noise seed at FF00.
        const auto previous_music=unpack_music(rom,Rom::lorom_offset(0x1ef3b5));
        upload_sound(previous_music,0,ram,loaded);
    }
    if(course==CourseId::white_land || course==CourseId::night_cruise) {
        const auto previous_music=unpack_music(rom,Rom::lorom_offset(0x1ecffd));
        upload_sound(previous_music,0,ram,loaded);
    }
    if(course==CourseId::sea_breeze || course==CourseId::white_land) {
        // These Expert rounds upload six fewer instrument bytes than round one.
        // Slot 26 in the original SPC state still comes from King's Forest.
        upload_sound(rom.bytes(),Rom::lorom_offset(0x1ba68a),ram,loaded);
    }
    std::size_t music_size=0;
    for(unsigned i=0;i<uploads.size();++i) {
        if(!uploads[i])continue;
        const auto offset=Rom::lorom_offset(uploads[i]);
        if(i!=1) { upload_sound(rom.bytes(),offset,ram,loaded);continue; }
        // CPU03:F05E/F077/F090 copies three songs directly. The other
        // sources use GSU01:CF42 to unpack the same upload record format.
        std::vector<std::uint8_t> decoded;
        std::span<const std::uint8_t> record=rom.bytes().subspan(offset);
        if(uploads[i]!=0x1e8000 && uploads[i]!=0x1e91bc && uploads[i]!=0x1eafc6) {
            decoded=unpack_music(rom,offset);record=decoded;
        }
        need(record.size()>=4 && record[2]==0x40 && record[3]==0xee,"Invalid course music upload");
        music_size=record[0]|(std::size_t(record[1])<<8);
        need(supported_music_extent(music_size),"Invalid course music length");
        upload_sound(record,0,ram,loaded);
    }
    const auto word=[&](std::size_t at){return static_cast<std::uint16_t>(ram.at(at)|(ram.at(at+1)<<8));};
    SoundBank bank;
    for(unsigned i=0;i<40;++i) {
        if(!loaded[0x3F00+4*i])continue; // Previous menu/course residue is outside this bank.
        const auto start=word(0x3F00+4*i),loop=word(0x3F02+4*i);
        if(start==0xFFFF) continue;
        SoundSample sample; sample.directory_index=static_cast<std::uint8_t>(i); sample.original_start=start;
        std::size_t cursor=start;
        for(;;) {
            need(cursor+9<=ram.size(),"Truncated BRR sample");
            need(std::all_of(loaded.begin()+cursor,loaded.begin()+cursor+9,[](auto x){return x!=0;}),
                 "BRR sample enters an unloaded region");
            const auto flags=ram[cursor]&3;
            sample.brr.insert(sample.brr.end(),ram.begin()+cursor,ram.begin()+cursor+9); cursor+=9;
            if(flags&1) {
                if(flags&2) {
                    need(loop>=start && loop<cursor && (loop-start)%9==0,"Invalid original BRR loop");
                    sample.loop_byte_offset=loop-start;
                }
                break;
            }
        }
        bank.samples.push_back(std::move(sample));
    }
    if(uploads[1]==0x1ef3b5)bank.instruments.resize(29); // Nine title instruments at SPC 0608.
    for(unsigned i=0;i<bank.instruments.size();++i) {
        const auto p=0x590+i*6;
        // OriginalSPC0BBE..0C16: pitch bytes are high,low, unlike pointers.
        bank.instruments[i]={ram[p],ram[p+1],ram[p+2],ram[p+3],
                             static_cast<std::uint16_t>((ram[p+4]<<8)|ram[p+5])};
    }
    std::copy_n(ram.begin()+0x650,8,bank.note_gates.begin());
    std::copy_n(ram.begin()+0x658,16,bank.note_volumes.begin());
    for(unsigned i=0;i<13;++i) bank.pitch_table[i]=word(0x6CD+2*i);
    std::copy_n(ram.begin()+0x107A,22,bank.pan_table.begin());
    for(unsigned filter=0;filter<4;++filter) for(unsigned i=0;i<8;++i)
        bank.echo_filters[filter][i]=static_cast<std::int8_t>(ram[0x3FAA+filter*8+i]);
    const auto curve=rom.slice(Rom::lorom_offset(0x0BB288),bank.engine_curve.size());
    std::copy(curve.begin(),curve.end(),bank.engine_curve.begin());
    for(unsigned i=0;i<2;++i) {
        const unsigned profile=engine_profile+i*5;
        bank.engine_profiles[i]={ram[0x2419+profile],ram[0x2423+profile],ram[0x242D+profile],ram[0x2437+profile]};
    }
    for(unsigned i=0;i<12;++i)bank.effect_pitch_table[i]=word(0xCAA8+i*2);
    for(unsigned i=0;i<122;++i)bank.effect_pointers[i]=word(0x702+i*2);
    bank.effects.assign(ram.begin()+0xBCA0,ram.begin()+0xC735);
    std::copy_n(ram.begin()+0xFF00,bank.effect_noise_seed.size(),bank.effect_noise_seed.begin());
    need(std::all_of(loaded.begin()+bank.music_base,loaded.begin()+bank.music_base+music_size,
                    [](auto value){return value!=0;}),"Music enters an unloaded region");
    bank.music.assign(ram.begin()+bank.music_base,ram.begin()+bank.music_base+music_size);
    validate(bank); return bank;
}

std::vector<std::uint8_t> encode_audio(const SoundBank& bank) {
    validate(bank); std::vector<std::uint8_t> out(magic.begin(),magic.end());
    if(bank.instruments.size()!=27) {
        out.back()='4';
        u32(out,static_cast<std::uint32_t>(bank.instruments.size()));
    }
    u32(out,static_cast<std::uint32_t>(bank.samples.size()));
    for(const auto& sample:bank.samples) {
        u32(out,sample.directory_index); u32(out,sample.original_start); u32(out,sample.loop_byte_offset);
        u32(out,static_cast<std::uint32_t>(sample.brr.size())); out.insert(out.end(),sample.brr.begin(),sample.brr.end());
    }
    for(const auto& instrument:bank.instruments) {
        out.insert(out.end(),{instrument.sample,instrument.adsr1,instrument.adsr2,instrument.gain});
        u32(out,instrument.pitch_multiplier);
    }
    out.insert(out.end(),bank.note_gates.begin(),bank.note_gates.end());
    out.insert(out.end(),bank.note_volumes.begin(),bank.note_volumes.end());
    for(auto value:bank.pitch_table) u32(out,value);
    out.insert(out.end(),bank.pan_table.begin(),bank.pan_table.end());
    for(const auto& filter:bank.echo_filters) for(auto value:filter) out.push_back(static_cast<std::uint8_t>(value));
    out.insert(out.end(),bank.engine_curve.begin(),bank.engine_curve.end());
    for(const auto& p:bank.engine_profiles)out.insert(out.end(),{p.volume_divisor,p.volume_base,p.pitch_base,p.pitch_slope});
    for(auto value:bank.effect_pitch_table)u32(out,value);
    for(auto value:bank.effect_pointers)u32(out,value);
    u32(out,static_cast<std::uint32_t>(bank.effects.size()));out.insert(out.end(),bank.effects.begin(),bank.effects.end());
    out.insert(out.end(),bank.effect_noise_seed.begin(),bank.effect_noise_seed.end());
    u32(out,bank.music_base); u32(out,static_cast<std::uint32_t>(bank.music.size()));
    out.insert(out.end(),bank.music.begin(),bank.music.end()); return out;
}
SoundBank decode_audio(std::span<const std::uint8_t> bytes) {
    need(bytes.size()<=4*1024*1024,"Oversized sound package"); Reader reader{bytes};
    for(auto value:std::span(magic).first(7))need(reader.byte()==value,"Invalid sound package signature");
    const auto version=reader.byte();
    need(version=='3' || version=='4',"Invalid sound package version");
    SoundBank bank;
    if(version=='4') {
        const auto instruments=reader.integer();
        need(instruments==29,"Invalid extended instrument table");
        bank.instruments.resize(instruments);
    }
    const auto count=reader.integer(); need(count>0 && count<=40,"Invalid sample count");
    for(unsigned i=0;i<count;++i) {
        SoundSample sample; const auto index=reader.integer(),start=reader.integer();
        need(index<40 && start<=65535,"Invalid sample identity");
        sample.directory_index=static_cast<std::uint8_t>(index); sample.original_start=static_cast<std::uint16_t>(start);
        sample.loop_byte_offset=reader.integer(); const auto size=reader.integer();
        need(size>0 && size<=65536 && size<=bytes.size()-reader.cursor,"Invalid sample byte count");
        sample.brr.assign(bytes.begin()+reader.cursor,bytes.begin()+reader.cursor+size); reader.cursor+=size;
        bank.samples.push_back(std::move(sample));
    }
    for(auto& instrument:bank.instruments) {
        instrument.sample=reader.byte(); instrument.adsr1=reader.byte();
        instrument.adsr2=reader.byte(); instrument.gain=reader.byte();
        const auto pitch=reader.integer(); need(pitch<=65535,"Invalid instrument pitch");
        instrument.pitch_multiplier=static_cast<std::uint16_t>(pitch);
    }
    for(auto& value:bank.note_gates) value=reader.byte();
    for(auto& value:bank.note_volumes) value=reader.byte();
    for(auto& value:bank.pitch_table) {
        const auto raw=reader.integer(); need(raw<=65535,"Invalid pitch table"); value=static_cast<std::uint16_t>(raw);
    }
    for(auto& value:bank.pan_table) value=reader.byte();
    for(auto& filter:bank.echo_filters) for(auto& value:filter) value=static_cast<std::int8_t>(reader.byte());
    for(auto& value:bank.engine_curve)value=reader.byte();
    for(auto& p:bank.engine_profiles)p={reader.byte(),reader.byte(),reader.byte(),reader.byte()};
    for(auto& value:bank.effect_pitch_table) {
        const auto raw=reader.integer();need(raw<=65535,"Invalid effect pitch");value=static_cast<std::uint16_t>(raw);
    }
    for(auto& value:bank.effect_pointers) {
        const auto raw=reader.integer();need(raw<=65535,"Invalid effect pointer");value=static_cast<std::uint16_t>(raw);
    }
    const auto effect_size=reader.integer();need(effect_size==0xC735-0xBCA0,"Invalid effect byte count");
    for(unsigned i=0;i<effect_size;++i)bank.effects.push_back(reader.byte());
    for(auto& value:bank.effect_noise_seed)value=reader.byte();
    const auto base=reader.integer(),size=reader.integer();
    need(base==0xEE40 && supported_music_extent(size) && size==bytes.size()-reader.cursor,
         "Invalid music data extent");
    bank.music_base=static_cast<std::uint16_t>(base); bank.music.assign(bytes.begin()+reader.cursor,bytes.end());
    validate(bank); return bank;
}
void generate_audio_assets(const Rom& rom,const std::filesystem::path& directory,CourseId course) {
    const auto package=directory/"audio.srfaudio", header=directory/"generated_audio.hpp";
    need(!std::filesystem::exists(package) && !std::filesystem::exists(header),"Audio output already exists");
    const auto bank=extract_course_audio(rom,course); const auto bytes=encode_audio(bank);
    std::ofstream data(package,std::ios::binary);
    need(bool(data.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))),"Cannot write audio package");
    std::ofstream output(header);
    output << "#pragma once\nnamespace srf::generated {\ninline constexpr char audio_sha256[]=\"" << sha256(bytes)
           << "\";\ninline constexpr unsigned audio_sample_count=" << bank.samples.size() << ";\n}\n";
    need(bool(output),"Cannot write audio manifest");
}

MusicSequence::MusicSequence(const SoundBank& bank,const std::array<MusicArticulation,8>& inherited):bank_(&bank) {
    for(unsigned i=0;i<8;++i) {
        channels_[i].duration=inherited[i].duration;
        channels_[i].gate=inherited[i].gate; channels_[i].volume=inherited[i].volume;
    }
    restart();
}
std::uint8_t MusicSequence::read(std::uint16_t& cursor) const {
    const auto& bytes=alternate_.bytes.empty()?bank_->music:alternate_.bytes;
    const auto base=alternate_.bytes.empty()?bank_->music_base:alternate_.base;
    need(cursor>=base && std::size_t(cursor-base)<bytes.size(),
         "Music pointer leaves the extracted sequence");
    return bytes[cursor++-base];
}
void MusicSequence::select_track(MusicTrack track) {
    alternate_=std::move(track);
    restart();
}
std::uint16_t MusicSequence::word(std::uint16_t& cursor) const {
    const unsigned low=read(cursor); return static_cast<std::uint16_t>(low|(read(cursor)<<8));
}
void MusicSequence::restart() {
    // SPC0A04 resets effects but retains note articulation from the preceding
    // song. Easy Ride's opening rests use those bytes until the first notes.
    const auto previous=channels_; channels_={};
    for(unsigned i=0;i<8;++i) {
        channels_[i].duration=previous[i].duration;
        channels_[i].gate=previous[i].gate; channels_[i].volume=previous[i].volume;
    }
    section_cursor_=alternate_.bytes.empty()?bank_->music_base:alternate_.base;
    section_repeat_=0; ticks_=0; playing_=true;
    next_section();
}
bool MusicSequence::next_section() {
    // SPC0A8C..0AD3. The low-byte section entries are repeat commands;
    // their following pointer is absolute. A zero word stops the song.
    for(unsigned budget=0;budget<256;++budget) {
        auto entry=word(section_cursor_);
        if(entry==0) { playing_=false; return false; }
        if(entry<0x100) {
            section_repeat_=static_cast<std::uint8_t>(section_repeat_-1);
            if(section_repeat_&0x80) section_repeat_=static_cast<std::uint8_t>(entry);
            const auto target=word(section_cursor_);
            if(section_repeat_) section_cursor_=target;
            continue;
        }
        for(auto& channel:channels_) {
            channel.cursor=word(entry); channel.repeat_count=0; channel.remaining=1;
        }
        return true;
    }
    throw std::runtime_error("Music section command cycle");
}
std::vector<MusicEvent> MusicSequence::tick() {
    std::vector<MusicEvent> events;
    if(!playing_) return events;
    // Argument counts verified from originalSPC108F..10A9 and3FCA..3FFE.
    constexpr std::array<std::uint8_t,27> counts{
        1,1,2,3,0,1,2,1,2,1,1,3,0,1,2,3,1,3,3,0,1,3,0,3,3,3,1};
    unsigned channel_index=0, budget=0;
    while(channel_index<channels_.size() && playing_) {
        need(++budget<=4096,"Music event budget exceeded");
        auto& channel=channels_[channel_index];
        const auto prefetch_slide=[&] {
            if(channel.slide_remaining || !channel.cursor) return;
            auto cursor=channel.cursor;
            if(read(cursor)!=0xF9) return;
            MusicEvent event{ticks_,static_cast<std::uint8_t>(channel_index),0xF9,{},3};
            for(auto& parameter:event.parameters) parameter=read(cursor);
            channel.cursor=cursor; channel.slide_delay=event.parameters[0];
            channel.slide_remaining=event.parameters[1]; events.push_back(event);
        };
        if(channel.cursor==0) { ++channel_index; continue; }
        if(--channel.remaining!=0) {
            if(channel.slide_remaining) {
                if(channel.slide_delay) --channel.slide_delay;
                else --channel.slide_remaining;
            }
            prefetch_slide(); ++channel_index; continue;
        }
        bool note=false, new_section=false;
        while(!note && !new_section && playing_) {
            need(++budget<=4096,"Music command cycle");
            auto value=read(channel.cursor);
            if(value==0) {
                if(channel.repeat_count) {
                    channel.cursor=channel.repeat_start;
                    if(--channel.repeat_count==0) channel.cursor=channel.repeat_return;
                } else {
                    new_section=next_section(); channel_index=0;
                }
                continue;
            }
            if(value<0x80) {
                channel.duration=value; value=read(channel.cursor);
                if(value<0x80) {
                    channel.gate=bank_->note_gates[(value>>4)&7];
                    channel.volume=bank_->note_volumes[value&15];
                    value=read(channel.cursor);
                }
            }
            need(value>=0x80,"Invalid music event after note attributes");
            MusicEvent event{ticks_,static_cast<std::uint8_t>(channel_index),value,{},0};
            if(value>=0xE0) {
                need(value<=0xFA,"Unsupported music command");
                event.parameter_count=counts[value-0xE0];
                for(unsigned i=0;i<event.parameter_count;++i) event.parameters.at(i)=read(channel.cursor);
                if(value==0xE0) channel.instrument=event.parameters[0];
                if(value==0xEA) channel.transpose=event.parameters[0];
                if(value==0xF1 || value==0xF2) {
                    channel.envelope_delay=event.parameters[0]; channel.envelope_length=event.parameters[1];
                }
                if(value==0xF3) channel.envelope_length=0;
                if(value==0xF9) {
                    channel.slide_delay=event.parameters[0]; channel.slide_remaining=event.parameters[1];
                }
                if(value==0xEF) {
                    channel.repeat_start=static_cast<std::uint16_t>(event.parameters[0]|event.parameters[1]<<8);
                    channel.repeat_return=channel.cursor; channel.repeat_count=event.parameters[2];
                    channel.cursor=channel.repeat_start;
                }
                events.push_back(event);
            } else {
                if(value>=0xCA) channel.instrument=value;
                if(value<0xC8 || value>=0xCA) {
                    channel.slide_remaining=channel.envelope_length;
                    channel.slide_delay=channel.envelope_delay;
                }
                channel.remaining=channel.duration; note=true;
                events.push_back(event);
            }
        }
        if(!new_section && playing_) { prefetch_slide(); ++channel_index; }
    }
    ++ticks_; return events;
}
}
