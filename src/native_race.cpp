#include "srf/native_race.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace srf {
namespace {
using Word=std::uint16_t;
std::int16_t signed_word(Word value) { return std::bit_cast<std::int16_t>(value); }
RacePosition position(const VehicleState& body) {
    return {signed_word(body.word(0x1e)),signed_word(body.word(0x20)),signed_word(body.word(0x22))};
}
std::array<Word,3> extents(const VehicleState& body) {
    return {body.word(0x0c),body.word(0x0e),body.word(0x10)};
}
Word absolute_word(Word value) { return signed_word(value)<0?Word(0U-value):value; }
// Original scene-command dispatch lengths, CPU08:801F. Parsing is bounded by
// the already verified course scene, including the terminating command04.
constexpr std::array<std::uint8_t,14> command_lengths{0,22,1,1,2,25,22,26,24,9,3,4,4,3};
}

NativeRace::NativeRace(const Rom& rom, PlayerCar car, CourseId course,std::optional<Word> championship_carry,
                       std::optional<GridOrder> championship_grid)
    : NativeRace(rom,car,course,extract_course_scene(rom,course),championship_carry,championship_grid) {}
NativeRace::NativeRace(const Rom& rom, PlayerCar car, CourseId course, const Scene& scene,
                       std::optional<Word> championship_carry,std::optional<GridOrder> championship_grid)
    : course_(course),championship_grid_(championship_grid),opponents_(extract_opponent_tuning(rom)),route_(extract_route(scene)),
      shapes_(extract_collision(rom,scene)),initial_props_(extract_props(rom,scene)),
      streaming_(extract_streaming(rom,scene)),rocks_(scene),movable_props_(rom,scene,course) {
    const auto player_type=static_cast<std::uint8_t>(car);
    if(player_type>2)throw std::invalid_argument("Unsupported player car");
    const auto bytes=rom.bytes();
    std::array<RacePosition,4> grid{};
    std::array<Word,4> yaw{};
    std::array<bool,4> found{};
    for(auto cursor=scene.start_offset;cursor<scene.coordinate_offset;) {
        const auto opcode=bytes[cursor];
        if(opcode==4)break;
        if((opcode&1) || opcode/2>=command_lengths.size() || !command_lengths[opcode/2])
            throw std::runtime_error("Unsupported original grid command");
        const auto length=command_lengths[opcode/2];
        if(cursor+length>scene.coordinate_offset)throw std::runtime_error("Truncated original grid command");
        if(opcode==18) {
            const auto slot=bytes[cursor+1];
            if(slot<1 || slot>4 || found[slot-1])throw std::runtime_error("Invalid original starting grid");
            found[slot-1]=true;
            for(unsigned axis=0;axis<3;++axis)grid[slot-1][axis]=signed_word(rom.u16(cursor+2+axis*2));
            yaw[slot-1]=Word(unsigned(bytes[cursor+8])<<8);
        } else if(opcode==20)surface_options_|=rom.u16(cursor+1);
        cursor+=length;
    }
    if(std::find(found.begin(),found.end(),false)!=found.end())
        throw std::runtime_error("Incomplete original starting grid");
    constexpr GridOrder first_grid{4,3,2,1}; // CPU08:B893 in GSU linked-list order.
    const auto& slots=championship_grid_?*championship_grid_:first_grid;
    if(!std::is_permutation(slots.begin(),slots.end(),first_grid.begin()))
        throw std::invalid_argument("Championship grid must assign all four starting slots");
    // Original selected player plus the three normal opponent types. Selection
    // tables and palette assignment are recovered CPU03:B6E6..B7B4 data.
    constexpr std::array<std::uint8_t,3> opponent_types{0,1,2};
    const auto palette_base=rom.u16(Rom::lorom_offset(0x0bb00c)+player_type*2);
    const auto palette_row=palette_base+3*opponent_types[0]+9*opponent_types[1]+27*opponent_types[2];
    for(std::size_t index=0;index<4;++index) {
        const auto entrant=3-index;
        const bool player=index==player_index();
        const auto definition=player ? rom.u16(Rom::lorom_offset(0x03b6f1)+player_type*2)
            : rom.u16(Rom::lorom_offset(0x03b704)+opponent_types[entrant-1]*2);
        const auto identifier=player ? bytes[Rom::lorom_offset(0x03b7b1)+player_type]&3
            : bytes[Rom::lorom_offset(0x0bb014)+palette_row+entrant-1]&3;
        // CPU08:B8B4 selects model, palette and actor by entrant, then
        // 08:B92B applies position and orientation from the saved place.
        const auto slot=slots[index]-1;
        auto pose=grid[slot];
        pose[1]=signed_word(Word(pose[1]-(player?160:480)));
        initial_[index]=create_vehicle(bytes,definition,pose,yaw[slot],player?1:4,
                                       Word(identifier),surface_options_);
        auto& body=initial_[index].body;
        if(player)body.word(0x1c)|=8; // CPU08:AF55.
        else {body.word(0x64)|=8;body.word(0x40)|=0x0200;} // CPU08:B208.
        tuning_[index]=extract_vehicle_tuning(bytes,body.word(0x6e));
        wheel_tuning_[index]=extract_wheel_contact_tuning(rom,tuning_[index].contact_parameters);
    }
    clock_tuning_=championship_carry || course==CourseId::sea_breeze || course==CourseId::night_cruise || course==CourseId::lake_side || course==CourseId::big_ravine || course==CourseId::sky_ramp || course==CourseId::harbor_city
        ? extract_course_clock(rom,course,initial_[player_index()].body.word(0x66))
        : extract_course_clock(rom,course);
    // CPU07:904A caps the carry at 99. CPU07:8F32 adds it to the grid grant.
    if(championship_carry) clock_tuning_.initial_seconds+=std::min<Word>(*championship_carry,99);
    for(const auto& prop:initial_props_) {
        const auto pose=position(prop.state);
        streaming_.objects.push_back({prop.command_offset,original_spatial_cell(pose[0],pose[2]),0});
    }
    if(course==CourseId::white_land || course==CourseId::big_ravine)breakables_.emplace(rom);
    if(course==CourseId::sky_ramp)sky_aircraft_.emplace(rom);
    if(course==CourseId::night_cruise)cruise_bonus_.emplace(scene);
    if(course==CourseId::sea_breeze)spray_.emplace(rom,initial_);
    if(course==CourseId::harbor_city)spray_.emplace(rom,initial_,std::array<std::uint8_t,4>{0x28,0xa7,0x55,0x7f});
    if(course==CourseId::sea_breeze || course==CourseId::white_land || course==CourseId::night_cruise || course==CourseId::lake_side || course==CourseId::big_ravine || course==CourseId::sky_ramp || course==CourseId::harbor_city) {
        for(std::size_t i=0;i<initial_props_.size();++i) {
            const auto& prop=initial_props_[i];
            if(prop.callback_address!=0x09ad5e && prop.callback_address!=0x09aebf)continue;
            const auto p=position(prop.state);
            const auto descriptor=rom.u16(prop.command_offset+7);
            pickups_.push_back({i,{prop.command_offset,descriptor,{p[0],p[1],p[2]},
                                   static_cast<std::uint8_t>(prop.state.word(0x24)>>8)}});
        }
    }
    if(const auto id=rocks_.trigger_id();id && std::none_of(streaming_.objects.begin(),streaming_.objects.end(),
        [=](const auto& object){return object.command_offset==id;})) {
        const auto pose=rocks_.trigger_position();
        streaming_.objects.push_back({id,original_spatial_cell(pose[0],pose[2]),0});
    }
    for(const auto& cone:movable_props_.poses())
        streaming_.objects.push_back({cone.command_offset,
            original_spatial_cell(signed_word(Word(cone.position.x)),signed_word(Word(cone.position.z))),0});
    std::sort(streaming_.objects.begin(),streaming_.objects.end(),
        [](const auto& a,const auto& b){return a.command_offset<b.command_offset;});
    for(const auto& object:scene.objects) if(object.callback_address==0x08caf6 || object.callback_address==0x08ca45) {
        const auto matches=[&](const auto& o){return o.command_offset==object.command_offset;};
        const auto shape=std::find_if(shapes_.begin(),shapes_.end(),matches);
        const auto stream=std::find_if(streaming_.objects.begin(),streaming_.objects.end(),matches);
        if(shape==shapes_.end() || stream==streaming_.objects.end() || !shape->animated_planes)
            throw std::runtime_error("Animated bridge has no collision shape");
        const auto timing=rom.slice(object.command_offset+22,3);
        bridges_.push_back({std::size_t(shape-shapes_.begin()),std::size_t(stream-streaming_.objects.begin()),
                            {timing[0],timing[1],timing[2]},
                            object.callback_address==0x08ca45?std::optional{timing[0]}:std::nullopt});
        bridge_poses_.push_back({object.command_offset,object.template_word,object.position,object.orientation});
    }
    reset();
}

