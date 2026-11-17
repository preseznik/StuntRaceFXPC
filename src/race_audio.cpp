#include "srf/race_audio.hpp"
#include <algorithm>
#include <stdexcept>

namespace srf {
namespace {
bool negative(std::uint16_t value) { return (value&0x8000)!=0; }
bool less_by_sign(std::uint16_t a,std::uint16_t b) {
    return negative(static_cast<std::uint16_t>(a-b));
}
std::uint16_t magnitude(std::uint16_t value) {
    return negative(value)?static_cast<std::uint16_t>(0-value):value;
}
void loop(RaceAudioState& state,RaceAudioOutput& out,std::uint16_t mask,
          bool enabled,std::uint8_t start,std::uint8_t stop) {
    if(enabled==bool(state.loop_flags&mask))return;
    out.commands.push_back(enabled?start:stop);
    if(enabled)state.loop_flags|=mask;
    else state.loop_flags&=static_cast<std::uint16_t>(~mask);
}
void surface(RaceAudioState& state,RaceAudioOutput& out,std::uint8_t next) {
    if(next==state.surface_command)return;
    if(!next)out.commands.push_back(0x0a);
    else {
        if(!state.surface_command)out.commands.push_back(0x09);
        out.commands.push_back(next);
    }
    state.surface_command=next;
}
}
std::uint16_t filter_player_boost(const VehicleState& body,std::uint16_t controls) {
    return body.word(0x6a)?controls:static_cast<std::uint16_t>(controls&~0x400U);
}
std::uint16_t prepare_player_input(RaceAudioState& state,const VehicleState& body,
                                  std::uint16_t requested,bool prestart,RaceAudioOutput& out) {
    // Original08:C657 skips AI bodies;C73E holds countdown input;C754 holds
    // locked control. These exits occur before the empty-boost latch changes.
    if(prestart || (body.word(0x1c)&0x10) || (body.word(0x42)&0x1000))return body.word(0x42);
    std::uint16_t controls=body.word(0x42)&0xd800;
    if(requested&1)controls|=1;
    else if(requested&2)controls|=2;
    if(requested&0x60) {
        controls|=0x2040;
        if(less_by_sign(body.word(0x4a),50))controls|=0x20;
    }
    if(requested&0x10)controls|=0x10;
    const bool empty_boost=(requested&0x400) && !body.word(0x6a);
    if(empty_boost) {
        if(!(state.loop_flags&0x10))out.effects.push_back(0x5e);
        state.loop_flags|=0x10;
    } else state.loop_flags&=0xffef;
    if((requested&0x400) && body.word(0x6a))controls|=0x400;
    return controls;
}
RaceAudioTuning extract_race_audio_tuning(std::span<const std::uint8_t> rom,
                                        std::uint16_t definition) {
    // Original0B:8A24 selects the same five table offsets.
    std::size_t offset=0;
    switch(definition) {
    case 0x853d:offset=0x98;break;
    case 0x8411:offset=0x130;break;
    case 0x82e5:offset=0x1c8;break;
    case 0x8669:offset=0x260;break;
    default:break;
    }
    const auto start=0x5b158+offset;
    RaceAudioTuning tuning;
    if(rom.size()<start+tuning.speed_curve.size())throw std::invalid_argument("ROM engine curve is truncated");
    std::copy_n(rom.begin()+start,tuning.speed_curve.size(),tuning.speed_curve.begin());
    return tuning;
}
std::uint8_t update_engine_audio(RaceAudioState& state,const RaceAudioTuning& tuning,
                                const VehicleState& body,const RaceAudioInput& input) {
    // Original0B:89AC, relocated7F:11AC. One increment per game phase.
    if(input.prestart && input.race_mode<6) {
        if(input.throttle)state.rev=std::min<std::uint16_t>(static_cast<std::uint16_t>(state.rev+40),255);
        else if(state.rev) {
            state.rev=static_cast<std::uint16_t>(state.rev-12);
            if(negative(state.rev))state.rev=0;
        }
        return static_cast<std::uint8_t>(state.rev);
    }
    auto speed=body.word(0x4a);
    if(negative(speed))speed=static_cast<std::uint16_t>(magnitude(speed)>>1);
    auto target=std::uint16_t(tuning.speed_curve[std::min<std::uint16_t>(speed,599)>>2]);
    // Original0B:8977. Preserve its asymmetric BPL decision, including the
    // below-target branch that can disable smoothing immediately.
    if(!input.prestart && input.race_mode<6 && !negative(state.rev)) {
        if(state.rev==target)state.rev=0xff00;
        else {
            state.rev=static_cast<std::uint16_t>(state.rev+(state.rev>target?-8:4));
            if(!less_by_sign(state.rev,target))target=state.rev;
            else state.rev=0xff00;
        }
    }
    return static_cast<std::uint8_t>(target);
}
void update_player_audio_events(RaceAudioState& state,const Vehicle& vehicle,
                               const RaceAudioInput& input,RaceAudioOutput& out) {
    const auto& body=vehicle.body;
    if(input.cpu_flags&0x1000)return; // Original08:C12F.
    if(!(body.word(0x40)&0x0100)) {
        if(input.terrain_flags&0x20) {
            bool wheel_contact=false;
            for(const auto& wheel:vehicle.wheels)
                wheel_contact|=(wheel.word(0x2c)&0x80) && ((wheel.word(0x46)&0x11)==0x11);
            const auto speed=magnitude(body.word(0x4a));
            if(wheel_contact && !less_by_sign(speed,40) &&
               !(input.counter&(less_by_sign(speed,160)?3:1)))out.effects.push_back(0x23);
        } else loop(state,out,4,(body.word(0x92)&0x20)!=0,0x12,0x0c);
    }
    loop(state,out,1,(body.word(0x42)&0x0400)!=0,0x23,0x24);
    loop(state,out,2,(body.word(0x42)&0x2000) && (body.word(0x40)&0x0800) &&
         !(body.word(0x40)&0x0100),0x0d,0x0b);
    const auto contact=static_cast<std::uint8_t>(body.word(0x56)>>8);
    if(contact<100)surface(state,out,contact==0?0:contact==1?0x32:contact<4?0x33:contact==4?0x34:0x35);
    const auto impact=body.word(0x94);
    if(impact>=50 && impact<2000)
        out.effects.push_back(impact<500?0x54:impact<800?0x55:impact<1100?0x56:0x57);
    if(body.word(0x2c)&0x0100)out.effects.push_back(0x0a);
    else if(body.word(0x2c)&0x0200)out.effects.push_back(0x0d);
    if(negative(body.word(0x62))) {
        const auto landing=magnitude(body.word(0x62));
        if(landing>=65 && landing<1000)
            out.effects.push_back(landing<80?0x16:landing<90?0x17:landing<110?0x18:0x19);
    }
    if(!(input.cpu_flags&0x40) && !less_by_sign(body.word(0x82),409) && !(state.loop_flags&0x20)) {
        out.effects.push_back(0x3d);
        state.loop_flags|=0x20;
    }
}
RaceAudioOutput update_race_audio(RaceAudioState& state,const RaceAudioTuning& tuning,
                                 const Vehicle& vehicle,const RaceAudioInput& input) {
    RaceAudioOutput out;
    update_player_audio_events(state,vehicle,input,out);
    out.engine_parameter=update_engine_audio(state,tuning,vehicle.body,input);
    return out;
}
void append_countdown_audio(std::uint8_t previous,std::uint8_t stage,RaceAudioOutput& out) {
    if(stage<=previous)return;
    for(unsigned next=previous+1;next<=stage;++next) {
        if(next>=3 && next<=5)out.effects.push_back(0x49);
        if(next==6)out.effects.push_back(0x4a);
    }
}
}
