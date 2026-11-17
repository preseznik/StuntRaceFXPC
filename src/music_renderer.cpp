#include "srf/audio.hpp"
#include "srf/race_audio.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace srf {
namespace {
constexpr unsigned sample_rate=32000;
constexpr std::array<unsigned,32> envelope_period{
    0,2048,1536,1280,1024,768,640,512,384,320,256,192,160,128,96,80,
    64,48,40,32,24,20,16,12,10,8,6,5,4,3,2,1};
int signed_byte(unsigned x) { return x<128?int(x):int(x)-256; }
std::int16_t clamp_sample(int value) { return static_cast<std::int16_t>(std::clamp(value,-32768,32767)); }

// Recovered note-to-pitch arithmetic, original sound driver095D..09D0.
unsigned note_pitch(const SoundBank& bank,unsigned note,unsigned multiplier) {
    note&=65535;
    const auto key=note>>8;
    if(key>=52) note=(note+key-52)&65535;
    else if(key<19) note=(note+2*(int(key)-19))&65535;
    const unsigned octave=(note>>8)/12,semitone=(note>>8)%12;
    unsigned base=bank.pitch_table[semitone]+((bank.pitch_table[semitone+1]-bank.pitch_table[semitone])*(note&255)>>8);
    base*=2;
    if(octave<6) base>>=6-octave;
    else if(octave>6) base<<=std::min(octave-6,15u);
    return ((base*multiplier)>>8)&16383;
}

struct SampleVoice {
    const SoundSample* sample{};
    Instrument instrument{};
    BrrHistory history{};
    std::array<std::int16_t,16> block{};
    std::size_t block_offset=0;
    unsigned cursor=16,phase=0,pitch=0,envelope=0,envelope_counter=0,key_delay=0;
    enum class Stage { attack,decay,sustain,release } stage=Stage::release;
    std::int16_t current=0,next=0;
    int left=0,right=0;
    bool ended=true;

