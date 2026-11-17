#include "srf/trailer_loading.hpp"
#include "srf/native_race.hpp"
#include "srf/presentation.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace srf {
namespace {
struct DoorTiming { unsigned approach_phase, approach_tick, closed_tick, phase100_tick, phase140_tick; };
// USA Rev 1 video interrupts, measured from truck allocation. The orbit and
// title increase the time taken by each original object pass. Only the
// schedule uses these measurements; car/wheel poses come from the native step.
constexpr unsigned phases[]{3,9,19,35,55,69};
constexpr unsigned phase_ticks[][6]{
    {10,37,110,181,277,345}, {10,37,109,176,272,338}, {10,35,108,176,269,332}
};
constexpr DoorTiming door_timings[3][3]{
    {{89,441,478,489,662},{85,425,461,489,661},{86,428,464,488,660}},
    {{89,431,466,477,644},{85,415,450,476,643},{86,418,452,475,642}},
    {{90,426,461,467,627},{86,411,445,467,626},{85,405,439,465,624}}
};

CollisionShape loading_ramp(const Rom& rom,const SceneObject& marker) {
    // 08:F06F..F0AC installs C6DC at marker Y-100, extents 0640 on all axes.
    CollisionShape shape{marker.command_offset,0xc6dc,
        {static_cast<std::int16_t>(marker.position.x),static_cast<std::int16_t>(marker.position.y-100),
         static_cast<std::int16_t>(marker.position.z)}, {1600,1600,1600},marker.orientation,0xc181,{}};
    const auto model=extract_world_model(rom,shape.descriptor_address);
    for(const auto& face:model.primitives) {
        if(!face.attributes[0])continue;
        const auto vertex=model.frames.front().at(face.indices.front());
        CollisionPlane plane{{static_cast<std::int16_t>(vertex.x),static_cast<std::int16_t>(vertex.y),
                              static_cast<std::int16_t>(vertex.z)}, {},face.attributes[0]};
        for(unsigned axis=0;axis<3;++axis)
            plane.normal[axis]=static_cast<std::int16_t>(-static_cast<std::int8_t>(face.attributes[3+axis])*256);
        shape.planes.push_back(plane);
    }
    return shape;
}
}

TrailerLoading::TrailerLoading(const Rom& rom,const Scene& scene,PlayerCar selected)
    :directions_(extract_opponent_tuning(rom)),route_(extract_route(scene)),shapes_(extract_collision(rom,scene)),
     selected_(static_cast<unsigned>(selected)) {
    for(;league_<3;++league_) {
        const auto ptr=rom.slice(Rom::lorom_offset(0x03f4a5)+(20+league_)*3,3);
        if(scene.start_offset==Rom::lorom_offset(ptr[0]|(ptr[1]<<8)|(ptr[2]<<16)))break;
    }
    const auto marker=std::find_if(scene.objects.begin(),scene.objects.end(),
        [](const auto& o){return o.callback_address==0x08efaf;});
    if(league_>=3 || selected_>=3 || marker==scene.objects.end())throw std::invalid_argument("Invalid trailer loading scene or car");
    // 08:EFAF creates a vehicle with the selected definition in WRAM 18DB;
    // 08:B1E5 configures near opponent dynamics and 08:EF19 owns its input.
    const auto definition=rom.u16(Rom::lorom_offset(0x03b6f1)+2*selected_);
    const auto definition_offset=Rom::lorom_offset(0x090000u|definition);
    const auto descriptor=rom.u16(definition_offset)&0x7fff;
    distant_variant_=rom.bytes()[definition_offset+4]&3;
    distant_diameter_=static_cast<std::uint16_t>(original_sprite_diameter(
        rom.u16(descriptor+16),rom.bytes()[descriptor+7],50));
    car_=create_vehicle(rom.bytes(),definition,
        {static_cast<std::int16_t>(marker->position.x),-480,static_cast<std::int16_t>(marker->position.z-3500)},
        static_cast<std::uint16_t>(marker->orientation<<8),12,0,0x0208);
    car_.body.word(0x40)|=0x0200;
    car_.body.word(0x70)=4; // Follow the loading callback; suppress route steering and racer avoidance.
    tuning_=extract_vehicle_tuning(rom.bytes(),car_.body.word(0x6e));
    wheels_=extract_wheel_contact_tuning(rom,tuning_.contact_parameters);
    drive_.boost_options=0x0208;
    ramp_={marker->command_offset,0xa7b0,marker->position,marker->orientation,0,0,0,false};
    ramp_.position.y-=50; // Visual child at the collision origin plus 50.
    shapes_.push_back(loading_ramp(rom,*marker));
}

