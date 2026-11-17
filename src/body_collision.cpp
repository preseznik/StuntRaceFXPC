#include "srf/body_collision.hpp"
#include <algorithm>
#include <stdexcept>

namespace srf {
namespace {
using Word=std::uint16_t;
int signed_word(Word x) { return x>=32768?int(x)-65536:int(x); }
Word add(Word a,Word b) { return Word(unsigned(a)+b); }
Word sub(Word a,Word b) { return Word(unsigned(a)-b); }
Word negate(Word a) { return Word(0u-a); }
Word absolute(Word a) { return signed_word(a)<0?negate(a):a; }
Word div2(Word a) { return Word(a==65535?0:signed_word(a)>>1); }
Word high_product(Word a,Word b) {
    const auto product=signed_word(a)*signed_word(b);
    return Word(static_cast<std::uint32_t>(product)>>16);
}
Word mass_fraction(Word target,Word sum) {
    // Original C135 divides a32-bit numerator by a16-bit denominator.
    // A zero-mass moving body (Big Ravine rock chip) leaves the original
    // 16 subtraction rounds successful. R4 is FFFF before the final LSR.
    if(target && target==sum)return 0x7fff;
    std::uint32_t numerator=std::uint32_t(target)<<16,denominator=sum;
    if((target|sum)&0x8000) { numerator>>=1; denominator>>=1; }
    if(!denominator) throw std::runtime_error("Invalid zero body-contact mass");
    return Word(numerator/denominator)>>1;
}
}

bool body_pair_overlap_filter(const VehicleState& a,const VehicleState& b) {
    const auto dx=sub(a.word(0x1e),b.word(0x1e)),dz=sub(a.word(0x22),b.word(0x22));
    const auto normal=normalize_vehicle_vector({dx,0,dz});
    const auto separation=add(high_product(add(dx,dx),normal[0]),high_product(add(dz,dz),normal[2]));
    const auto closing=negate(add(high_product(div2(sub(a.word(0x2e),b.word(0x2e))),normal[0]),
                                  high_product(div2(sub(a.word(0x32),b.word(0x32))),normal[2])));
    auto allowance=sub(add(a.word(0xc),b.word(0xc)),660);
    if(signed_word(allowance)<0)allowance=0;
    return signed_word(sub(add(allowance,closing),separation))>=0;
}

RadialImpulse radial_body_impulse(const VehicleState& body,const BodyContactCandidate& candidate) {
    const auto direction=normalize_vehicle_vector({sub(body.word(0x1e),candidate.position[0]),0,
                                                   sub(body.word(0x22),candidate.position[1])});
    const auto dx=sub(candidate.velocity[0],body.word(0x2e));
    const auto dz=sub(candidate.velocity[1],body.word(0x32));
    Word closing=add(high_product(add(dx,dx),direction[0]),high_product(add(dz,dz),direction[2]));
    if(signed_word(closing)>=0 && signed_word(sub(1200,closing))>=0) closing=1200;
    const auto mass=candidate.target->word(0x2a);
    const auto fraction=mass_fraction(mass,add(mass,body.word(0x2a)));
    return {direction,closing,high_product(add(closing,closing),fraction)};
}

void update_body_collisions(VehicleState& body,VehicleForces& forces,
                            std::span<const BodyContactCandidate> candidates,
                            const BodyCollisionContext& context,BodyCollisionWork& work) {
    work.magnitude=work.closing_speed=0;
    if(!context.damage_mode) body.word(0x94)=0;
    if(candidates.empty()) { body.word(0x1c)&=Word(~0x40); return; }
    if(body.word(0x1c)&0x40) return;
    if(!(body.word(0x16)&0x10)) throw std::runtime_error("Unsupported non4WD body contact path");
    for(const auto& candidate:candidates) {
        if(!candidate.target) throw std::invalid_argument("Missing body-contact target");
        const auto& target=*candidate.target;
        if((target.word(0x1a)&2) || (target.word(0x16)&0x20)) continue;
        if(target.word(0x16)&2) {
            const auto delay=sub(20,add(context.difficulty,context.difficulty));
            body.word(0x7c)=signed_word(delay)<0?0:delay;
            if(add(body.word(0x5e),target.word(0x5e))) continue;
        }
        work.force.fill(0);
        if((target.word(0x16)&2) && (!(body.word(0x64)&0x10) || (target.word(0x64)&3)))
            body.word(0x2c)|=4;
        const auto [direction,closing,strength]=radial_body_impulse(body,candidate);
        work.closing_speed=closing;
        if(signed_word(strength)>=0 && strength!=32000) {
            work.contact_flags|=1;
            work.magnitude=add(strength,strength); work.normal=direction;
            for(unsigned axis=0;axis<3;++axis)
                work.force[axis]=Word(std::clamp(signed_word(high_product(work.magnitude,direction[axis])),-5000,5000));
            if(signed_word(closing)>=0) body.word(0x94)=add(body.word(0x94),closing);
        }
        work.force[0]=add(work.force[0],work.force[0]);
        work.force[2]=add(work.force[2],work.force[2]); work.force[1]=0;
        forces.words[0]=add(forces.words[0],work.force[0]);
        forces.words[2]=add(forces.words[2],work.force[2]);
        forces.impulses[0]=add(forces.impulses[0],work.force[0]);
        forces.impulses[2]=add(forces.impulses[2],work.force[2]);
    }
}

void apply_nearby_crash_impulse(const VehicleState& source,std::span<VehicleState* const> racers) {
    const std::array<Word,3> position{source.word(0x1e),source.word(0x20),source.word(0x22)};
    for(auto* target:racers) {
        if(!target) throw std::invalid_argument("Missing racer");
        if(target==&source) continue;
        std::array<Word,3> delta{};Word distance=0;
        for(unsigned axis=0;axis<3;++axis) {
            delta[axis]=sub(target->word(0x1e + axis*2),position[axis]);
            distance=add(distance,absolute(div2(delta[axis])));
        }
        const auto proximity=sub(distance,2000);
        if(signed_word(proximity)>=0) continue;
        const auto amplitude=add(absolute(proximity),absolute(proximity));
        const auto direction=normalize_vehicle_vector({delta[0],0,delta[2]});
        target->word(0x2e)=add(target->word(0x2e),high_product(direction[0],amplitude));
        target->word(0x32)=add(target->word(0x32),high_product(direction[2],amplitude));
        const auto spin=target->word(0x34);
        const auto spin_scale=add(add(amplitude,amplitude),add(amplitude,amplitude));
        target->word(0x34)=add(spin,high_product(Word(signed_word(spin)<0?-16000:16000),spin_scale));
        target->word(0x7c)=30;
    }
}
}