    std::int16_t source_sample() {
        if(!sample || ended)return 0;
        if(cursor==16) {
            if(block_offset==sample->brr.size()) {
                if(sample->loop_byte_offset==UINT32_MAX) {ended=true;return 0;}
                block_offset=sample->loop_byte_offset;
            }
            block=decode_brr_block(std::span<const std::uint8_t,9>(sample->brr.data()+block_offset,9),history);
            block_offset+=9;cursor=0;
        }
        return block[cursor++];
    }
    void start(const SoundBank& bank,unsigned index) {
        if(index>=bank.instruments.size())throw std::runtime_error("Music instrument outside extracted bank");
        start(bank,bank.instruments[index]);
    }
    void start(const SoundBank& bank,Instrument selected,const SoundSample* custom=nullptr) {
        instrument=selected;sample=custom;
        for(const auto& candidate:bank.samples) if(candidate.directory_index==instrument.sample)sample=&candidate;
        if(!sample)throw std::runtime_error("Music references an unloaded sample");
        history={};block_offset=0;cursor=16;phase=envelope=envelope_counter=0;
        stage=Stage::attack;ended=false;key_delay=5;
        current=source_sample();next=source_sample();
    }
    int render() {
        if(!sample)return 0;
        if(key_delay) {--key_delay;return 0;}
        if(stage==Stage::release) envelope=envelope>8?envelope-8:0;
        else if(instrument.adsr1&128) {
            unsigned rate=0;
            if(stage==Stage::attack)rate=(instrument.adsr1&15)*2+1;
            else if(stage==Stage::decay)rate=((instrument.adsr1>>4)&7)*2+16;
            else rate=instrument.adsr2&31;
            if(rate && ++envelope_counter>=envelope_period[rate]) {
                envelope_counter=0;
                if(stage==Stage::attack) {
                    envelope=std::min(2047u,envelope+(rate==31?1024:32));
                    if(envelope==2047)stage=Stage::decay;
                } else {
                    envelope-=((envelope-unsigned(envelope!=0))>>8)+unsigned(envelope!=0);
                    if(stage==Stage::decay && envelope<=(unsigned(instrument.adsr2>>5)+1)*256)stage=Stage::sustain;
                }
            }
        } else if(!(instrument.gain&128)) envelope=(instrument.gain&127)*16;
        else throw std::runtime_error("Dynamic gain envelope has not been recovered");
        const int value=(int(current)*(4096-int(phase))+int(next)*int(phase))/4096;
        phase+=pitch;
        while(phase>=4096) {phase-=4096;current=next;next=source_sample();}
        return value*int(envelope)/2048;
    }
};

// SPC DIV YA,X has defined overflow and divide-by-zero results. These occur
// deliberately in the original secondary engine's volume calculation.
unsigned divide_sound(unsigned value,unsigned divisor) {
    const unsigned high=value>>8;
    return (high<divisor*2?value/divisor:255-(value-divisor*512)/(256-divisor))&255;
}

struct EngineSound {
    unsigned port=0,previous=0,age=0,step=0,rpm=0,direction=0,overspeed=0;
    unsigned throttle_count=0,throttle_pitch=0,boost_count=0,boost_pitch=0;
    unsigned fade_delay=0,gain=255;
    bool enabled=false,throttle=false,boost=false,requested_throttle=false,requested_boost=false;
    std::array<SampleVoice,2> voices{};
    void input(const SoundBank& bank,unsigned next,bool gas,bool turbo,bool active) {
        port=next;
        requested_throttle=gas;requested_boost=turbo;
        if(active&&!enabled) {
            voices[0].start(bank,Instrument{28,0x8F,0xE0,0,256});
            voices[1].start(bank,Instrument{32,0x8F,0xE0,0,256});
        }
        enabled=active;
    }
    void tick(const SoundBank& bank,unsigned tunnel_gain) {
        if(!enabled)return;
        // Song 5 resets SPC 056A/056B. At 62.5 Hz the first counter reaches
        // its high bit, then the second fades in both engine voices.
        if(fade_delay)--fade_delay;
        else if(gain<255)++gain;
        auto ramp=[](bool pressed,unsigned& count,unsigned& pitch,unsigned maximum,unsigned amount) {
            if(pressed) {if(count!=maximum){++count;pitch=(pitch+amount)&65535;}}
            else if(count) {--count;pitch=(pitch-amount)&65535;}
            else pitch=0;
        };
        ramp(throttle,throttle_count,throttle_pitch,20,32);
        ramp(boost,boost_count,boost_pitch,25,60);
        // Original16F1 applies the previous flags before command18D6/18AC
        // latches this tick's edges. Reversing that order adds an extra ramp.
        if(requested_throttle!=throttle)throttle_pitch=requested_throttle?64:(throttle_pitch-64)&65535;
        if(requested_boost!=boost)boost_pitch=requested_boost?512:(boost_pitch-512)&65535;
        throttle=requested_throttle;boost=requested_boost;
        // Original dispatch order is secondary voice6, then main voice7.
        for(unsigned voice_index:{1u,0u}) {
            const auto& profile=bank.engine_profiles[voice_index];auto& voice=voices[voice_index];
            const auto sum=profile.volume_base+divide_sound(255-port,profile.volume_divisor);
            unsigned volume=sum&255;
            if(tunnel_gain<256)volume=volume*tunnel_gain>>8;
            if(boost)volume=(volume+3+unsigned(sum>255))&255;
            if(!throttle)volume=volume*208>>8;
            volume=volume*gain>>8;voice.left=voice.right=static_cast<int>(volume);
            if(port!=previous) {
                rpm=port<<8;
                const int delta=signed_byte((port-previous)&255);direction=delta<0?2:1;
                const unsigned magnitude=unsigned(std::abs(delta)),divisor=std::max(1u,age);
                step=divide_sound(magnitude<<8,divisor)+(magnitude>=divisor?256:0);age=0;
            } else {
                if(!port)step&=0xFF00; // Original0536 clear does not clear0537.
                if(port>=240)overspeed=(overspeed+3)&65535;else overspeed=0;
                if(((age-10)&255)<128)step=((step-1)&0x8000)?0:(step-1)&65535;
                age=std::min(age+1,255u);
                const unsigned base=age==1?port<<8:rpm;
                if(direction==2) {if(base>=step)rpm=base-step;}
                else if(base+step<=65535)rpm=base+step;
            }
            unsigned pitch=((unsigned(profile.pitch_base)<<8)+(rpm>>8)*profile.pitch_slope+
                            boost_pitch+throttle_pitch+overspeed)&65535;
            // Original2589 is SBC, not CMP; the wrapped high byte is audible.
            auto high=((pitch>>8)-63)&255;if(high<128)high=63;
            voice.pitch=((high<<8)|(pitch&255))&16383;previous=port;
        }
    }
};

struct EffectVoice {
    SampleVoice voice;
    SoundSample noise;
    Instrument instrument{0,0x8F,0xE0,0,256};
    unsigned id=0,cursor=0,remaining=0,age=0,maximum_age=0,pitch=0,mode=0,custom_mode=0,detune=0,pending_action=0;
    unsigned left=0,right=0,left_fraction=0,right_fraction=0;
    int pitch_step=0,left_step=0,right_step=0;
    bool hold=false,echo=false,pitch_slide=false,volume_slide=false;
};

struct EffectSounds {
    const SoundBank& bank;
    std::array<EffectVoice,8> channels{};
    unsigned engine_port=0;
    unsigned surface_variant=0;
    unsigned surface_counter=0;
    bool surface_pitch_alternate=false;
    explicit EffectSounds(const SoundBank& value):bank(value) {}
    unsigned byte(unsigned& cursor) const {
        if(cursor<0xBCA0 || cursor>=0xC735)throw std::runtime_error("Sound effect left extracted sequence data");
        return bank.effects.at(cursor++-0xBCA0);
    }
    unsigned word(unsigned& cursor) const {const auto low=byte(cursor);return low|(byte(cursor)<<8);}
    void start(unsigned id) {
        if(!id || id>bank.effect_pointers.size())throw std::runtime_error("Invalid sound effect ID");
        unsigned cursor=bank.effect_pointers[id-1];const auto channel=byte(cursor)&15;
        if(channel>=8)throw std::runtime_error("Invalid sound effect channel");
        auto& ch=channels[channel];ch=EffectVoice{};ch.id=id;ch.cursor=cursor;
    }
    void stop(unsigned id) {
        for(auto& ch:channels)if(ch.id==id){ch.id=0;ch.voice.stage=SampleVoice::Stage::release;}
    }
    void command(unsigned value) {
        // Original command table1804, separate from effect table0702.
        switch(value) {
        case 0x09:start(0x25);break;
        case 0x0A:stop(0x25);break;
        case 0x0B:stop(0x15);break;
        case 0x0C:stop(0x17);break;
        case 0x0D:start(0x15);break;
        case 0x0E:stop(0x21);break;
        case 0x12:start(0x17);break;
        case 0x13:start(0x21);break;
        case 0x23:case 0x24:break; // Boost ramp is set by the typed engine input.
        case 0x32:case 0x33:case 0x34:case 0x35:surface_variant=value-0x32;break;
        default:throw std::runtime_error("Unrecovered race sound command");
        }
    }
    unsigned volume(unsigned raw,unsigned mode) const {
        if(mode!=4)return raw;
        const unsigned scaled=(raw*(128+std::max(1u,engine_port)*3/2)>>8)&255;
        return scaled>=128?127:scaled;
    }
    void commands(unsigned channel,unsigned depth=0) {
        if(depth>8)throw std::runtime_error("Sound effect spawn cycle");
        auto& ch=channels[channel];
        for(unsigned budget=0;budget<256;++budget) {
            const unsigned command=byte(ch.cursor);
            if(!command)continue;
            if(command<128) {
                ch.remaining=command;ch.hold=command==127;
                if(ch.mode<2) {
                    if(ch.mode==0)ch.voice.stage=SampleVoice::Stage::release;
                    ch.voice.start(bank,ch.instrument,ch.instrument.sample==35?&ch.noise:nullptr);
                }
                ch.mode=0;return;
            }
            if(command<0xE0) {
                const auto key=command-128,octave=key/12;
                ch.pitch=bank.effect_pitch_table[key%12]>>(7-octave);
                if(ch.detune) {
                    unsigned extra;
                    if(ch.pitch>=4096)extra=(divide_sound(divide_sound(ch.pitch,67)*4,252)*ch.detune)&65535;
                    else extra=divide_sound(ch.detune,divide_sound(255,divide_sound(ch.pitch,17)));
                    ch.pitch=(ch.pitch+extra)&65535;
                }
                continue;
            }
            switch(command) {
            case 0xE0:if(!ch.hold){ch.id=0;ch.voice.stage=SampleVoice::Stage::release;}else {--ch.cursor;ch.remaining=1;}return;
            case 0xE1:ch.mode=1;break;
            case 0xE2:ch.mode=2;break;
            case 0xE3:ch.instrument.sample=static_cast<std::uint8_t>(byte(ch.cursor));break;
            case 0xE4:ch.left=volume(byte(ch.cursor),ch.custom_mode);ch.right=volume(byte(ch.cursor),ch.custom_mode);ch.left_fraction=ch.left<<8;ch.right_fraction=ch.right<<8;break;
            case 0xE5:ch.instrument.adsr1=(ch.instrument.adsr1&0xF0)|(byte(ch.cursor)&15)|128;break;
            case 0xE6:ch.instrument.adsr1=(ch.instrument.adsr1&0x8F)|((byte(ch.cursor)&7)<<4)|128;break;
            case 0xE7:ch.instrument.adsr2=(ch.instrument.adsr2&31)|((byte(ch.cursor)&7)<<5);break;
            case 0xE8:ch.instrument.adsr2=(ch.instrument.adsr2&0xE0)|(byte(ch.cursor)&31);break;
            case 0xE9:ch.pitch_step=static_cast<std::int16_t>(word(ch.cursor));ch.pitch_slide=true;break;
            case 0xEA:ch.echo=true;break;
            case 0xEB:ch.echo=false;break;
            case 0xEC: {
                const auto pointer=word(ch.cursor);
                const auto found=std::find(bank.effect_pointers.begin(),bank.effect_pointers.end(),pointer);
                if(found==bank.effect_pointers.end())throw std::runtime_error("Unknown child sound effect");
                unsigned child_cursor=pointer;const auto child_channel=byte(child_cursor)&15;
                if(child_channel==channel)throw std::runtime_error("Sound effect replaces its own command stream");
                start(unsigned(found-bank.effect_pointers.begin())+1);commands(child_channel,depth+1);break;
            }
            case 0xED:ch.left_step=static_cast<std::int16_t>(word(ch.cursor));ch.right_step=static_cast<std::int16_t>(word(ch.cursor));ch.volume_slide=true;break;
            case 0xEE:ch.detune=byte(ch.cursor);break;
            case 0xF1:ch.pitch_slide=false;break;
            case 0xF2:ch.volume_slide=false;break;
            case 0xF3:ch.cursor=word(ch.cursor);break;
            case 0xF8:ch.maximum_age=byte(ch.cursor);break;
            case 0xFB:ch.voice.stage=SampleVoice::Stage::release;ch.mode=2;break;
            case 0xFC:ch.custom_mode=byte(ch.cursor);break;
            case 0xFD: {
                const auto header=byte(ch.cursor)&254;
                ch.noise={35,0xFF00,0,{bank.effect_noise_seed.begin(),bank.effect_noise_seed.end()}};
                for(unsigned p=0;p<243;p+=9)ch.noise.brr[p]=static_cast<std::uint8_t>(header);
                ch.noise.brr[243]=static_cast<std::uint8_t>(header+1);break;
            }
            case 0xFE:ch.left=ch.right=volume(byte(ch.cursor),ch.custom_mode);ch.left_fraction=ch.left<<8;ch.right_fraction=ch.right<<8;break;
            default:throw std::runtime_error("Unrecovered sound effect command");
            }
        }
        throw std::runtime_error("Sound effect command budget exceeded");
    }
    void tick() {
        for(unsigned i=0;i<channels.size();++i) {
            auto& ch=channels[i];if(!ch.id)continue;
            if(ch.pending_action==2) {
                const auto id=ch.id;start(id);
            } else if(ch.pending_action==1) {
                ch.voice.start(bank,ch.instrument,ch.instrument.sample==35?&ch.noise:nullptr);
                ch.pending_action=0;
            }
            if(ch.maximum_age&&ch.age>=ch.maximum_age){ch.id=0;ch.voice.stage=SampleVoice::Stage::release;continue;}
            ++ch.age;
            if(!ch.remaining || !--ch.remaining)commands(i);
            else {
                if(ch.pitch_slide)ch.pitch=(ch.pitch+ch.pitch_step)&65535;
                if(ch.volume_slide) {
                    ch.left_fraction=(ch.left_fraction+ch.left_step)&65535;
                    ch.right_fraction=(ch.right_fraction+ch.right_step)&65535;
                    auto clamp=[](unsigned v){const auto high=(v>>8)&255;return high<128?high:(high>=192?0u:127u);};
                    ch.left=clamp(ch.left_fraction);ch.right=clamp(ch.right_fraction);
                }
            }
            ch.voice.instrument=ch.instrument;
            // Original261F..265B uses one shared speed-dependent counter.
            // Its request is consumed by the channel state machine next tick.
            if(ch.id==0x25 || ch.id==0x17 || ch.id==0x21) {
                const unsigned divisor=ch.id==0x25?30:ch.id==0x17?20:40;
                const unsigned base=ch.id==0x17?10:3;
                surface_counter=(surface_counter+1)&255;
                const unsigned limit=(255-engine_port)/divisor+base;
                if(((limit-surface_counter)&255)>=128) {
                    ch.pending_action=ch.id==0x25?1:2;surface_counter=1;
                    if(ch.id==0x21)surface_pitch_alternate=!surface_pitch_alternate;
                }
                if(ch.id==0x21)ch.pitch=surface_pitch_alternate?0x110:0x180;
            }
            ch.voice.pitch=(ch.pitch&0x8000)?0:std::min(ch.pitch,16383u);
            ch.voice.left=signed_byte(ch.left);ch.voice.right=signed_byte(ch.right);
            if(ch.id==0x25) {
                if(ch.left)ch.voice.left=signed_byte((ch.left+surface_variant*4)&255);
                if(ch.right)ch.voice.right=signed_byte((ch.right+surface_variant*4)&255);
            }
        }
    }
};
}