std::optional<ScenerySprite> TrailerLoading::distant_car(double eye_x,double eye_z,std::uint8_t camera_yaw) const {
    const auto& body=car_.body;
    const Vertex position{std::int16_t(body.word(0x1e)),std::int16_t(body.word(0x20)),std::int16_t(body.word(0x22))};
    // 08:B5FA compares Manhattan distance to the camera. In the far shot
    // B644 combines the camera-relative octant with the definition's car ID.
    if(!car_visible() || std::abs(eye_x-position.x)+std::abs(eye_z-position.z)<4500)return std::nullopt;
    const auto angle=static_cast<std::uint8_t>((body.word(0x24)>>8)-camera_yaw);
    const auto frame=static_cast<std::uint16_t>(((angle>>3)&0x1c)|distant_variant_);
    return ScenerySprite{ramp_.command_offset,position,0x99a3,distant_diameter_,frame};
}

unsigned TrailerLoading::callback_tick(unsigned phase) const {
    const auto door=door_timings[league_][selected_];
    unsigned begin_phase=2,begin_tick=league_==1?8:7;
    const auto within=[&](unsigned end_phase,unsigned end_tick) {
        const unsigned span=end_phase-begin_phase;
        return begin_tick+((phase-begin_phase)*(end_tick-begin_tick)+span/2)/span;
    };
    for(unsigned i=0;i<std::size(phases);++i) {
        if(phase<=phases[i])return within(phases[i],phase_ticks[league_][i]);
        begin_phase=phases[i]; begin_tick=phase_ticks[league_][i];
    }
    if(phase<=door.approach_phase)return within(door.approach_phase,door.approach_tick);
    begin_phase=door.approach_phase; begin_tick=door.approach_tick;
    if(phase<=begin_phase+8)return within(begin_phase+8,door.closed_tick);
    begin_phase+=8; begin_tick=door.closed_tick;
    if(phase<=100)return within(100,door.phase100_tick);
    begin_phase=100; begin_tick=door.phase100_tick;
    return within(140,door.phase140_tick);
}

void TrailerLoading::update_car() {
    auto& body=car_.body;
    body.word(0x42)=static_cast<std::int16_t>(body.word(0x4a))<30?0x0800:0;
    drive_.update_phase=static_cast<std::uint16_t>(phase_);
    // The stationary tractor's contact in 2342 is zero while airborne, then
    // -350 after its wheels settle. Its last contact feeds the next loading impulse.
    drive_.loading_contact_y=phase_<8?0:-350;
    refresh_vehicle_contact_candidates(car_,shapes_,candidates_);
    std::array<VehicleState*,1> participant{&body};
    VehicleStepEnvironment env{};
    env.route=route_; env.racers=participant; env.player_rank=1;
    env.wheel_candidate_lists=&candidates_;
    for(unsigned i=0;i<car_.wheels.size();++i) {
        car_.wheels[i].word(0x18)=!candidates_[i].empty();
        if(candidates_[i].empty())continue;
        const auto shape=std::find_if(shapes_.begin(),shapes_.end(),
            [&](const auto& s){return s.command_offset==candidates_[i].front();});
        if(shape!=shapes_.end())env.wheel_candidates[i]=&*shape;
    }
    update_vehicle(car_,tuning_,wheels_,directions_,drive_,env);
}

void TrailerLoading::advance(unsigned ticks) {
    ticks_+=std::min(ticks,std::numeric_limits<unsigned>::max()-ticks_);
    ramp_.visible=!closed_ && ticks_>=(league_==1?8u:7u);
    while(!closed_ && ticks_>=callback_tick(phase_+1)) {
        update_car();
        ++phase_;
        // 08:EF38 signals arrival at marker Z-600. F0B1 selects A794 and
        // advances it once on each subsequent pass; the pass after frame 7
        // retires the child and EF73 hides the selected car and its wheels.
        if(closing_) {
            if(ramp_.frame==7) {
                closed_=true;
                ramp_.visible=false;
            } else {
                ramp_.visual_descriptor=0xa794;
                ++ramp_.frame;
            }
        } else closing_=static_cast<std::int16_t>(car_.body.word(0x22))+600>=ramp_.position.z;
    }
    // The camera runs on the same object clock after the car's retirement.
    while(closed_ && phase_<140 && ticks_>=callback_tick(phase_+1))++phase_;
}
}