void NativeRace::reset() {
    if(spray_)spray_->reset();
    if(breakables_)breakables_->reset();
    if(cruise_bonus_)cruise_bonus_->reset();
    storm_phase_.reset();
    for(std::size_t i=0;i<bridges_.size();++i) {
        auto& b=bridges_[i];
        b.animation.reset();
        shapes_[b.shape].frame=b.fixed_pose.value_or(0);
        bridge_poses_[i].frame=shapes_[b.shape].frame;
    }
    rocks_.reset();
    movable_props_.reset();
    for(const auto& cone:movable_props_.poses()) {
        const auto entry=std::find_if(streaming_.objects.begin(),streaming_.objects.end(),
            [&](const auto& object){return object.command_offset==cone.command_offset;});
        entry->cell_offset=original_spatial_cell(signed_word(Word(cone.position.x)),signed_word(Word(cone.position.z)));
    }
    racers_=initial_;
    props_=initial_props_;
    if(sky_aircraft_) {
        sky_aircraft_->reset();
        for(const auto& prop:initial_props_)if(prop.callback_address==0x099bd7 || prop.callback_address==0x099c4a || prop.callback_address==0x099c97) {
            const auto object=std::ranges::find(streaming_.objects,prop.command_offset,&StreamObject::command_offset);
            object->cell_offset=original_spatial_cell(signed_word(prop.state.word(0x1e)),signed_word(prop.state.word(0x22)));
        }
    }
    ravine_traction_armed_=false;
    pickup_poses_.clear();pickup_effects_.clear();
    for(auto& pickup:pickups_) {
        pickup.remaining=0;
        pickup_poses_.push_back(pickup.initial);
    }
    prop_player_contact_.assign(props_.size(),false);
    grip_gate_contact_.assign(props_.size(),false);
    prop_collected_lap_.assign(props_.size(),4); // Original CPU object+0A.
    stream_=reset_collision_stream(streaming_);
    active_shapes_.clear();
    for(auto& lists:wheel_candidates_)for(auto& list:lists)list.clear();
    for(auto& list:body_candidate_ids_)list.clear();
    for(auto& list:body_candidates_)list.clear();
    for(std::size_t i=0;i<racers_.size();++i)cpu_positions_[i]=position(racers_[i].body);
    standings_=championship_grid_.value_or(GridOrder{});
    // The original constructors run after scheduler phase1 (capture8E82).
    drive_={1,surface_options_,0,0};
    completion_={};
    failure_=RaceFailure::none;
    clock_={0,clock_tuning_.initial_seconds,9999};
    frozen_=2; // Original initial3, after the first phase before actor creation.
    opponent_start_delay_={0,5,0}; // CPU08:B1BA/B1C7/B1D4, original slot order.
    countdown_stage_=countdown_ticks_=0;
    timeout_phases_left_=0;
    phase_=0;
}

