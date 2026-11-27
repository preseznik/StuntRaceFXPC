#include "srf/body_collision.hpp"
#include "srf/vehicle_math.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
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

namespace {
struct Axis {
    double x,z;
    double dot(Axis b) const {return x*b.x+z*b.z;}
};

struct RacerHull {
    Axis forward,side;
    double width,length,roof,axle;
    double extent(Axis n) const {return width*std::abs(side.dot(n))+length*std::abs(forward.dot(n));}
};
RacerHull racer_hull(const Vehicle& car) {
    const double yaw=-car.body.word(0x24)*(2*std::numbers::pi/65536);
    int width=0,length=0,roof=0,axle=0;
    for(unsigned i=0;i<car.wheels.size();++i) {
        const auto& wheel=car.wheels[i];
        const int diameter=car.tire_diameters[i];
        // Include the outer tread beyond each hub, including the taller 4WD tyres.
        const int track=std::abs(signed_word(wheel.word(0x3c)));
        width=std::max(width,track+diameter/4);
        length=std::max(length,std::abs(signed_word(wheel.word(0x40)))+diameter/2);
        roof=std::max(roof,track);
        axle=std::max(axle,signed_word(wheel.word(0x3e)));
    }
    return {{std::sin(yaw),std::cos(yaw)},{std::cos(yaw),-std::sin(yaw)},
            double(width),double(length),double(roof),double(axle)};
}
RacePosition racer_position(const Vehicle& car) {
    return {std::int16_t(car.body.word(0x1e)),std::int16_t(car.body.word(0x20)),std::int16_t(car.body.word(0x22))};
}
Axis movement(RacePosition from,RacePosition to) {
    return {double(std::int16_t(to[0]-from[0])),double(std::int16_t(to[2]-from[2]))};
}
struct RacerContact { Axis normal; double correction; };

// Intersect the time intervals on both cars' separating axes. Keep the side
// of first contact even if their centres exchanged places during the phase.
std::optional<RacerContact> sweep_racers(const RacerHull& a,RacePosition start_a,RacePosition end_a,
                                         const RacerHull& b,RacePosition start_b,RacePosition end_b) {
    const auto start=movement(start_b,start_a),da=movement(start_a,end_a),db=movement(start_b,end_b);
    const Axis travel{da.x-db.x,da.z-db.z};
    double enter=0,leave=1;
    const double y=std::int16_t(start_a[1]-start_b[1]);
    const double dy=std::int16_t(end_a[1]-start_a[1])-std::int16_t(end_b[1]-start_b[1]);
    const auto interval=[&](double p,double d,double low,double high) {
        if(d==0)return p>=low && p<=high;
        const auto t0=(low-p)/d,t1=(high-p)/d;
        enter=std::max(enter,std::min(t0,t1));leave=std::min(leave,std::max(t0,t1));
        return enter<=leave;
    };
    if(!interval(y,dy,-(a.axle+b.roof),a.roof+b.axle))return {};
    const std::array axes{a.side,a.forward,b.side,b.forward};
    for(auto n:axes) {
        const double extent=a.extent(n)+b.extent(n);
        if(!interval(start.dot(n),travel.dot(n),-extent,extent))return {};
    }
    double shallowest=1e30;
    RacerContact hit{};
    for(auto n:axes) {
        const double extent=a.extent(n)+b.extent(n),speed=travel.dot(n);
        const double at=start.dot(n)+enter*speed;
        const double depth=extent-std::abs(at);
        if(depth>=shallowest)continue;
        shallowest=depth;
        const double sign=at==0?(speed>0?-1.:1.):std::copysign(1.,at);
        hit={{sign*n.x,sign*n.z},extent-sign*(start.dot(n)+speed)};
    }
    if(hit.correction<0)return {};
    return hit;
}

void translate_wheels(Vehicle& car,int dx,int dz) {
    for(auto& wheel:car.wheels) {
        wheel.word(0x1e)+=dx;wheel.word(0x22)+=dz;
    }
}

Axis contact_velocity(const Vehicle& car,RacePosition previous) {
    const auto& body=car.body;
    if(body.word(0x1c)&0x10) {
        // Distant AI stores the route command in its velocity words.
        const auto d=movement(previous,racer_position(car));
        return {8*d.x,8*d.z};
    }
    return {double(signed_word(body.word(0x2e))),double(signed_word(body.word(0x32)))};
}

bool push_racer(Vehicle& car,const VehicleTuning& tuning,const RacerContact& hit,double share,
                double closing,VehicleForces& impact,std::span<const CourseWall> walls,
                std::span<const CollisionShape> shapes) {
    auto& body=car.body;
    const auto from=racer_position(car);
    const auto orientation=vehicle_orientation(body,tuning);
    const auto n=hit.normal;
    const bool distant=(body.word(0x1c)&0x10)!=0;
    const auto vx=body.word(0x2e),vz=body.word(0x32),spin=body.word(0x34);
    if(!distant)for(unsigned axis:{0u,2u}) {
        // Restitution 1/8. Exchange only the closing normal velocity.
        const int impulse=int(std::lround(1.125*closing*share*(axis==0?n.x:n.z)));
        auto& v=body.word(0x2e + axis*2);
        v=Word(std::clamp(signed_word(v)+impulse,-32768,32767));
        auto& sample=impact.words[axis];
        if(std::abs(impulse)>std::abs(signed_word(sample)))sample=Word(std::clamp(impulse,-32767,32767));
    }
    const double correction=(hit.correction+1)*share;
    body.word(0x1e)+=std::lround(correction*n.x);
    body.word(0x22)+=std::lround(correction*n.z);
    resolve_course_walls(car,from,orientation,walls,shapes);
    if(distant) {
        body.word(0x2e)=vx;body.word(0x32)=vz;body.word(0x34)=spin;
    } else {
        const RacePosition velocity{std::int16_t(body.word(0x2e)),std::int16_t(body.word(0x30)),std::int16_t(body.word(0x32))};
        const RacePosition forward{std::int16_t(orientation[6]),std::int16_t(orientation[7]),std::int16_t(orientation[8])};
        body.word(0x4a)=Word(project_speed(velocity,forward));
    }
    const int dx=std::int16_t(body.word(0x1e)-from[0]),dz=std::int16_t(body.word(0x22)-from[2]);
    translate_wheels(car,dx,dz);
    return dx!=0 || dz!=0;
}
}

