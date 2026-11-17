#include "srf/vehicle.hpp"
#include "srf/race.hpp"
#include "srf/body_collision.hpp"
#include "srf/vehicle_math.hpp"
#include "srf/scene.hpp"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace srf {
namespace {
using Word = std::uint16_t;
int signed_word(Word value) { return value >= 0x8000 ? int(value) - 0x10000 : int(value); }
Word wrap(int value) { return static_cast<Word>(value); }
Word add(Word a, Word b) { return wrap(int(a) + b); }
Word sub(Word a, Word b) { return wrap(int(a) - b); }
Word negate(Word x) { return wrap(-int(x)); }
Word absolute(Word x) { return signed_word(x) < 0 ? negate(x) : x; }
Word div2(Word x) {
    const int n = signed_word(x);
    return wrap(n == -1 ? 0 : n < 0 ? (n - 1) / 2 : n / 2);
}
Word divide(Word x, unsigned count) { while (count--) x = div2(x); return x; }
Word high_product(Word a, Word b) {
    const std::int32_t product = signed_word(a) * signed_word(b);
    return static_cast<Word>(static_cast<std::uint32_t>(product) >> 16);
}
bool negative_difference(Word a, Word b) { return signed_word(sub(a,b)) < 0; }
Word product_q15(Word a, Word b) {
    const std::int32_t product=signed_word(a)*signed_word(b);
    return static_cast<Word>(static_cast<std::uint32_t>(product)>>15);
}
}

VehicleTuning extract_vehicle_tuning(std::span<const std::uint8_t> rom,Word parameter_address) {
    if (rom.size() != 0x100000) throw std::invalid_argument("Expected verified headerless USA Rev 1 ROM");
    const auto word = [&](std::size_t address) { return Word(rom[address] | (unsigned(rom[address+1]) << 8)); };
    if(parameter_address<0x8000 || parameter_address>0xffcc)throw std::invalid_argument("Invalid original vehicle parameter address");
    const std::size_t parameter_offset=0x48000+(parameter_address&0x7fff);
    VehicleTuning result;
    for (std::size_t i=0; i<result.contact_parameters.size(); ++i) result.contact_parameters[i] = word(parameter_offset + i*2);
    for (std::size_t i=0; i<result.words.size(); ++i) result.words[i] = word(parameter_offset + 6 + i*2);
    for (std::size_t i=0; i<result.sine.size(); ++i) result.sine[i] = word(0x183e + i*2);
    const std::size_t limits = 0x48000 + (result.at(0x267a) & 0x7fff);
    const std::size_t curves = 0x48000 + (result.at(0x267c) & 0x7fff);
    for (std::size_t i=0; i<5; ++i) {
        result.gear_limits[i] = word(limits+i*2);
    }
    for (std::size_t i=0; i<6; ++i) {
        for (std::size_t j=0; j<17; ++j) result.gear_curves[i][j] = rom[curves-17+i*17+j];
    }
    if (word(limits+10) != 0 && result.gear_limits.back()!=0) {
        result.sixth_gear_limit=word(limits+10);
        if(word(limits+12)!=0)throw std::runtime_error("Unexpected vehicle gear table terminator");
        for(std::size_t j=0;j<17;++j)result.sixth_gear_curve[j]=rom[curves+5*17+j];
    }
    return result;
}

VehicleTuning extract_4wd_tuning(std::span<const std::uint8_t> rom) { return extract_vehicle_tuning(rom,0x8486); }

std::array<Word,9> vehicle_orientation(const VehicleState& state,const VehicleTuning& tuning) {
    const auto sine=[&](Word angle) {
        const unsigned index=angle>>8;
        const Word base=tuning.sine[index];
        return add(base,product_q15(sub(tuning.sine[index+1],base),Word((angle&255)<<7)));
    };
    const Word pitch=negate(state.word(0x26)),yaw=negate(state.word(0x24)),roll=negate(state.word(0x28));
    const Word sp=sine(pitch),cp=sine(add(pitch,0x4000));
    const Word sy=sine(yaw),cy=sine(add(yaw,0x4000));
    const Word sr=sine(roll),cr=sine(add(roll,0x4000));
    const Word sycr=product_q15(sy,cr),cycr=product_q15(cy,cr);
    const Word sysr=product_q15(sy,sr),cysr=product_q15(cy,sr);
    // Original04:C1A7 constructs the matrix;04:C2C8 transposes it in place.
    return {add(product_q15(sysr,sp),cycr),product_q15(sr,cp),sub(product_q15(cysr,sp),sycr),
            sub(product_q15(sycr,sp),cysr),product_q15(cr,cp),add(product_q15(cycr,sp),sysr),
            product_q15(sy,cp),negate(sp),product_q15(cy,cp)};
}