void NativeRace::update_streaming() {
    const auto& player=cpu_positions_[player_index()];
    refresh_collision_stream(streaming_,stream_,player[0],player[2]);
    // CPU08:CB15 advances only on ordinary active calls. CAF6/CD55 performs
    // initialization (also on reattachment) and leaves the last pose intact.
    for(std::size_t i=0;i<bridges_.size();++i) {
        auto& b=bridges_[i];
        const auto& object=stream_.objects[b.stream];
        if(b.fixed_pose || !object.initialized || object.timer<2 || object.timer>=0x80)continue;
        b.animation.advance();
        shapes_[b.shape].frame=b.animation.visible;
        bridge_poses_[i].frame=b.animation.visible;
    }
    update_collision_stream(streaming_,stream_);
    movable_props_.cpu_step(streaming_,stream_);
    if(breakables_)breakables_->begin_phase();
    if(cruise_bonus_)cruise_bonus_->begin_phase();
    pickup_effects_.clear();
    pickup_poses_.resize(pickups_.size());
    for(std::size_t i=0;i<props_.size();++i) {
        if(props_[i].retirement_age) {
            if(props_[i].retirement_age<3)++props_[i].retirement_age;
            continue;
        }
        const auto id=props_[i].command_offset;
        const auto object=std::find_if(streaming_.objects.begin(),streaming_.objects.end(),
            [id](const auto& value){return value.command_offset==id;});
        const bool active=stream_.objects[static_cast<std::size_t>(object-streaming_.objects.begin())].keepalive;
        update_native_prop(props_[i],static_cast<std::uint8_t>(racers_[player_index()].body.word(0x7a)),
                           active,prop_player_contact_[i]);
        const auto callback=props_[i].callback_address;
        if(cruise_bonus_ && active && prop_player_contact_[i] &&
           (callback==0x09937f || callback==0x0993ed))cruise_bonus_->touch(id);
        if(sky_aircraft_ && active && (callback==0x099bd7 || callback==0x099c4a || callback==0x099c97)) {
            auto& state=props_[i].state;
            if(grip_gate_contact_[i]) {
                // 08:CEC3 copies the relocated CPU trigger into the GSU on its next active call.
                const auto& original=initial_props_[i].state;
                state.word(0x1e)=Word(original.word(0x1e)+(callback==0x099bd7?3840:-3840));
                state.word(0x22)=Word(original.word(0x22)+(callback==0x099c97?-3840:3840));
                object->cell_offset=original_spatial_cell(signed_word(state.word(0x1e)),signed_word(state.word(0x22)));
            } else if(prop_player_contact_[i] && completion_.acknowledged_laps==1) {
                const auto p=position(state);
                if(callback==0x099bd7)sky_aircraft_->spawn(id,{p[0],p[1],p[2]});
                grip_gate_contact_[i]=true;
            }
        }
        if(course_==CourseId::big_ravine) {
            const bool contact=active && prop_player_contact_[i];
            const auto lap=completion_.acknowledged_laps;
            auto& surface=racers_[player_index()].body.word(0x8a);
            if(contact && !grip_gate_contact_[i]) {
                if(callback==0x09aa33 && lap==0) {
                    ravine_traction_armed_=true;surface|=1;props_[i].retirement_age=1;
                } else if(callback==0x09aabc && lap==0 && ravine_traction_armed_)surface|=1;
                else if(callback==0x09ab56 && lap!=2 && ravine_traction_armed_)surface&=0x00fe;
                else if((callback==0x09a621 && lap==2) || (callback==0x09a6f9 && lap==0))props_[i].retirement_age=1;
            }
            grip_gate_contact_[i]=contact;
        }
        if(course_==CourseId::white_land) {
            const bool contact=active && prop_player_contact_[i];
            if(callback==0x09a06a)breakables_->update_solid(props_[i],active,contact);
            if(contact && !grip_gate_contact_[i]) {
                auto& surface=racers_[player_index()].body.word(0x8a);
                // 09:A4CF/A55E change traction only while 7F:3460 is zero.
                // A574 starts the ramp; original phase1071 -> first weight1 at1118.
                const bool clear_weather=!storm_phase_ || phase_-*storm_phase_<47;
                if(callback==0x09a458 && clear_weather)surface&=0x00fd;
                if(callback==0x09a4e7 && clear_weather)surface|=2;
            }
            if(callback==0x09a574 && contact && !storm_phase_ && completion_.acknowledged_laps==1) {
                storm_phase_=phase_;
                racers_[player_index()].body.word(0x8a)&=0x00fd;
            }
            grip_gate_contact_[i]=contact;
        }
        const bool collected=active && prop_player_contact_[i] && prop_collected_lap_[i]!=completion_.acknowledged_laps;
        if(collected && (callback==0x09ad5e || callback==0x09aebf || callback==0x09ac4f || callback==0x09acab)) {
            prop_collected_lap_[i]=completion_.acknowledged_laps;
            if(callback==0x09ac4f || callback==0x09acab) {
                extend_race_clock(clock_,clock_tuning_.checkpoint_seconds);
                if(callback==0x09acab || ((course_==CourseId::lake_side || course_==CourseId::harbor_city) && callback==0x09ac4f))pickup_effects_.push_back(0x41); // 09:AD35.
            }
            else apply_race_pickup(racers_[player_index()].body,
                              callback==0x09ad5e?RacePickup::boost:RacePickup::repair);
        }
        for(std::size_t j=0;j<pickups_.size();++j) {
            auto& pickup=pickups_[j];
            if(pickup.prop!=i)continue;
            auto& pose=pickup_poses_[j];
            if(active) {
                pose.yaw=static_cast<std::uint8_t>(props_[i].state.word(0x24)>>8);
                pose.pitch+=16; // Original ADB6 / AF17 rotate both axes.
                pose.visible=prop_collected_lap_[i]!=completion_.acknowledged_laps;
            }
            if(collected) {
                pickup.remaining=10;
                pickup_effects_.push_back(callback==0x09ad5e?0x25:0x1b);
            }
            // AE60's ten-count child copies the current CPU player position,
            // rises another 40 units each active phase, and draws nine times.
            if(pickup.remaining && --pickup.remaining) {
                const int age=10-pickup.remaining;
                const auto at=position(racers_[player_index()].body);
                const auto angle=static_cast<std::uint8_t>(16*age);
                pickup_poses_.push_back({pose.command_offset,pose.visual_descriptor,
                    {at[0],at[1]-192-40*age,at[2]},angle,angle});
            }
            break;
        }
    }
    active_shapes_.clear();
    for(const auto id:stream_.active_command_offsets) {
        const auto shape=std::find_if(shapes_.begin(),shapes_.end(),[id](const auto& s){return s.command_offset==id;});
        if(shape!=shapes_.end())active_shapes_.push_back(*shape);
    }
    const auto cells=activation_cells(streaming_.normal,player[0],player[2]);
    if(sky_aircraft_)sky_aircraft_->step(cells);
    if(cruise_bonus_)cruise_bonus_->cpu_step(racers_[player_index()].body,cells);
    if(const auto trigger=rocks_.trigger_id()) {
        const auto it=std::find_if(streaming_.objects.begin(),streaming_.objects.end(),
            [trigger](const auto& object){return object.command_offset==trigger;});
        rocks_.cpu_step(stream_.objects[it-streaming_.objects.begin()].keepalive,
                        racers_[player_index()].body,completion_.acknowledged_laps,cells);
    }
    if(breakables_) {
        // 09:8EAF gives the four 9048 impact fragments higher vertical speeds.
        constexpr std::array<FragmentParameters,4> snow{{
            {{40,-85,0},20},{{-40,-85,0},3},{{0,-90,40},10},{{0,-90,-40},1}
        }};
        for(const auto& p:rocks_.fragment_origins()) {
            if(course_==CourseId::big_ravine)breakables_->spawn_rock_chips(rocks_.trigger_id(),p);
            else breakables_->spawn(rocks_.trigger_id(),p,snow);
        }
        breakables_->submit_fragments();
        rocks_.append_debris(breakables_->meshes());
    }
    for(std::size_t index=0;index<player_index();++index) {
        const auto& pose=cpu_positions_[index];
        const auto cell=original_spatial_cell(pose[0],pose[2]);
        // CPU02:CE08 refreshes selected timers to3; CPU03:D388 decrements to2.
        // CPU08:C9A9 resets an unrefreshed1 to2 and sets the far-controller bit.
        const bool near=std::any_of(cells.begin(),cells.end(),[cell](const auto& c){return c.offset==cell && c.kind!=0x23;});
        auto& flags=racers_[index].body.word(0x16);
        flags=near ? Word(flags&~0x0400U) : Word(flags|0x0400U);
    }
}

