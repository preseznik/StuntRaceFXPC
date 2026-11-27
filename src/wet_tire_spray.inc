// A short visual history in simulation time. A parked, paused or replayed car
// emits exactly the same spray regardless of the desktop refresh rate.
class WetTireSpray {
    struct Puff {
        n64::Vec position, drift;
        float born, strength, seed;
    };
    std::vector<Puff> puffs;
    float tire_scale=1.f;
    static constexpr float lifetime=5.f; // native phases

public:
    static bool grounded(const srf::VehicleState& wheel) {
        return (wheel.word(0x46)&0x210)==0x10; // loaded contact, visible wheel
    }
    static float contact(const srf::VehicleState& wheel,unsigned axis) {
        const auto sum=std::uint16_t(wheel.word(0x1e + 2*axis)+wheel.word(0x4a + 2*axis));
        return float(std::bit_cast<std::int16_t>(sum));
    }
    WetTireSpray()=default;
    explicit WetTireSpray(unsigned tire_diameter)
        :tire_scale(float(tire_diameter)/192.f) {} // Reference 4WD tire diameter.
    void begin_phase(unsigned phase) {
        if(phase<=1)puffs.clear();
        std::erase_if(puffs,[&](const Puff& p){return float(phase)-p.born>=lifetime+1;});
    }
    void emit(unsigned phase,unsigned actor,const srf::Vehicle& before,const srf::Vehicle& car) {
        // Route-only rivals have no live contact patches.
        if(car.body.word(0x16)&0x400)return;
        const float speed=std::abs(int(std::bit_cast<std::int16_t>(car.body.word(0x4a))));
        const float strength=std::clamp(tire_scale*(speed-24)/370.f,0.f,1.f);
        if(strength==0)return;
        for(unsigned j=0;j<car.wheels.size();++j) {
            const auto& w=car.wheels[j];
            if(std::bit_cast<std::int16_t>(w.word(0x40))>=0 || !grounded(w))continue;
            const auto& old=before.wheels[j];
            // Reset the trail across a respawn or a signed world-coordinate wrap.
            std::array<float,3> travel{};
            for(unsigned a=0;a<3;++a)travel[a]=contact(w,a)-contact(old,a);
            const bool continuous=grounded(old) && !(before.body.word(0x16)&0x400) &&
                std::ranges::all_of(travel,[speed](float d){return std::abs(d)<2*speed+256;});
            // Keep a tire-width spacing at racing speed without piling up
            // extra particles as the car slows.
            const int samples=continuous?std::clamp(int(std::ceil(speed/65.f)),1,8):1;
            for(int sample=0;sample<samples;++sample) {
                const float t=float(sample+1)/samples;
                Puff p{{},{},float(phase)-1+t,strength,float((phase*17+actor*37+j*53+sample*19)%127)};
                for(unsigned a=0;a<3;++a) {
                    const float move=continuous?travel[a]:0;
                    p.position[a]=contact(w,a)-(1-t)*move;
                    p.drift[a]=.22f*move;
                }
                p.position[1]-=12;
                puffs.push_back(p);
            }
        }
    }
    template<class Normalize>
    void mesh(std::vector<GpuVertex>& vertices,float phase,float cy,float sy,float cp,float sp,
              float unit,const Normalize& normalize) const {
        for(const auto& p:puffs) {
            const float age=phase-p.born,t=age/lifetime;
            if(t<0 || t>=1)continue;
            auto location=p.position;
            for(unsigned a=0;a<3;++a)location[a]+=p.drift[a]*age;
            location[1]-=tire_scale*t*(40+72*p.strength);
            const auto c=normalize(location);
            const float radius=unit*tire_scale*(35+112*t)*(.55f+.65f*p.strength);
            const float opacity=.40f*p.strength*(1-t)*(1-t);
            std::array<GpuVertex,4> quad;
            for(unsigned i=0;i<4;++i) {
                const float u=(i==1 || i==2)?1.f:-1.f,v=i>=2?1.f:-1.f;
                const float x=u*radius,y=v*radius*.8f;
                quad[i]={c[0]+cy*x+sy*sp*y,c[1]+cp*y,c[2]+sy*x-cy*sp*y,
                         opacity,p.seed,t,u,v,0};
            }
            for(auto i:{0,1,2,0,2,3})vertices.push_back(quad[i]);
        }
    }
};

// Use each vehicle's own track position: the camera car and a rival can
// straddle a portal. Native +58 is the one-based nearest route point.
bool spray_exposed(srf::CourseId course,const srf::CourseEnvironment& environment,const srf::VehicleState& body) {
    if(course==srf::CourseId::aqua_tunnel) {
        const auto route=body.word(0x58);
        const int x=std::bit_cast<std::int16_t>(body.word(0x1e));
        // The north descent starts at the outer E9E1 gate. The south ascent
        // emerges at EA63. Split their approach segments at the portal edge.
        if(route==31 || route==32)return x>5632;
        if(route==77 || route==78)return x>=-3968;
        return route<31 || route>78;
    }
    if(course==srf::CourseId::lake_side || course==srf::CourseId::harbor_city) {
        const srf::RacePosition position{std::int16_t(body.word(0x1e)),std::int16_t(body.word(0x20)),std::int16_t(body.word(0x22))};
        for(const auto& gate:environment.triggers()) {
            if(gate.callback_address==0x09ebe5 && srf::collision_bounds_overlap(position,
               {body.word(0x0c),body.word(0x0e),body.word(0x10)},gate.position,{gate.extent,gate.extent,gate.extent}))return false;
        }
    }
    return true;
}