static Vehicle create_chassis(std::span<const std::uint8_t> rom,Word definition,const std::array<std::int16_t,3>& position,
                             Word yaw,Word kind,Word identifier,Word surface_options,VehicleState* guide) {
    if(rom.size()!=0x100000 || definition<0x8000 || definition>0xff00)throw std::invalid_argument("Invalid original vehicle definition");
    const std::size_t base=0x48000+(definition&0x7fff);
    const auto field=[&](std::size_t offset) { return Word(rom[base+offset] | (unsigned(rom[base+offset+1])<<8)); };
    const auto attachment_count=rom[base+0x25];
    if(attachment_count!=(guide?5:4))
        throw std::runtime_error("Unsupported original vehicle attachment count");
    const auto visual_attachments=0x28u+14u*attachment_count;
    const Word parameters=Word(definition+visual_attachments+1+rom[base+visual_attachments]*5);
    const auto tuning=extract_vehicle_tuning(rom,parameters);
    Vehicle vehicle;
    auto& body=vehicle.body;
    for(unsigned axis=0;axis<3;++axis) {
        body.word(0x0c+axis*2)=field(0x11+axis*2);
        body.word(0x1e + axis*2)=wrap(position[axis]);
    }
    body.word(0x02)=0x98;
    body.word(0x16)=field(0x0b);
    body.word(0x1a)=field(0x0f);
    body.word(0x1c)=4;
    body.word(0x24)=yaw;
    body.word(0x2a)=field(0x21);
    body.word(0x40)=field(0x0d);
    body.word(0x5a)=field(7);
    body.word(0x5c)=field(9);
    body.word(0x64)=kind;
    body.word(0x66)=definition;
    body.word(0x6a)=384;
    body.word(0x6e)=parameters;
    body.word(0x58)=1;
    body.word(0x86)=2;
    body.word(0x8a)=surface_options;
    body.word(0x8c)=4096;
    body.word(0x14)=field(0x23);
    body.word(0x96)=identifier;
    const auto matrix=vehicle_orientation(body,tuning);
    // Original starting-grid linked-list order observed with the4WD definition.
    constexpr std::array<unsigned,4> order{3,2,1,0};
    for(unsigned i=0;i<attachment_count;++i) {
        auto& wheel=i==4?*guide:vehicle.wheels[i];
        const std::size_t row=0x28+(i==4?4:order[i])*14;
        wheel.word(0x02)=0x52;
        for(unsigned axis=0;axis<3;++axis) {
            wheel.word(0x0c+axis*2)=20;
            wheel.word(0x3c+axis*2)=field(row+2+axis*2);
        }
        wheel.word(0x3c)=negate(wheel.word(0x3c));
        wheel.word(0x12)=field(row);
        wheel.word(0x16)=4;
        wheel.word(0x1a)=field(0x26);
        wheel.word(0x1c)=4;
        wheel.word(0x2a)=body.word(0x2a);
        wheel.word(0x44)=field(row+8);
        wheel.word(0x46)=field(row+12);
        wheel.word(0x50)=field(row+10);
        position_vehicle_attachment(body,wheel,matrix);
    }
    return vehicle;
}

Vehicle create_vehicle(std::span<const std::uint8_t> rom,Word definition,const std::array<std::int16_t,3>& position,
                       Word yaw,Word kind,Word identifier,Word surface_options) {
    return create_chassis(rom,definition,position,yaw,kind,identifier,surface_options,nullptr);
}

Vehicle create_bonus_trailer(std::span<const std::uint8_t> rom,const std::array<std::int16_t,3>& position,
                             Word yaw,VehicleState& guide) {
    return create_chassis(rom,0x8669,position,yaw,1,1,0x0208,&guide);
}

Vehicle create_4wd(std::span<const std::uint8_t> rom,const std::array<std::int16_t,3>& position,Word yaw) {
    return create_vehicle(rom,0x8411,position,yaw,1,1,0x0108);
}

void begin_vehicle_forces(VehicleState& state,const VehicleTuning& tuning,VehicleForces& forces,
                          bool advance_timing) {
    forces.words.fill(0);
    forces.impulses.fill(0);
    forces.words[1]=add(120,absolute(high_product(state.word(0x4a),tuning.at(0x2670))));
    if (!(state.word(0x1c)&8)) forces.words[1]=add(forces.words[1],60);
    if(advance_timing)state.word(0x40)&=Word(~0x086a);
    state.word(0x40)|=0x0100; // Support is re-evaluated at every physical step.
}