void NativeRace::refresh_candidates() {
    for(std::size_t i=0;i<racers_.size();++i)
        refresh_vehicle_contact_candidates(racers_[i],active_shapes_,wheel_candidates_[i]);
    std::array<std::vector<std::uint32_t>,4> overlapping;
    if(sky_aircraft_)sky_aircraft_->record_contact(racers_[player_index()].body);
    if(cruise_bonus_)cruise_bonus_->record_contact(racers_[player_index()].body);
    std::fill(prop_player_contact_.begin(),prop_player_contact_.end(),false);
    // Non-solid props are skipped by BD20's force response, but their original
    // candidate presence still affects damage. Preserve them in active order.
    for(const auto id:stream_.active_command_offsets) {
        if(const auto* cone=movable_props_.body(id)) {
            for(std::size_t i=0;i<racers_.size();++i) {
                const auto& car=racers_[i].body;
                if(!(car.word(0x1a)&cone->word(0x16)) &&
                   collision_bounds_overlap(position(car),extents(car),position(*cone),extents(*cone)))
                    overlapping[i].push_back(id);
            }
            continue;
        }
        const auto prop=std::find_if(props_.begin(),props_.end(),[id](const auto& p){return p.command_offset==id;});
        if(prop==props_.end() || prop->retirement_age>=3 || (breakables_ && breakables_->solid_retired(id)))continue;
        const auto& player=racers_[player_index()].body;
        prop_player_contact_[static_cast<std::size_t>(prop-props_.begin())]=
            !(prop->state.word(0x1a)&player.word(0x16)) && (player.word(0x16)&2) &&
            (player.word(0x64)&1) &&
            collision_bounds_overlap(position(player),extents(player),position(prop->state),extents(prop->state));
        for(std::size_t i=0;i<racers_.size();++i) {
            const auto& body=racers_[i].body;
            if(!(body.word(0x1a)&prop->state.word(0x16)) &&
               collision_bounds_overlap(position(body),extents(body),position(prop->state),extents(prop->state)))
                overlapping[i].push_back(id);
        }
    }
    for(std::size_t i=0;i<racers_.size();++i)for(std::size_t j=i+1;j<racers_.size();++j) {
        const auto& a=racers_[i].body;
        const auto& b=racers_[j].body;
        if(!collision_bounds_overlap(position(a),extents(a),position(b),extents(b)))continue;
        if((a.word(0x16)&b.word(0x16)&2) && !body_pair_overlap_filter(a,b))continue;
        if(!(a.word(0x1a)&b.word(0x16)))overlapping[i].push_back(static_cast<std::uint32_t>(j));
        if(!(b.word(0x1a)&a.word(0x16)))overlapping[j].push_back(static_cast<std::uint32_t>(i));
    }
    for(std::size_t i=0;i<racers_.size();++i) {
        if(cruise_bonus_ || sky_aircraft_) {
            const auto* pickup=cruise_bonus_?cruise_bonus_->body():sky_aircraft_->body();
            const auto& car=racers_[i].body;
            if(pickup && !(car.word(0x1a)&pickup->word(0x16)) &&
               collision_bounds_overlap(position(car),extents(car),position(*pickup),extents(*pickup)))
                overlapping[i].push_back(cruise_bonus_?NightCruiseBonus::pickup_id:SkyRampAircraft::body_id);
        }
        if(breakables_) {
            const auto ids=breakables_->overlapping(racers_[i].body);
            overlapping[i].insert(overlapping[i].end(),ids.begin(),ids.end());
        }
        for(unsigned slot=2;slot-- >0;) {
            const auto id=SunsetRocks::first_body_id+slot;
            const auto* rock=rocks_.body(id);
            const auto& car=racers_[i].body;
            if(rock && !(car.word(0x1a)&rock->word(0x16)) &&
               collision_bounds_overlap(position(car),extents(car),position(*rock),extents(*rock)))
                overlapping[i].push_back(id);
        }
        update_contact_candidates(body_candidate_ids_[i],overlapping[i]);
        body_candidates_[i].clear();
        for(const auto id:body_candidate_ids_[i]) {
            const auto prop=std::find_if(props_.begin(),props_.end(),[id](const auto& p){return p.command_offset==id;});
            const auto* rock=rocks_.body(id);
            const auto* cone=movable_props_.body(id);
            const auto* fragment=breakables_?breakables_->body(id):nullptr;
            const auto* bonus=cruise_bonus_ && id==NightCruiseBonus::pickup_id?cruise_bonus_->body():nullptr;
            if(!bonus && sky_aircraft_ && id==SkyRampAircraft::body_id)bonus=sky_aircraft_->body();
            const auto& target=id<racers_.size()?racers_[id].body:rock?*rock:cone?*cone:fragment?*fragment:bonus?*bonus:prop->state;
            body_candidates_[i].push_back({{target.word(0x1e),target.word(0x22)},
                {target.word(0x2e),target.word(0x32)},&target});
        }
        // Native state uses this original field only as a presence predicate;
        // candidates themselves are owned containers, never emulated pointers.
        racers_[i].body.word(0x18)=body_candidates_[i].empty()?0:1;
        for(std::size_t wheel=0;wheel<4;++wheel)
            racers_[i].wheels[wheel].word(0x18)=wheel_candidates_[i][wheel].empty()?0:1;
    }
}