struct MusicRenderer::Impl {
    const SoundBank& bank;
    MusicSequence sequence;
    EngineSound engine;
    EffectSounds effects;
    bool music_enabled=true;
    MusicTrack following_music;
    unsigned effect_timer=0;
    struct Channel {
        SampleVoice voice;
        unsigned instrument=0,note=0,detune=0,pan=10,volume=255,gate_ticks=0;
        unsigned vibrato_delay=0,vibrato_rate=0,vibrato_depth=0,vibrato_age=0,vibrato_phase=0;
        unsigned slide_delay=0,slide_ticks=0,slide_target=0;
        int slide_step=0;
        unsigned envelope_delay=0,envelope_ticks=0,envelope_amount=0;
        bool envelope_from_note=false;
        unsigned volume_fraction=255U<<8,volume_ticks=0,volume_target=0;
        unsigned pan_fraction=10U<<8,pan_ticks=0,pan_target=0;
        int volume_step=0,pan_step=0;
        bool new_note=false;
    };
    std::array<Channel,8> channels{};
    unsigned tempo=32,tempo_fraction=0,timer_samples=0,master=192,transpose=0,drum_base=0;
    bool music_fading=false;
    unsigned fade_volume=0;
    unsigned echo_mask=0,echo_delay=1,echo_filter=0,echo_cursor=0,history_cursor=0;
    int echo_left=0,echo_right=0,echo_feedback=0;
    unsigned tunnel_mode=0,tunnel_amount=0,tunnel_stage=0,tunnel_gain=255;
    unsigned saved_echo_mask=0;
    int saved_echo_feedback=0,saved_echo_left=0,saved_echo_right=0;
    std::vector<std::array<int,2>> echo_buffer{512};
    std::array<std::array<int,2>,8> echo_history{};