void update_vehicle_controls(VehicleState& state,const VehicleTuning& tuning,
                             VehicleForces& forces,const VehicleDriveContext& context,
                             std::optional<std::int16_t> steering_q15) {
    Word control=state.word(0x42);
    // Set direction before the original suppression gates. A disabled wheel or
    // recovery state must not regain steering through the proportional path.
    if(context.advance_timing && steering_q15 && !(state.word(0x64)&8) && !(state.word(0x1c)&0x10) && !(control&0x1000)) {
        control=Word(control&~3U);
        if(*steering_q15<0)control|=1;
        else if(*steering_q15>0)control|=2;
    } else steering_q15.reset();
    const Word surface=state.word(0x8a);
    forces.contact_friction={16000,18000};
    if (state.word(0x64)&8) forces.contact_friction=state.word(0x7c) ? std::array<Word,2>{16000,16000} : std::array<Word,2>{32000,32000};
    else if (!(state.word(0x40)&0x10)) {
        if (surface&4) forces.contact_friction={17500,17700};
        else if (surface&1) forces.contact_friction={16000,16500};
        else if (surface&0x42) forces.contact_friction={17000,15000};
    }
    if(context.advance_timing) {
        const Word speed_limit=(control&0x0400) ? 330 : 240;
        if (!(state.word(0x40)&0x2000) && ((surface&0x80) || (!(state.word(0x40)&0x10) && state.word(0x46)))) {
            if (!negative_difference(absolute(state.word(0x4a)),speed_limit)) control=Word((control|0x40)&~0x0430);
        }
        if (!negative_difference(state.word(0x44),3)) { control&=Word(~0x0430);state.word(0x40)|=8; }
        if (state.word(0x54) || (state.word(0x40)&4)) control&=Word(~0x043f);
        if (state.word(0x5e)) --state.word(0x5e);
        else if ((control&0x80) && tuning.at(0x2672)) {
            forces.words[1]=add(forces.words[1],tuning.at(0x2672));state.word(0x5e)=32;
            forces.impulses[1]=add(forces.impulses[1],tuning.at(0x2672));
        }
        Word steering=vehicle_steering(state,tuning,control,steering_q15);
        if (state.word(0x70)&8) steering=add(steering,steering);
        state.word(0x3a)=steering;
    }
    vehicle_drivetrain(state,tuning,context,control);
    if (control&0x0800) {
        if(!context.loading_contact_y)throw std::runtime_error("Loading impulse requires trailer contact");
        // 04:B57B..B5AD adds local (0,0,600), rotates at 97BB, and
        // transfers at the center (987D). The prior contact in 2342 survives.
        const auto orientation=vehicle_orientation(state,tuning);
        for(unsigned axis=0;axis<3;++axis) {
            auto force=product_q15(600,orientation[6+axis]);
            if(axis==1)force=add(force,wrap(*context.loading_contact_y));
            const auto impulse=high_product(0x7fff,div2(force));
            forces.words[axis]=add(forces.words[axis],impulse);
            forces.impulses[axis]=add(forces.impulses[axis],impulse);
        }
    }
    Word braking=0;
    if (!(control&0x0410)) {
        Word speed=absolute(wrap(int(state.word(0x4a))*8));
        braking=high_product(add(speed,speed),tuning.at(0x2660));
        if (negative_difference(braking,0x0800)) braking=0x0800;
    }
    if ((control&0x40) && signed_word(state.word(0x4a))>=0) {
        if (negative_difference(200,state.word(0x4a))) state.word(0x40)|=0x0800;
        braking=tuning.at(0x265e);
    }
    state.word(0x48)=braking;
    state.word(0x42)=control;
    state.word(0x2c)=64;
}

Word vehicle_steering(const VehicleState& state, const VehicleTuning& tuning, Word control,
                      std::optional<std::int16_t> steering_q15) {
    // Proportional input is a modern target within the original steering range,
    // not an overwrite after forces have consumed the steering state. Full stick
    // and neutral use the original function unchanged, including special neutral.
    if(steering_q15 && !(control&12)) {
        const int request=std::clamp(int(*steering_q15),-32767,32767);
        const int magnitude=std::abs(request);
        const Word direction=request<0 ? 1 : request>0 ? 2 : 0;
        if(magnitude && magnitude<32767 && (control&3)==direction) {
            const int limit=tuning.at(0x2666);
            const int target=limit*magnitude/32767;
            auto target_state=state;
            // +70 bit8 doubles the stored result after steering. Partial targets
            // operate in the pre-doubling range; reusing the doubled result here
            // would grow instead of converging when the stick is reduced.
            if(state.word(0x70)&8)target_state.word(0x3a)=div2(target_state.word(0x3a));
            const int previous=signed_word(target_state.word(0x3a));
            const int sign=request<0 ? 1 : -1; // Original left steering is positive.
            if(previous*sign>target) {
                // Reducing a nonzero target uses the original centering amount,
                // stopping at the requested magnitude instead of passing it.
                return wrap(sign*std::min(limit,std::max(target,std::abs(previous)-int(tuning.at(0x266a)))));
            }
            const Word candidate=vehicle_steering(target_state,tuning,control);
            if(signed_word(candidate)*sign>target)return wrap(sign*target);
            return candidate;
        }
    }
    Word initial = tuning.at(0x2662);
    const bool special = (state.word(0x40) & 0x0400) != 0;
    if (special) {
        Word speed = absolute(state.word(0x4a));
        if (!negative_difference(speed,127)) speed = 127;
        initial = high_product(add(initial,initial), Word((speed << 8) | (speed >> 8)));
    }
    Word steering = state.word(0x3a);
    if (!(control & 15)) {
        if (special || !steering) return steering;
        const Word centering = tuning.at(0x266a);
        if (signed_word(steering) < 0) {
            steering = add(steering,centering);
            if (signed_word(steering) >= 0) steering = 0;
        } else {
            steering = sub(steering,centering);
            if (signed_word(steering) < 0) steering = 0;
        }
        return steering;
    }
    Word limit = 0;
    if (control & 3) {
        limit = tuning.at(0x2666);
        if (control & 1) {
            if (signed_word(steering) < 0 && !special) steering = 0;
            steering = add(steering,steering ? 128 : initial);
        } else if (control & 2) {
            if (signed_word(steering) >= 0 && !special) steering = 0;
            steering = sub(steering,steering ? 128 : initial);
        }
    }
    if (control & 12) {
        limit = add(limit,tuning.at(0x2668));
        if (control & 4) steering = add(steering,tuning.at(0x2664));
        if (control & 8) steering = sub(steering,tuning.at(0x2664));
    }
    if (signed_word(steering) >= 0) {
        if (!negative_difference(steering,limit)) steering = limit;
    } else if (negative_difference(steering,negate(limit))) steering = negate(limit);
    return steering;
}

