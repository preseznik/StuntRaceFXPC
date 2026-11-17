#include "srf/speed_bonus.hpp"
#include <algorithm>
#include <bit>

namespace srf {
namespace {
Scene bonus_scene(const Rom& rom,SpeedClass league) {
    const auto id=static_cast<unsigned>(league);
    if(id>2)throw std::invalid_argument("Unsupported bonus class");
    const auto pointer=rom.slice(Rom::lorom_offset(0x03f4a5)+(20+id)*3,3);
    return extract_scene(rom,Rom::lorom_offset(pointer[0]|(pointer[1]<<8)|(pointer[2]<<16)));
}
RacePosition position(Vertex vertex) {
    return {static_cast<std::int16_t>(vertex.x),static_cast<std::int16_t>(vertex.y),static_cast<std::int16_t>(vertex.z)};
}
}

SpeedBonus::SpeedBonus(const Rom& rom,SpeedClass league)
    :scene_(bonus_scene(rom,league)),route_(extract_route(scene_)),shapes_(extract_collision(rom,scene_)),
     tuning_(extract_vehicle_tuning(rom.bytes(),0x86dd)),wheel_tuning_(extract_wheel_contact_tuning(rom,tuning_.contact_parameters)),
     direction_(extract_opponent_tuning(rom)) {
    // 08:AF0F selects truck definition 09:8669 for the three bonus scenes.
    const auto spawn=std::find_if(scene_.objects.begin(),scene_.objects.end(),[](const auto& o){return o.callback_address==0x08af0f;});
    if(spawn==scene_.objects.end())throw std::runtime_error("Missing trailer start");
    auto start=position(spawn->position);
    start[1]-=160;
    truck_=create_bonus_trailer(rom.bytes(),start,std::uint16_t(spawn->orientation<<8),guide_);
    truck_.body.word(0x1c)|=8;
    drive_.boost_options=0x0208;
    for(const auto& o:scene_.objects) {
        if(o.callback_address!=0x09c244 && o.callback_address!=0x09c280 && o.callback_address!=0x09c2c5)continue;
        const int rotation=o.callback_address==0x09c280?-32:o.callback_address==0x09c2c5?32:0;
        gates_.push_back({o.command_offset,o.template_word,o.position,static_cast<std::uint8_t>(o.orientation+rotation)});
        gate_extents_.push_back({rom.u16(o.command_offset+14),rom.u16(o.command_offset+16),rom.u16(o.command_offset+18)});
    }
    clock_tuning_=extract_easy_ride_clock(rom);
    // CPU03:EF81 loads a class-specific BCD time with no incoming championship carry.
    const auto bcd=rom.bytes()[Rom::lorom_offset(0x03d810)+static_cast<unsigned>(league)];
    clock_.remaining_seconds=(bcd>>4)*10+(bcd&15);
}

void SpeedBonus::step(std::uint16_t requested,std::uint8_t ticks,std::optional<std::int16_t> steering) {
    if(ended())return;
    advance_race_clock(clock_,clock_tuning_,ticks);
    if(ended())return;
    auto& body=truck_.body;
    RaceAudioOutput events;
    body.word(0x42)=prepare_player_input(controls_,body,requested,false,events);
    last_steering_=steering;
    const auto x=body.word(0x1e),z=body.word(0x22);
    update_truck(steering);
    // The parked tally was measured at the starting grid. Suspension settles
    // vertically even there. A truck that drove away and stopped uses driving timing.
    if(body.word(0x1e)!=x || body.word(0x22)!=z || body.word(0x4a)!=0)stayed_parked_=false;
    score_crossings();
}

void SpeedBonus::coast_after_timeout() {
    if(!ended())return;
    // CPU08:B508..B50F keeps the last steering and brake during the tally.
    truck_.body.word(0x42)&=0xfbcf;
    update_truck(last_steering_);
}

void SpeedBonus::update_truck(std::optional<std::int16_t> steering) {
    auto& body=truck_.body;
    ++drive_.update_phase;
    refresh_vehicle_contact_candidates(truck_,shapes_,candidates_);
    VehicleStepEnvironment env{};
    env.route=route_;
    env.steering_q15=steering;
    const RacePosition guide_position{std::bit_cast<std::int16_t>(guide_.word(0x1e)),std::bit_cast<std::int16_t>(guide_.word(0x20)),
                                      std::bit_cast<std::int16_t>(guide_.word(0x22))};
    const std::array guide_extents{guide_.word(0xc),guide_.word(0xe),guide_.word(0x10)};
    std::vector<std::uint32_t> overlaps;
    for(const auto& shape:shapes_)if(!(guide_.word(0x1a)&shape.object_flags) &&
        collision_bounds_overlap(guide_position,guide_extents,shape.position,shape.extents))overlaps.push_back(shape.command_offset);
    update_contact_candidates(guide_candidates_,overlaps);
    guide_.word(0x18)=!guide_candidates_.empty();
    env.trailer_guide=&guide_;
    if(!guide_candidates_.empty()) {
        const auto it=std::find_if(shapes_.begin(),shapes_.end(),[&](const auto& shape){return shape.command_offset==guide_candidates_.front();});
        if(it!=shapes_.end())env.trailer_guide_surface=&*it;
    }
    env.wheel_candidate_lists=&candidates_;
    std::array<VehicleState*,1> participant{&body};
    env.racers=participant;
    env.player_rank=1;
    for(std::size_t i=0;i<truck_.wheels.size();++i) {
        truck_.wheels[i].word(0x1c)&=std::uint16_t(~4u);
        truck_.wheels[i].word(0x18)=!candidates_[i].empty();
        if(candidates_[i].empty())continue;
        const auto it=std::find_if(shapes_.begin(),shapes_.end(),[&](const auto& shape){return shape.command_offset==candidates_[i].front();});
        if(it!=shapes_.end())env.wheel_candidates[i]=&*it;
    }
    for(int half=0;half<2;++half) {
        drive_.damage_mode=static_cast<std::uint16_t>(half);
        update_player_vehicle(truck_,tuning_,wheel_tuning_,direction_,drive_,env);
    }
    body.word(0x1c)&=std::uint16_t(~4u);
}

void SpeedBonus::score_crossings() {
    const auto& body=truck_.body;
    if(body.word(0x7a)>reward_.extra_cars) {
        reward_.extra_cars=body.word(0x7a);
        for(auto& gate:gates_)gate.frame=0;
    }
    const RacePosition center{std::bit_cast<std::int16_t>(body.word(0x1e)),std::bit_cast<std::int16_t>(body.word(0x20)),
                              std::bit_cast<std::int16_t>(body.word(0x22))};
    const std::array extents{body.word(0xc),body.word(0xe),body.word(0x10)};
    for(std::size_t i=0;i<gates_.size();++i) {
        auto& gate=gates_[i];
        if(gate.frame==1){gate.frame=2;continue;}
        if(!gate.frame && collision_bounds_overlap(center,extents,position(gate.position),gate_extents_[i])) {
            gate.frame=1;
            ++reward_.extra_seconds; // 09:C37A adds one decimal second per pair.
        }
    }
}
}