    explicit Impl(const SoundBank& input):bank(input),sequence(input),effects(input) {}
    void sound_command(unsigned value) {
        // SPC17FE decrements the raw port command before indexing table1804.
        switch(value) {
        case 0x04:break; // Bonus tally exit; SPC180A points directly to RET at 18D5.
        case 0x05:music_fading=true;fade_volume=master<<8;break;
        case 0x0f:tunnel_mode=1;break; // SPC1951, full echo
        case 0x10:tunnel_mode=0;break; // SPC1956, restore song settings
        case 0x11:tunnel_mode=2;break; // SPC195B, half echo
        default:effects.command(value);break;
        }
    }
    void tunnel_tick() {
        // SPC15C4..16B0, called at62.5Hz before engine/effect updates. Preserve
        // its staged entry/exit (including a command reversal during a ramp).
        if(!tunnel_amount) {
            if(tunnel_stage==1) {
                saved_echo_mask=echo_mask;saved_echo_feedback=echo_feedback;
                saved_echo_left=echo_left;saved_echo_right=echo_right;tunnel_amount=1;
            } else if(tunnel_stage==3) {
                echo_mask=saved_echo_mask;echo_feedback=saved_echo_feedback;
                echo_left=saved_echo_left;echo_right=saved_echo_right;tunnel_stage=0;
            } else if(tunnel_mode)tunnel_stage=1;
            return;
        }
        echo_mask=0xc0;
        if(tunnel_stage==1) {
            ++tunnel_amount;
            if(tunnel_amount==60 || (tunnel_amount==30 && tunnel_mode!=1))tunnel_stage=2;
        } else if(tunnel_stage==2) {
            if(tunnel_mode==1 && tunnel_amount!=60)tunnel_stage=1;
            else if(!tunnel_mode || (tunnel_mode==2 && tunnel_amount!=30))tunnel_stage=3;
        } else if(--tunnel_amount==60 && tunnel_mode==2)tunnel_stage=2;
        tunnel_gain=255-((tunnel_amount*176)>>8);
        const auto blend=[&](int original) {
            const unsigned raw=static_cast<std::uint8_t>(original);
            return signed_byte((raw+((100-raw)&255)*tunnel_amount/60)&255);
        };
        echo_feedback=blend(saved_echo_feedback);
        // Original16AC/16AE writes the left calculation to both channels;
        // restoration above retains the separately saved right volume.
        echo_left=echo_right=blend(saved_echo_left);
    }
    unsigned instrument_index(unsigned raw) const {return raw>=0xCA?raw-0xCA+drum_base:raw;}
    void event(const MusicEvent& e) {
        auto& ch=channels[e.channel];const auto& p=e.parameters;
        if(e.command<0xE0) {
            if(e.command==0xC9) {ch.voice.stage=SampleVoice::Stage::release;return;}
            if(e.command==0xC8)return;
            unsigned note=e.command&127;
            if(e.command>=0xCA) {ch.instrument=e.command;note=36;}
            ch.voice.start(bank,instrument_index(ch.instrument));
            ch.note=(((note+transpose+sequence.channels()[e.channel].transpose)&255)<<8)|ch.detune;
            ch.vibrato_age=ch.vibrato_phase=0;ch.slide_ticks=0;ch.new_note=true;
            // SPC0935..0957: F2 approaches the new note from note-amount;
            // F1 starts at the note and approaches note+amount. Byte wrapping
            // makes Aqua's amount FF a one-semitone downward attack.
            if(ch.envelope_ticks) {
                auto start=ch.note>>8;
                if(!ch.envelope_from_note)start=(start-ch.envelope_amount)&255;
                const auto target=(start+ch.envelope_amount)&127;
                ch.note=(start<<8)|ch.detune;
                ch.slide_target=(target<<8)|ch.detune;
                ch.slide_delay=ch.envelope_delay;ch.slide_ticks=ch.envelope_ticks;
                ch.slide_step=(int(target)-int(start))*256/int(ch.slide_ticks);
            }
            const auto& sequence_ch=sequence.channels()[e.channel];
            ch.gate_ticks=std::max(1u,unsigned(sequence_ch.duration)*sequence_ch.gate>>8);
            return;
        }
        switch(e.command) {
        case 0xE0:ch.instrument=p[0];break;
        case 0xE1:ch.pan=p[0];ch.pan_fraction=(ch.pan&31)<<8;break;
        case 0xE2:
            // SPC0C25/0E6F: signed Q8.8 increment, exact target on final tick.
            ch.pan_ticks=p[0];ch.pan_target=p[1];
            ch.pan_step=ch.pan_ticks?(int(ch.pan_target)-int(ch.pan_fraction>>8))*256/int(ch.pan_ticks):0;
            break;
        case 0xE3:ch.vibrato_delay=p[0];ch.vibrato_rate=p[1];ch.vibrato_depth=p[2];break;
        case 0xE4:ch.vibrato_depth=0;break;
        case 0xE5:master=p[0];break;
        case 0xE7:tempo=p[0];break;
        case 0xE9:transpose=p[0];break;
        case 0xEA:break; // Transpose is retained by the native sequence.
        case 0xED:ch.volume=p[0];ch.volume_fraction=ch.volume<<8;break;
        case 0xEE:
            // SPC0CD0/0E1B uses the same fixed-point rule for channel volume.
            ch.volume_ticks=p[0];ch.volume_target=p[1];
            ch.volume_step=ch.volume_ticks?(int(ch.volume_target)-int(ch.volume))*256/int(ch.volume_ticks):0;
            break;
        case 0xEF:break; // Subpattern call is handled by the sequence.
        case 0xF1:case 0xF2:
            ch.envelope_delay=p[0];ch.envelope_ticks=p[1];ch.envelope_amount=p[2];
            ch.envelope_from_note=e.command==0xF1;break;
        case 0xF3:ch.envelope_ticks=0;break; // SPC0CC3 leaves an active slide intact.
        case 0xF4:ch.detune=p[0];break;
        case 0xF5:echo_mask=p[0];echo_left=signed_byte(p[1]);echo_right=signed_byte(p[2]);break;
        case 0xF6:echo_mask=0;echo_left=echo_right=0;break;
        case 0xF7:
            echo_delay=std::max(1u,unsigned(p[0]&15));echo_feedback=signed_byte(p[1]);echo_filter=p[2];
            if(echo_filter>=bank.echo_filters.size())throw std::runtime_error("Invalid music echo filter");
            echo_buffer.assign(echo_delay*512,{});echo_cursor=history_cursor=0;echo_history={};break;
        case 0xF9: {
            ch.slide_delay=p[0];ch.slide_ticks=p[1];
            const auto target=(p[2]+transpose+sequence.channels()[e.channel].transpose)&127;
            ch.slide_target=(target<<8)|ch.detune;
            ch.slide_step=ch.slide_ticks?(int(target)-int(ch.note>>8))*256/int(ch.slide_ticks):0;
            break;
        }
        case 0xFA:drum_base=p[0];break;
        default:throw std::runtime_error("Music renderer encountered an unrecovered effect");
        }
    }
    void start_track(MusicTrack track) {
        sequence.select_track(std::move(track));
        channels={};
        master=192;tempo=32;tempo_fraction=0;
        music_fading=false;
        transpose=0;drum_base=0;music_enabled=true;
    }
    void tick() {
        if(!sequence.playing() && !following_music.bytes.empty())start_track(std::move(following_music));
        for(auto& ch:channels) {
            ch.new_note=false;
            if(ch.gate_ticks && !--ch.gate_ticks)ch.voice.stage=SampleVoice::Stage::release;
            if(ch.slide_ticks) {
                if(ch.slide_delay)--ch.slide_delay;
                else if(!--ch.slide_ticks)ch.note=ch.slide_target;
                else ch.note=(ch.note+ch.slide_step)&65535;
            }
            if(ch.vibrato_age<ch.vibrato_delay)++ch.vibrato_age;
            else ch.vibrato_phase=(ch.vibrato_phase+ch.vibrato_rate)&255;
        }
        for(const auto& e:sequence.tick())event(e);
        // Original fades advance after the current tick's sequence commands.
        const auto fade=[](unsigned& value,unsigned& ticks,unsigned target,int step) {
            if(ticks)value=!--ticks?target<<8:(value+step)&65535;
        };
        for(auto& ch:channels) {
            fade(ch.volume_fraction,ch.volume_ticks,ch.volume_target,ch.volume_step);
            fade(ch.pan_fraction,ch.pan_ticks,ch.pan_target,ch.pan_step);
            ch.volume=ch.volume_fraction>>8;
        }
        update_voices();
    }
    void update_voices() {
        for(unsigned i=0;i<8;++i) {
            auto& ch=channels[i];if(!ch.voice.sample)continue;
            unsigned note=ch.note;
            if(ch.vibrato_age>=ch.vibrato_delay && !ch.new_note) {
                unsigned triangle=(ch.vibrato_phase<<2)&255;
                if(ch.vibrato_phase&64)triangle^=255;
                const unsigned amount=ch.vibrato_depth>=241?triangle*(ch.vibrato_depth&15):triangle*ch.vibrato_depth>>8;
                note=(note+((ch.vibrato_phase&128)?-int(amount):int(amount)))&65535;
            }
            ch.voice.pitch=note_pitch(bank,note,ch.voice.instrument.pitch_multiplier);
            unsigned amplitude=255*(music_fading?fade_volume>>8:master)>>8;
            amplitude=amplitude*sequence.channels()[i].volume>>8;
            amplitude=amplitude*ch.volume>>8;amplitude=amplitude*amplitude>>8;
            const auto pan_gain=[&](unsigned position) {
                const auto index=position>>8,fraction=position&255;
                const int first=bank.pan_table[index],next=bank.pan_table[index+1];
                return unsigned(first+((next-first)*int(fraction)>>8));
            };
            const unsigned pan=std::min(ch.pan_fraction,20U<<8);
            ch.voice.left=int(amplitude*pan_gain(pan)>>8)*((ch.pan&128)?-1:1);
            ch.voice.right=int(amplitude*pan_gain((20U<<8)-pan)>>8)*((ch.pan&64)?-1:1);
        }
    }
    std::array<std::int16_t,2> frame() {
        // Original08:089C runs tunnel control before music when both timers
        // fire together. A song F5 command may override it until the next tick.
        if(effect_timer+1==512)tunnel_tick();
        // Original timer0 at500Hz; its accumulator overflows at the song tempo.
        // Scheduling uses sample counts, independent of rendering and physics.
        if(++timer_samples==sample_rate/500) {
            timer_samples=0;tempo_fraction+=tempo;
            if(tempo_fraction>=256) {tempo_fraction-=256;if(music_enabled)tick();}
        }
        if(++effect_timer==512) {
            effect_timer=0;
            // SPC1AB0 subtracts 2.F0 from the song volume at 62.5 Hz during a bank change.
            if(music_fading) {
                fade_volume-=std::min(fade_volume,0x2f0u);
                update_voices();
            }
            // SPC25B2..25BB applies (0552 - 1) only in full reverb.
            engine.tick(bank,tunnel_mode==1?(tunnel_gain-1)&255:256);
            effects.engine_port=engine.port;effects.tick();
        }
        std::array<int,2> dry{},send{},wet{};
        for(unsigned i=0;i<8;++i) {
            auto& effect=effects.channels[i];
            auto* selected=&channels[i].voice;
            if(engine.enabled&&i>=6)selected=&engine.voices[7-i];
            if(effect.id||effect.voice.envelope)selected=&effect.voice;
            const auto value=selected->render();
            const std::array<int,2> voice{value*selected->left/128,value*selected->right/128};
            const bool echo=selected==&effect.voice?effect.echo:
                (selected==&channels[i].voice || tunnel_stage!=0) && (echo_mask&(1u<<i));
            for(unsigned side=0;side<2;++side) {dry[side]+=voice[side];if(echo)send[side]+=voice[side];}
        }
        echo_history[history_cursor]=echo_buffer[echo_cursor];
        for(unsigned i=0;i<8;++i)for(unsigned side=0;side<2;++side)
            wet[side]+=echo_history[(history_cursor+8-i)%8][side]*bank.echo_filters[echo_filter][i]/128;
        for(unsigned side=0;side<2;++side)echo_buffer[echo_cursor][side]=clamp_sample(send[side]+wet[side]*echo_feedback/128);
        echo_cursor=(echo_cursor+1)%echo_buffer.size();history_cursor=(history_cursor+1)%8;
        return {clamp_sample(dry[0]*96/128+wet[0]*echo_left/128),
                clamp_sample(dry[1]*96/128+wet[1]*echo_right/128)};
    }
};