void vehicle_drivetrain(VehicleState& state, const VehicleTuning& tuning,
                        const VehicleDriveContext& context, Word control) {
    // +78 is the pre-curve demand selected by the last control tick. In
    // particular, spending the last boost unit still boosts this interval.
    Word drive = state.word(0x78);
    if(context.advance_timing) {
        drive = (control & 0x20) ? negate(tuning.at(0x265c)) : 0;
        if (control & 0x0400) {
            drive = tuning.at(0x2658);
            Word reserve = state.word(0x6a);
            if (reserve) {
                if (!(state.word(0x40) & 0x2000)) {
                    reserve = sub(reserve,tuning.at(0x267e));
                    if (signed_word(reserve) < 0) reserve = 0;
                    if (!(context.boost_options & 0x0200)) state.word(0x6a) = reserve;
                }
                drive = tuning.at(0x265a);
            }
        } else if (control & 0x10) drive = tuning.at(0x2658);
        if (state.word(0x40) & 0x2000) drive = add(add(drive,drive),add(drive,drive));
        state.word(0x78) = drive;
    }

    const Word speed = state.word(0x4a);
    Word selected_gear = 1, lower_limit = 0;
    const bool reverse_accelerating = signed_word(speed) < 0 && (control & 0x0410);
    Word bucket = 15;
    if (!reverse_accelerating) {
        if (signed_word(speed) < 0) selected_gear = 0;
        else {
            const auto gears=tuning.gear_limits.size()+(tuning.sixth_gear_limit?1:0);
            for (std::size_t i=0; i<gears; ++i) {
                const auto limit=i<tuning.gear_limits.size()?tuning.gear_limits[i]:tuning.sixth_gear_limit;
                if(!limit) { --selected_gear;break; }
                if (!negative_difference(limit,speed)) break;
                lower_limit = limit;
                if (i+1<gears) ++selected_gear;
            }
        }
        if (context.advance_timing && !(context.update_phase & 3) && state.word(0x4e) != selected_gear) {
            state.word(0x4e) = selected_gear;
            if (selected_gear == 4) state.word(0x40) |= 2;
        }
        const Word gear = state.word(0x4e);
        if (gear>6 || (gear==6&&!tuning.sixth_gear_limit)) throw std::runtime_error("Invalid original gear index");
        const auto& curve = gear==6?tuning.sixth_gear_curve:tuning.gear_curves[gear];
        const Word factor = Word(unsigned(curve[0]) << 8);
        bucket = high_product(add(sub(absolute(speed),lower_limit),sub(absolute(speed),lower_limit)),factor);
        if (!negative_difference(bucket,15)) bucket=15;
        if (bucket>15) throw std::runtime_error("Invalid original gear curve index");
        drive = high_product(add(drive,drive),Word(unsigned(curve[bucket+1]) << 8));
    }
    state.word(0x3c) = drive;
    const Word target_engine = wrap(int(bucket)*64);
    const Word previous_engine = state.word(0x50);
    if(context.advance_timing)state.word(0x50) = add(previous_engine,divide(sub(target_engine,previous_engine),2));
}

void integrate_vehicle_pose(VehicleState& state, bool include_angles) {
    const bool half_step = (state.word(0x1c) & 8) != 0;
    for (unsigned axis=0;axis<3;++axis)
        state.word(0x1e + axis*2) = add(state.word(0x1e + axis*2),divide(state.word(0x2e + axis*2),half_step ? 4 : 3));
    if (include_angles) for (unsigned axis=0;axis<3;++axis)
        state.word(0x24 + axis*2) = add(state.word(0x24 + axis*2),divide(state.word(0x34 + axis*2),half_step ? 1 : 0));
}

void position_vehicle_attachment(const VehicleState& body, VehicleState& attachment,
                                 const std::array<Word,9>& orientation) {
    if (attachment.word(0x46)&0x0400) {
        for (unsigned axis=0;axis<3;++axis) attachment.word(0x2e + axis*2)=0;
        return;
    }
    const std::array<Word,3> local{attachment.word(0x3c),attachment.word(0x3e),attachment.word(0x40)};
    for (unsigned axis=0;axis<3;++axis) {
        Word offset=0;
        for(unsigned column=0;column<3;++column)
            offset=add(offset,product_q15(local[column],orientation[column*3+axis]));
        attachment.word(0x36+axis*2)=offset;
        const Word position=add(body.word(0x1e + axis*2),offset);
        attachment.word(0x2e + axis*2)=wrap(signed_word(sub(position,attachment.word(0x1e + axis*2)))*8);
        attachment.word(0x1e + axis*2)=position;
    }
}