void resolve_racer_contacts(std::span<Vehicle> cars,std::span<const RacePosition> previous,
                            std::span<const VehicleTuning> tuning,std::span<const CourseWall> walls,
                            std::span<const CollisionShape> shapes,std::span<Vehicle> midpoint) {
    constexpr unsigned max_cars=4;
    if(cars.size()>max_cars || previous.size()<cars.size() || tuning.size()<cars.size() ||
       (!midpoint.empty() && midpoint.size()!=cars.size()))throw std::invalid_argument("Invalid racer contact grid");
    std::array<RacerHull,max_cars> hulls{};
    std::array<VehicleForces,max_cars> impacts{};
    std::array<bool,max_cars> touched{};
    for(unsigned i=0;i<cars.size();++i)hulls[i]=racer_hull(cars[i]);
    // Recheck neighbours after a push, particularly when a wall limits the
    // correction one car can take. Fixed grid order is shared by every peer.
    for(int pass=0;pass<8;++pass) {
        bool moved=false;
        for(unsigned i=0;i<cars.size();++i)for(unsigned j=i+1;j<cars.size();++j) {
            const auto a=racer_position(cars[i]),b=racer_position(cars[j]);
            const auto hit=sweep_racers(hulls[i],pass?a:previous[i],a,hulls[j],pass?b:previous[j],b);
            if(!hit)continue;
            const double vi=contact_velocity(cars[i],previous[i]).dot(hit->normal);
            const double vj=contact_velocity(cars[j],previous[j]).dot(hit->normal);
            const double closing=std::max(0.,vj-vi);
            const double mi=cars[i].body.word(0x2a),mj=cars[j].body.word(0x2a);
            moved|=push_racer(cars[i],tuning[i],*hit,mj/(mi+mj),closing,impacts[i],walls,shapes);
            moved|=push_racer(cars[j],tuning[j],*hit,-mi/(mi+mj),closing,impacts[j],walls,shapes);
            touched[i]=touched[j]=true;
        }
        if(!moved)break;
    }
    for(unsigned i=0;i<cars.size();++i)if(touched[i]) {
        if(!midpoint.empty() && (cars[i].body.word(0x1c)&8)) {
            // The headset may present the saved half-step. Keep that chassis
            // and its tyres on the corrected trajectory as well.
            auto& mid=midpoint[i];const auto d=movement(previous[i],racer_position(cars[i]));
            const int dx=std::int16_t(previous[i][0]+int(d.x)/2-mid.body.word(0x1e));
            const int dz=std::int16_t(previous[i][2]+int(d.z)/2-mid.body.word(0x22));
            mid.body.word(0x1e)+=dx;mid.body.word(0x22)+=dz;translate_wheels(mid,dx,dz);
        }
        if(impacts[i].words[0] || impacts[i].words[2]) {
            VehicleDriveContext context{};context.damage_mode=1;
            update_vehicle_damage(cars[i].body,tuning[i],impacts[i],context);
        }
    }
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

// Resolve only relative motion into the contact normal. A glancing contact
// keeps its tangential speed, and a separating pair receives no second kick.
static RadialImpulse enhanced_body_impulse(const VehicleState& body,const BodyContactCandidate& candidate) {
    const int vx=signed_word(candidate.velocity[0])-signed_word(body.word(0x2e));
    const int vz=signed_word(candidate.velocity[1])-signed_word(body.word(0x32));
    auto dx=sub(body.word(0x1e),candidate.position[0]),dz=sub(body.word(0x22),candidate.position[1]);
    if(!dx && !dz) { dx=Word(vx/2); dz=Word(vz/2); }
    const auto normal=normalize_vehicle_vector({dx,0,dz});
    const auto projected=std::int64_t(vx)*signed_word(normal[0])+std::int64_t(vz)*signed_word(normal[2]);
    const int closing=int(std::clamp<std::int64_t>(projected/32768,0,16383));
    const unsigned other_mass=candidate.target->word(0x2a);
    const unsigned total_mass=body.word(0x2a)+other_mass;
    // The existing force transfer doubles strength. Target restitution is 1/8.
    const auto strength=std::int64_t(closing)*9*other_mass/(16*std::max(1u,total_mass));
    return {normal,Word(closing),Word(strength)};
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
        const auto [direction,closing,strength]=context.physics==DrivingPhysics::enhanced
            ?enhanced_body_impulse(body,candidate):radial_body_impulse(body,candidate);
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