MusicRenderer::MusicRenderer(const SoundBank& bank):impl_(std::make_unique<Impl>(bank)) {}
MusicRenderer::~MusicRenderer()=default;
void MusicRenderer::reset() {const auto& bank=impl_->bank;impl_=std::make_unique<Impl>(bank);}
void MusicRenderer::render(std::span<std::int16_t> output) {
    if(output.size()%2)throw std::invalid_argument("Stereo output must contain complete frames");
    for(std::size_t i=0;i<output.size();i+=2) {const auto frame=impl_->frame();output[i]=frame[0];output[i+1]=frame[1];}
}
std::uint32_t MusicRenderer::ticks() const {return impl_->sequence.ticks();}
void MusicRenderer::set_engine(std::uint8_t port,bool throttle,bool boost,bool enabled) {impl_->engine.input(impl_->bank,port,throttle,boost,enabled);}
void MusicRenderer::play_effect(std::uint8_t id) {impl_->effects.start(id);}
void MusicRenderer::play_race_effect(std::uint8_t port_code) {
    // CPU04:D656 port codes become sequence indices at SPC17A5.
    if(port_code<6 || port_code>127)throw std::runtime_error("Invalid race effect port command");
    play_effect(static_cast<std::uint8_t>(port_code-5));
}
void MusicRenderer::sound_command(std::uint8_t command) {impl_->sound_command(command);}
EchoState MusicRenderer::echo_state() const {
    return {impl_->echo_mask,impl_->tunnel_amount,impl_->tunnel_stage,
        impl_->echo_feedback,impl_->echo_left,impl_->echo_right};
}
void MusicRenderer::update_race(const RaceAudioOutput& state,bool throttle,bool boost,bool engine_enabled,bool music_enabled) {
    set_engine(state.engine_parameter,throttle,boost,engine_enabled);
    set_music_enabled(music_enabled);
    for(auto command:state.commands)sound_command(command);
    for(auto effect:state.effects)play_race_effect(effect);
}
void MusicRenderer::set_music_enabled(bool enabled) {
    if(impl_->music_enabled==enabled)return;
    impl_->music_enabled=enabled;impl_->tempo_fraction=0;
    if(!enabled)for(auto& ch:impl_->channels)ch.voice.stage=SampleVoice::Stage::release;
}
void MusicRenderer::play_music(MusicTrack track,bool fade_in_engine,MusicTrack following) {
    // SPC0A04 initializes the new arrangement. Engine and effect channels
    // continue to run independently across the loading-to-driving transition.
    impl_->following_music=std::move(following);
    impl_->start_track(std::move(track));
    if(fade_in_engine){impl_->engine.fade_delay=127;impl_->engine.gain=0;}
}
std::array<EffectChannelState,8> MusicRenderer::effect_states() const {
    std::array<EffectChannelState,8> result{};
    for(unsigned i=0;i<8;++i) {
        const auto& ch=impl_->effects.channels[i];result[i]={static_cast<std::uint16_t>(ch.cursor),
            static_cast<std::uint16_t>(ch.voice.pitch),static_cast<std::uint8_t>(ch.id),
            static_cast<std::uint8_t>(ch.remaining),static_cast<std::uint8_t>(ch.left),static_cast<std::uint8_t>(ch.right),
            ch.instrument.sample,ch.instrument.adsr1,ch.instrument.adsr2};
    }
    return result;
}
}