std::array<Word,3> normalize_vehicle_vector(const std::array<Word,3>& vector) {
    std::uint32_t sum=0;
    for(unsigned axis=0;axis<3;++axis) {
        const int component=signed_word(vector[axis]);
        sum+=static_cast<std::uint32_t>(component*component);
    }
    // Original04:C10F restoring integer square root, expressed without registers.
    std::uint32_t length=0,remainder=sum,bit=1u<<30;
    while(bit>remainder)bit>>=2;
    while(bit) {
        if(remainder>=length+bit){remainder-=length+bit;length=(length>>1)+bit;}
        else length>>=1;
        bit>>=2;
    }
    std::array<Word,3> normalized{};
    if (length) {
        unsigned shift=0;
        for(auto n=length;n>>=1;)++shift;
        std::uint32_t numerator=0x7fffu<<shift,denominator=length;
        if(denominator&0x8000){numerator>>=1;denominator>>=1;}
        const Word reciprocal=Word(numerator/denominator);
        for(unsigned axis=0;axis<3;++axis) {
            const int product=signed_word(vector[axis])*signed_word(reciprocal);
            const int divisor=1<<shift;
            normalized[axis]=wrap(product<0 ? -int((std::uint32_t(-product)+divisor-1)/divisor) : product/divisor);
        }
    }
    return normalized;
}

void update_vehicle_alignment(VehicleState& state,const std::array<Word,9>& orientation) {
    const auto normalized=normalize_vehicle_vector({state.word(0x2e),state.word(0x30),state.word(0x32)});
    std::uint32_t projection=0;
    for(unsigned axis=0;axis<3;++axis)
        projection+=static_cast<std::uint32_t>(signed_word(normalized[axis])*signed_word(orientation[6+axis]));
    Word result=Word(projection>>15);
    if(negative_difference(absolute(state.word(0x4a)),10))result=0x7fff;
    state.word(0x6c)=result;
}

void update_vehicle_contact_state(VehicleState& state,const VehicleTuning& tuning,VehicleForces& forces) {
    if (!(state.word(0x40)&0x0100)) state.word(0x60)=state.word(0x20);
    if (negative_difference(state.word(0x60),state.word(0x20))) state.word(0x60)=0;
    if ((state.word(0x42)&0x0400) && tuning.at(0x2682) && negative_difference(state.word(0x4e),2)
        && !negative_difference(state.word(0x26),0xeaab))
        forces.words[4]=sub(forces.words[4],tuning.at(0x2682));
}

bool update_vehicle_damage(VehicleState& state,const VehicleTuning& tuning,
                           const VehicleForces& forces,VehicleDriveContext& context) {
    // Consume the raw force sample once per original callback. Repeating a
    // held mode1 would repeatedly add damage and emit the same crash event.
    if(!context.advance_timing)return false;
    if (state.word(0x64)&8) return false;
    if ((state.word(0x40)&0x2201) || (state.word(0x8a)&0x0200)) {
        state.word(0x62)=0;return false;
    }
    state.word(0x62)=divide(forces.words[1],2);
    Word damage=absolute(state.word(0x62));
    if (negative_difference(state.word(0x44),2)) damage=divide(damage,2);
    if (signed_word(state.word(0x30))<0) damage=0;
    damage=add(damage,divide(absolute(forces.words[0]),3));
    damage=add(damage,divide(absolute(forces.words[2]),3));
    damage=high_product(damage,tuning.at(0x2678));
    if (state.word(0x18)) damage=div2(damage);
    if (!context.damage_mode) context.damage_accumulator=0;
    context.damage_accumulator=add(context.damage_accumulator,damage);
    if (!context.damage_mode) return false;
    damage=div2(context.damage_accumulator);
    if (negative_difference(damage,16)) return false;
    if (!negative_difference(damage,96)) damage=96;
    state.word(0x82)=add(state.word(0x82),damage);
    if (!negative_difference(damage,24)) {
        state.word(0x70)|=0x80;
        state.word(0x40)|=negative_difference(damage,56) ? 0x40 : 0x20;
    } else state.word(0x40)|=0x80;
    const bool nearby_impulse=(state.word(0x40)&0x20)!=0;
    if (!negative_difference(state.word(0x82),512)) state.word(0x40)|=1;
    return nearby_impulse;
}

void update_vehicle_recovery(VehicleState& state,VehicleForces& forces,
                             std::optional<Word> desired_yaw,bool advance_motion_rules) {
    if(!(state.word(0x70)&0x80))return;
    bool clear=true;
    if(desired_yaw) {
        // This is a constraint throughout recovery, not a recurring impulse.
        forces.words[3]=state.word(0x34)=0;
        if(advance_motion_rules) {
            const Word previous=state.word(0x24);
            state.word(0x24)=add(previous,divide(sub(*desired_yaw,previous),2));
            const Word high=wrap(std::int8_t(sub(previous,*desired_yaw)>>8));
            clear=div2(high)==0;
        }
    }
    if(advance_motion_rules && clear)state.word(0x70)&=Word(~0x80);
}