void NativeRace::update_standings() {
    std::array<RouteProgress,4> progress{};
    for(std::size_t i=0;i<racers_.size();++i) {
        const auto& body=racers_[i].body;
        progress[i]={body.word(0x58),body.word(0x86),body.word(0x7a),body.word(0x84)};
    }
    const auto ranks=rank_racers(progress);
    for(std::size_t i=0;i<racers_.size();++i) {
        racers_[i].body.word(0x88)=ranks[i];
        // The first frozen GSU update still ties on all four route distances.
        // The cartridge presents the grid only after the next update resolves
        // them. Keep the visible starting place until the route sort is ready.
        if(!championship_grid_ || phase_>1)standings_[i]=ranks[i];
    }
    // Original04:CF08 also refreshes each opponent's horizontal distance to
    // the player for nearby-car audio; signed addition overflow skips the write.
    const auto& player=racers_[player_index()].body;
    for(std::size_t i=0;i<player_index();++i) {
        auto& body=racers_[i].body;
        const auto x=absolute_word(Word(body.word(0x1e)-player.word(0x1e)));
        const auto z=absolute_word(Word(body.word(0x22)-player.word(0x22)));
        const auto sum=int(signed_word(x))+int(signed_word(z));
        if(sum>=-32768 && sum<=32767)body.word(0x8c)=Word(x+z);
    }
}