void integrate_vehicle_velocity(VehicleState& state, VehicleForces& forces,
                                const std::array<Word,9>& orientation,bool advance_timing,
                                bool advance_motion_rules) {
    // The original limits force Y and all three torques, not force X/Z.
    for (const unsigned index : {1u,3u,4u,5u})
        forces.words[index] = wrap(std::clamp(signed_word(forces.words[index]),-3000,3000));
    for (const unsigned angle : {0x28u,0x26u}) {
        const int before = signed_word(state.word(angle));
        if (before>=8192) { state.word(angle)=8192;state.word(angle+0x10)=0; }
        else if (before < -8192) { state.word(angle)=wrap(-8192);state.word(angle+0x10)=0; }
    }
    if (!negative_difference(absolute(state.word(0x4a)),600)) {
        forces.words[0]=forces.words[2]=0;
        // Suppression is physical; the original one-eighth loss is per callback.
        if(advance_motion_rules)for (const unsigned velocity : {0x2eu,0x32u})
            state.word(velocity)=sub(state.word(velocity),divide(state.word(velocity),3));
    }
    for (unsigned axis=0;axis<6;++axis)
        state.word(0x2e + axis*2)=add(state.word(0x2e + axis*2),forces.words[axis]);
    // Keep original asymmetric thresholds and post-force ordering. Applying
    // these snaps on every small increment prevents small torques accumulating.
    if (advance_motion_rules && !(state.word(0x64)&8)) {
        if (!(state.word(0x42)&0x0430) && !(state.word(0x2c)&0x0400)) {
            for (const unsigned axis : {0u,1u,2u}) {
                auto& value=state.word(0x2e + axis*2);
                const int threshold=axis==1 ? 50 : 200;
                const int speed=signed_word(value);
                if (speed>=-threshold && speed<threshold) value=0;
            }
        }
        for (unsigned axis=0;axis<3;++axis) {
            auto& value=state.word(0x34 + axis*2);
            const int speed=signed_word(value);
            if (speed>=-64 && speed<64) value=0;
        }
    }
    std::array<std::int16_t,3> velocity{},coefficients{};
    for(unsigned axis=0;axis<3;++axis) {
        velocity[axis]=static_cast<std::int16_t>(signed_word(state.word(0x2e + axis*2)));
        coefficients[axis]=static_cast<std::int16_t>(signed_word(orientation[6+axis]));
    }
    state.word(0x4a)=wrap(project_speed(velocity,coefficients));
    Word engine=state.word(0x50);
    if (!negative_difference(state.word(0x44),4)) {
        engine=add(engine,engine);
        if(advance_motion_rules)state.word(0x28)=sub(state.word(0x28),divide(state.word(0x28),2));
    }
    if(advance_timing)state.word(0x52)=add(state.word(0x52),div2(sub(engine,state.word(0x52))));
}
std::vector<NativeProp> extract_easy_ride_props(const Rom& rom) {
    auto result=extract_props(rom,extract_easy_ride(rom));
    if(result.size()!=7)throw std::runtime_error("Unexpected Easy Ride trigger/pickup set");
    return result;
}
std::vector<NativeProp> extract_props(const Rom& rom, const Scene& scene) {
    std::vector<NativeProp> result;
    const bool ravine=scene.start_offset==course_definition(CourseId::big_ravine).scene_offset;
    const bool sky=scene.start_offset==course_definition(CourseId::sky_ramp).scene_offset;
    for(const auto& object:scene.objects) {
        Word extent_override=0;
        bool pickup=false;
        switch(object.callback_address) {
        case 0x09e3e4:extent_override=0x2a80;break;
        case 0x09ac4f:case 0x09ebe5:extent_override=0x0780;break;
        case 0x09937f:case 0x0993ed:case 0x09a877:case 0x09acab:case 0x09e9e1:case 0x09ea63:
            if(scene.start_offset!=course_definition(CourseId::night_cruise).scene_offset &&
               !((ravine || sky) && object.callback_address==0x09acab))continue;
            if(object.callback_address==0x09937f)extent_override=480;
            else if(object.callback_address==0x0993ed)extent_override=3840;
            else if(object.callback_address==0x09acab)extent_override=960;
            else extent_override=1920;
            break;
        case 0x098c16:case 0x09a293:
            if(scene.start_offset!=course_definition(CourseId::lake_side).scene_offset &&
               !(ravine && object.callback_address==0x09a293))continue;
            extent_override=0x0f00; // CPU09:8CB9 and 09:A2BB call 08:CCD2 with 3840 on each axis.
            break;
        case 0x09a997:case 0x099bd7:case 0x099c4a:case 0x099c97:case 0x0996fc:
            if(!sky)continue;
            extent_override=3840; // CPU09:A9B5, 9BF5 and 971F use the same three-axis bounds.
            break;
        case 0x09a907:
            if(scene.start_offset!=course_definition(CourseId::harbor_city).scene_offset)continue;
            extent_override=1920; // CPU09:A92F..A948, passed to 08:CCD2.
            break;
        case 0x09dfec:
            if(!ravine)continue;
            break;
        case 0x09a621:case 0x09a6f9:case 0x09ab56:case 0x09aabc:case 0x09aa33:
        case 0x09e8f1:case 0x09e969:case 0x09e87f:case 0x09e80d:case 0x09dd1e:
            if(!ravine)continue;
            extent_override=0x0780; // CPU08:CCD2 receives 1920 on all three axes.
            break;
        case 0x09a458:case 0x09a4e7:case 0x09a574:case 0x099048:case 0x09a06a:
            if(scene.start_offset!=course_definition(CourseId::white_land).scene_offset)continue;
            if(object.callback_address!=0x09a06a)
                extent_override=object.callback_address==0x099048?5760:1920;
            break;
        case 0x09cbdb:
            if(scene.start_offset!=course_definition(CourseId::sea_breeze).scene_offset)continue;
            extent_override=0x0f00; // 09:CBF9 registers the original 90E8 proximity body.
            break;
        case 0x09ad5e:case 0x09aebf:pickup=true;break;
        case 0x09e097:break;
        case 0x09e0ff:
            if(scene.start_offset!=course_definition(CourseId::kings_forest).scene_offset)continue;
            break;
        default:continue;
        }
        NativeProp prop{object.command_offset,{}};
        prop.callback_address=object.callback_address;
        prop.cpu_x=wrap(object.position.x);
        prop.destination_x=sub(prop.cpu_x,0x2080);
        auto& state=prop.state;
        // GSU04:8C95 + CPU08:CCA3, then08:D86F and callback extent overrides.
        state.word(0x16)=0x30;
        state.word(0x1a)=0x042d;
        state.word(0x1c)=4;
        state.word(0x14)=object.descriptor_address;
        state.word(0x1e)=wrap(object.position.x);
        state.word(0x20)=wrap(object.position.y);
        state.word(0x22)=wrap(object.position.z);
        state.word(0x24)=Word(object.orientation)<<8;
        for(unsigned axis=0;axis<3;++axis)
            state.word(0x0c+axis*2)=extent_override ? extent_override : rom.u16(object.command_offset+14+axis*2);
        // CPU09:AD89/AEEA passes20 to08:CD08, which stores A<<8.
        if(pickup)state.word(0x2a)=0x1400;
        // The Sunset roadside B4EC object uses the solid GSU04:8C0A body.
        // CPU08:CCA3 adds bit0x10 and 09:E0C2 sets mass20. Its position and
        // collision body persist when the CPU emits the contact effect.
        if(object.callback_address==0x09e097 || object.callback_address==0x09dfec) {
            state.word(0x16)=0x0018;
            state.word(0x1a)=0x0424;
            state.word(0x2a)=0x1400;
        }
        if(object.callback_address==0x09e0ff) {
            // E0FF lowers the trunk before E1A3 allocates its solid body.
            state.word(0x16)=0x0018;
            state.word(0x1a)=0x0424;
            state.word(0x20)-=140;
            state.word(0x2a)=0x0800;
        }
        if(object.callback_address==0x09a06a) {
            // 09:A06A lowers the sprite before allocating the 80-unit solid.
            state.word(0x20)-=120;
            state.word(0x16)=0x18;
            state.word(0x1a)=0x0424;
            state.word(0x2a)=0x0200;
        }
        result.push_back(prop);
    }
    return result;
}

void update_native_prop(NativeProp& prop,std::uint8_t player_lap,
                        bool active,bool player_contact) {
    if(!active)return;
    prop.state.word(0x1c)|=4; // CPU08:D070.
    if(prop.callback_address==0x09ad5e || prop.callback_address==0x09aebf) {
        // CPU09:ADB6/AF17 changes the byte angle before08:CE02 transfers it.
        prop.state.word(0x24)=add(prop.state.word(0x24),0x1000);
    } else if(prop.callback_address==0x09e3e4) {
        //08:CEC3 copies position before09:E4B7 advances the CPU's next position.
        prop.state.word(0x1e)=prop.cpu_x;
        if(player_contact && player_lap==1 && prop.cpu_x!=prop.destination_x) {
            prop.cpu_x=negative_difference(prop.cpu_x,prop.destination_x)
                ? add(prop.cpu_x,0x0280) : sub(prop.cpu_x,0x0280);
        }
    }
}

VehicleStepResult update_vehicle(Vehicle& vehicle,const VehicleTuning& tuning,
    const WheelContactTuning& contact_tuning,const OpponentTuning& direction_tuning,
    VehicleDriveContext& context,const VehicleStepEnvironment& environment) {
    auto& body=vehicle.body;
    VehicleStepResult result;
    body.word(0x1c)|=4;
    for(auto& wheel:vehicle.wheels) wheel.word(0x1c)|=4;
    if(environment.frozen) {
        result.orientation=vehicle_orientation(body,tuning);
        for(auto& wheel:vehicle.wheels)position_vehicle_attachment(body,wheel,result.orientation);
        return result;
    }
    body.word(0x1c)&=Word(~2);
    body.word(0x92)=0;
    const auto position=[&]() -> RacePosition {
        return {std::int16_t(signed_word(body.word(0x1e))),std::int16_t(signed_word(body.word(0x20))),
                std::int16_t(signed_word(body.word(0x22)))};
    };
    const auto run_ai=[&]() {
        if(!environment.wheel_candidate_lists)throw std::invalid_argument("Missing opponent candidate lists");
        std::vector<VehicleState> racers;
        for(const auto* racer:environment.racers) {
            if(!racer)throw std::invalid_argument("Null opponent participant");
            racers.push_back(*racer);
        }
        return update_opponent_ai(vehicle,environment.route,direction_tuning,tuning,
            std::uint8_t(environment.difficulty),environment.player_rank,racers,environment.opponent_index,
            *environment.wheel_candidate_lists,result.orientation);
    };
    bool run_physics=!(body.word(0x1c)&0x20);
    if(run_physics && (body.word(0x1c)&0x10))run_physics=(body.word(0x64)&8) && run_ai();
    if (run_physics) {
        integrate_vehicle_pose(body);
        result.orientation=vehicle_orientation(body,tuning);
        if(body.word(0x64)&8)run_physics=run_ai();
        else {
            const auto nearest=environment.route.empty()?std::nullopt:nearest_route_point(environment.route,position());
            body.word(0x58)=nearest ? nearest->index : 0;
        }
    }
    if(run_physics) {
        begin_vehicle_forces(body,tuning,result.forces,context.advance_timing);
        update_vehicle_controls(body,tuning,result.forces,context,environment.steering_q15);
        for(auto& wheel:vehicle.wheels)position_vehicle_attachment(body,wheel,result.orientation);
        body.word(0x44)=body.word(0x46)=body.word(0x56)=0;
        if(auto* guide=environment.trailer_guide) {
            // The last attachment allocated by 08:B956 is first in the GSU contact list.
            position_vehicle_attachment(body,*guide,result.orientation);
            update_wheel_contact(body,*guide,result.forces,environment.trailer_guide_surface,tuning,contact_tuning,result.orientation);
            body.word(0x92)|=guide->word(0x2c);
        }
        for(std::size_t i=0;i<vehicle.wheels.size();++i) {
            auto& wheel=vehicle.wheels[i];
            if(wheel.word(0x46)&0x0800)throw std::invalid_argument("Special wheel routine is not recovered");
            update_wheel_contact(body,wheel,result.forces,environment.wheel_candidates[i],
                                 tuning,contact_tuning,result.orientation);
            body.word(0x92)|=wheel.word(0x2c);
        }
        update_vehicle_contact_state(body,tuning,result.forces);
        BodyCollisionWork contact_work;
        update_body_collisions(body,result.forces,environment.body_candidates,
                               {context.damage_mode,environment.difficulty},contact_work);
        update_vehicle_alignment(body,result.orientation);
        if(update_vehicle_damage(body,tuning,result.forces,context))
            apply_nearby_crash_impulse(body,environment.racers);
        if(body.word(0x70)&0x80) {
            std::optional<Word> desired;
            if(body.word(0x58) && (body.word(0x8a)&0x0100)) {
                const auto match=nearest_route_point(environment.route,position());
                if(match) {
                    const auto next=advance_route_index(match->index,Word(environment.route.size()),body.word(0x70));
                    const auto& target=environment.route[next-1].position;
                    desired=negate(original_direction_angle(
                        std::int16_t(signed_word(sub(wrap(target[0]),body.word(0x1e)))),
                        std::int16_t(signed_word(sub(wrap(target[2]),body.word(0x22)))),direction_tuning));
                }
            }
            update_vehicle_recovery(body,result.forces,desired,context.advance_motion_rules);
        }
        integrate_vehicle_velocity(body,result.forces,result.orientation,context.advance_timing,
                                   context.advance_motion_rules);
    }
    // Stunt courses have section triggers and no ordered lap route.
    if(environment.route.empty())return result;
    RouteProgress progress{body.word(0x58),body.word(0x86),body.word(0x7a),body.word(0x84)};
    update_route_progress(progress,Word(environment.route.size()));
    body.word(0x86)=progress.accepted_index;
    body.word(0x7a)=progress.laps;
    body.word(0x84)=route_next_distance(environment.route,body.word(0x86),body.word(0x70),position());
    return result;
}

VehicleStepResult update_player_vehicle(Vehicle& vehicle,const VehicleTuning& tuning,
    const WheelContactTuning& contact_tuning,const OpponentTuning& direction_tuning,
    VehicleDriveContext& context,const VehicleStepEnvironment& environment) {
    if(vehicle.body.word(0x64)&8)throw std::invalid_argument("Player update requires a player vehicle");
    return update_vehicle(vehicle,tuning,contact_tuning,direction_tuning,context,environment);
}

}