void NativeRace::advance_countdown(std::uint8_t ticks) {
    if(!countdown_stage_) {countdown_stage_=1;countdown_ticks_=60;return;}
    if(countdown_stage_>=6)return;
    const bool expired=countdown_ticks_<ticks;
    countdown_ticks_=static_cast<std::uint8_t>(countdown_ticks_-ticks);
    if(expired) {++countdown_stage_;countdown_ticks_=50;}
}

void NativeRace::acknowledge_lap() {
    const auto event=update_race_completion(completion_,racers_[player_index()].body.word(0x7a));
    if(event==LapEvent::first_lap)extend_race_clock(clock_,clock_tuning_.lap_seconds[0]);
    if(event==LapEvent::second_lap)extend_race_clock(clock_,clock_tuning_.lap_seconds[1]);
}

void NativeRace::step(std::uint16_t controls,std::uint8_t elapsed_clock_ticks,
                      std::optional<std::int16_t> steering) {
    if(ended())return;
    const bool held=countdown_active();
    if(!held)advance_race_clock(clock_,clock_tuning_,elapsed_clock_ticks);
    update_streaming();
    // Original07:E595 checks time before damage/lap dispatch. CPU trigger
    // callbacks can add checkpoint time before that check.
    const auto& player=racers_[player_index()].body;
    const bool pending=time_over_pending();
    if(pending) {
        // Speed Trax task 0C (07:ED4C) can still accept a lap or checkpoint.
        // The rank snapshot is taken before the next vehicle update.
        if(static_cast<std::uint8_t>(player.word(0x7a))>completion_.acknowledged_laps) {
            timeout_phases_left_=0;
            acknowledge_lap();
            if(completion_.finished)return;
        } else if(std::abs(int(signed_word(player.word(0x4a))))*15/64<16) {
            timeout_phases_left_=0;
            failure_=RaceFailure::time_out;
        } else if(clock_.remaining_seconds%100!=0) {
            timeout_phases_left_=0; // 07:ED6F reads the low byte of the BCD clock.
        } else if(--timeout_phases_left_==0) {
            failure_=RaceFailure::time_out;
        }
    } else if(!held && (!clock_.remaining_seconds || clock_.elapsed_units>=5999900)) {
        if(championship_grid_)timeout_phases_left_=32;
        else failure_=RaceFailure::time_out;
    } else {
        failure_=race_failure(player.word(0x40));
    }
    if(failure_!=RaceFailure::none)return;
    if(time_over_pending())controls&=0xfbcf; // Keep steering and the service brake.
    if(spray_)spray_->step(racers_);
    for(std::size_t i=0;i<player_index();++i) {
        auto& flags=racers_[i].body.word(0x70);
        if(held)flags|=4;
        else if(opponent_start_delay_[i])--opponent_start_delay_[i];
        else flags&=Word(~4U);
    }
    advance_countdown(elapsed_clock_ticks);
    // CPU08:C9F7 mirrors status and latches positions after cell activation,
    // before the next GSU phase. Streaming therefore uses a one-phase-old pose.
    for(std::size_t i=0;i<racers_.size();++i) {
        racers_[i].body.word(0x90)=racers_[i].body.word(0x40);
        cpu_positions_[i]=position(racers_[i].body);
    }
    if(frozen_)--frozen_;
    ++phase_;
    ++drive_.update_phase;
    if(!frozen_) {
        refresh_candidates();
        if(breakables_) {
            const std::array<const VehicleState*,2> rock_bodies{rocks_.body(SunsetRocks::first_body_id),
                                                                rocks_.body(SunsetRocks::first_body_id+1)};
            breakables_->integrate(active_shapes_,stream_.active_command_offsets,props_,racers_,
                course_==CourseId::big_ravine?std::span<const VehicleState* const>(rock_bodies):std::span<const VehicleState* const>{});
        }
        rocks_.integrate(active_shapes_,racers_);
        movable_props_.integrate(active_shapes_,stream_.active_command_offsets,racers_);
    }
    if(!held)racers_[player_index()].body.word(0x42)=controls;
    std::array<VehicleState*,4> participants{};
    for(std::size_t i=0;i<racers_.size();++i)participants[i]=&racers_[i].body;
    const auto player_rank=racers_[player_index()].body.word(0x88);
    for(std::size_t i=0;i<racers_.size();++i) {
        // Attached wheels precede their owning body in the original active
        // list. Their keepalive is cleared before the body sets it again.
        for(auto& wheel:racers_[i].wheels)wheel.word(0x1c)&=Word(~4U);
        VehicleStepEnvironment environment{};
        environment.route=route_;
        environment.racers=participants;
        environment.body_candidates=body_candidates_[i];
        environment.opponent_index=i;
        environment.player_rank=player_rank;
        environment.wheel_candidate_lists=&wheel_candidates_[i];
        environment.frozen=frozen_!=0;
        if(i==player_index() && !held && !(racers_[i].body.word(0x1c)&0x10) &&
           !(racers_[i].body.word(0x42)&0x1000))environment.steering_q15=steering;
        for(std::size_t wheel=0;wheel<4;++wheel)if(!wheel_candidates_[i][wheel].empty()) {
            const auto id=wheel_candidates_[i][wheel].front();
            const auto candidate=std::find_if(active_shapes_.begin(),active_shapes_.end(),
                [id](const auto& shape){return shape.command_offset==id;});
            if(candidate!=active_shapes_.end())environment.wheel_candidates[wheel]=&*candidate;
        }
        if(racers_[i].body.word(0x1c)&8) {
            drive_.damage_mode=0;
            update_vehicle(racers_[i],tuning_[i],wheel_tuning_[i],opponents_,drive_,environment);
        }
        drive_.damage_mode=1;
        update_vehicle(racers_[i],tuning_[i],wheel_tuning_[i],opponents_,drive_,environment);
        racers_[i].body.word(0x1c)&=Word(~4U);
        if(!racers_[i].body.word(0x18))body_candidate_ids_[i].clear();
    }
    update_standings();
    // CPU07:E5D9 dispatches damage failure before CPU07:E6A8 can acknowledge a lap.
    if(!pending && !time_over_pending() && race_failure(player.word(0x40))==RaceFailure::none)
        acknowledge_lap();
    finish_collision_stream(streaming_,stream_);
}
}
