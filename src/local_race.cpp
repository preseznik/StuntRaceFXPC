#include "srf/local_race.hpp"
#include <algorithm>

namespace srf {
namespace {
Scene read_local_scene(const Rom& rom,unsigned index) {
    const auto address=rom.slice(Rom::lorom_offset(0x03f4a5)+3*index,3);
    return extract_scene(rom,Rom::lorom_offset(address[0]|address[1]<<8|address[2]<<16));
}
RacePosition position(const VehicleState& b){
    return {std::int16_t(b.word(0x1e)),std::int16_t(b.word(0x20)),std::int16_t(b.word(0x22))};
}
std::array<std::uint16_t,3> bounds(const VehicleState& b){return {b.word(12),b.word(14),b.word(16)};}
}
LocalRace::LocalRace(const Rom& rom,LocalCourse course,PlayerCar first,PlayerCar second,bool cpu,std::span<const PlayerCar> extra)
    :scene_(supports_lan_course(course.index)?extract_course_scene(rom,static_cast<CourseId>(course.index)):read_local_scene(rom,course.index)),route_(extract_route(scene_)),shapes_(extract_collision(rom,scene_)),
     opponents_(extract_opponent_tuning(rom)),count_(course.index==practice_course.index?1:2+unsigned(extra.size())),cpu_(cpu && count_==2),lan_(supports_lan_course(course.index)) {
    if(count_>max_lan_drivers || (!lan_ && !extra.empty()))throw std::invalid_argument("Invalid local grid size");
    grid_.resize(count_);
    ravine_=course.index==static_cast<unsigned>(CourseId::big_ravine);
    if(course.index==static_cast<unsigned>(CourseId::kings_forest) || course.index==static_cast<unsigned>(CourseId::lake_side) || ravine_){
        streamed_course_=extract_streaming(rom,scene_);
        std::erase_if(streamed_course_.objects,[&](const StreamObject& entry){
            const auto shape=std::ranges::find(shapes_,entry.command_offset,&CollisionShape::command_offset);
            return shape==shapes_.end() || !shape->animated_planes;
        });
        bridges_=CourseBridges(rom,scene_,shapes_,streamed_course_);
        if(ravine_){
            ice_.emplace(rom);
            balls_.emplace(scene_);
            for(const auto& prop:extract_props(rom,scene_))
                if(prop.callback_address==0x09aa33 || prop.callback_address==0x09aabc || prop.callback_address==0x09ab56)
                    grip_gates_.push_back(prop);
        }
    }else if(course.index==static_cast<unsigned>(CourseId::sky_ramp)){
        streamed_course_.normal=extract_streaming(rom,scene_).normal;
        aircraft_.emplace(rom);
        for(const auto& prop:extract_props(rom,scene_))
            if(prop.callback_address==0x099bd7)aircraft_gate_=prop;
        if(!aircraft_gate_)throw std::runtime_error("Sky Ramp aircraft trigger missing");
    }else if(course.index==static_cast<unsigned>(CourseId::sea_breeze) || course.index==static_cast<unsigned>(CourseId::night_cruise) || course.index==static_cast<unsigned>(CourseId::harbor_city)){
        streamed_course_=extract_streaming(rom,scene_);
        props_.emplace(rom,scene_,static_cast<CourseId>(course.index));
        if(course.index==static_cast<unsigned>(CourseId::night_cruise)){
            cruise_.emplace(scene_);
            for(const auto& prop:extract_props(rom,scene_))
                if(prop.callback_address==0x09937f || prop.callback_address==0x0993ed)cruise_gates_.push_back(prop);
        }
        for(const auto& pose:props_->poses())
            streamed_course_.objects.push_back({pose.command_offset,original_spatial_cell(std::int16_t(pose.position.x),std::int16_t(pose.position.z)),0});
        std::ranges::sort(streamed_course_.objects,{},&StreamObject::command_offset);
    }else if(course.index==static_cast<unsigned>(CourseId::white_land)){
        streamed_course_=extract_streaming(rom,scene_);
        ice_.emplace(rom);
        balls_.emplace(scene_);
        for(const auto& prop:extract_props(rom,scene_)){
            if(prop.callback_address==0x09a06a)snowmen_.push_back(prop);
            if(prop.callback_address==0x09a458 || prop.callback_address==0x09a4e7 || prop.callback_address==0x09a574)
                grip_gates_.push_back(prop);
        }
        for(const auto& prop:snowmen_){
            const auto p=position(prop.state);
            streamed_course_.objects.push_back({prop.command_offset,original_spatial_cell(p[0],p[2]),0});
        }
        std::ranges::sort(streamed_course_.objects,{},&StreamObject::command_offset);
    }
    if(route_.empty())throw std::runtime_error("Local course has no driving route");
    for(unsigned i=0;i<count_;++i){
        GridSlot spawn{};
        if(lan_) {
            spawn=scene_.grid.at(i).value();
        } else {
            const auto object=std::ranges::find(scene_.objects,i?0x08afd7u:0x08af0fu,&SceneObject::callback_address);
            if(object==scene_.objects.end())throw std::runtime_error("Local course is missing its grid");
            spawn={object->position,object->orientation};
            spawn.position.y=route_[0].position[1];
        }
        const unsigned type=static_cast<unsigned>(count_==1?PlayerCar::four_wd:i>=2?extra[i-2]:i?second:first);
        const bool ai=i && cpu_;
        const auto definition=rom.u16(Rom::lorom_offset(ai?0x03b704:0x03b6f1)+2*type);
        const RacePosition start{std::int16_t(spawn.position.x),std::int16_t(spawn.position.y-(ai?480:160)),std::int16_t(spawn.position.z)};
        const auto color=rom.bytes()[Rom::lorom_offset(0x03b7b1)+type]&3;
        grid_[i]=create_vehicle(rom.bytes(),definition,start,spawn.orientation<<8,ai?4:1,color,scene_.surface_flags);
        auto& b=grid_[i].body;
        if(ai){b.word(0x64)|=8;b.word(0x40)|=0x0200;}
        else b.word(0x1c)|=8;
        tuning_[i]=extract_vehicle_tuning(rom.bytes(),b.word(0x6e));
        wheels_[i]=extract_wheel_contact_tuning(rom,tuning_[i].contact_parameters);
    }
    if(course.index==static_cast<unsigned>(CourseId::sea_breeze))spray_.emplace(rom,grid_);
    if(course.index==static_cast<unsigned>(CourseId::harbor_city))spray_.emplace(rom,grid_,std::array<std::uint8_t,4>{0x28,0xa7,0x55,0x7f});
    reset();
}
void LocalRace::reset(){
    if(aircraft_){aircraft_->reset();aircraft_triggered_=aircraft_pending_=false;}
    if(props_){
        props_->reset();
        for(const auto& pose:props_->poses()){
            auto entry=std::ranges::find(streamed_course_.objects,pose.command_offset,&StreamObject::command_offset);
            entry->cell_offset=original_spatial_cell(std::int16_t(pose.position.x),std::int16_t(pose.position.z));
        }
    }
    if(cruise_){
        cruise_->reset();cruise_qualified_={};
        flyover_driver_.reset();boost_contact_.reset();boost_owner_.reset();
    }
    if(spray_)spray_->reset();
    if(ice_){ice_->reset();storm_phases_={};ravine_traction_={};gate_contacts_.assign(grip_gates_.size(),{});}
    if(balls_)balls_->reset();
    stream_=reset_collision_stream(streamed_course_);
    bridges_.reset(shapes_);
    cars_=grid_;candidates_={};contexts_={};inputs_={};phase_=0;result_.reset();
    used_={};computer_={false,cpu_};
}
const CollisionShape* LocalRace::wheel_surface(unsigned driver,unsigned wheel) const {
    const auto& list=candidates_.at(driver).at(wheel);
    if(list.empty())return nullptr;
    const auto it=std::ranges::find(shapes_,list.front(),&CollisionShape::command_offset);
    return it==shapes_.end()?nullptr:&*it;
}
std::vector<std::uint16_t> LocalRace::standings() const {
    std::array<RouteProgress,max_lan_drivers> progress{};
    for(unsigned i=0;i<count_;++i){
        const auto& body=cars_[i].body;
        progress[i]={body.word(0x58),body.word(0x86),body.word(0x7a),body.word(0x84)};
    }
    return rank_racers(std::span(progress).first(count_));
}

std::vector<ActivationCell> LocalRace::active_cells() const {
    std::vector<ActivationCell> cells;
    for(const auto& car:cars_){
        const auto p=position(car.body);
        const auto area=activation_cells(streamed_course_.normal,p[0],p[2]);
        cells.insert(cells.end(),area.begin(),area.end());
    }
    return cells;
}
void LocalRace::step_aircraft(){
    const auto& gate=*aircraft_gate_;
    const auto p=position(gate.state);
    if(aircraft_pending_ && !aircraft_triggered_){
        aircraft_->spawn(gate.command_offset,{p[0],p[1],p[2]});
        aircraft_triggered_=true;
    }
    aircraft_->step(active_cells());
    aircraft_->record_contacts(cars_);
    aircraft_pending_=!aircraft_triggered_ && std::ranges::any_of(cars_,[&](const Vehicle& car){
        const auto& b=car.body;
        return b.word(0x7a)==1 && (b.word(0x16)&2) && (b.word(0x64)&1) && !(b.word(0x16)&gate.state.word(0x1a)) &&
            collision_bounds_overlap(position(b),bounds(b),p,bounds(gate.state));
    });
}
void LocalRace::step_cruise(){
    cruise_->begin_phase();
    for(const auto& gate:cruise_gates_){
        bool touched=false;
        for(unsigned i=0;i<cars_.size();++i){
            const auto& car=cars_[i].body;
            if((gate.state.word(0x1a)&car.word(0x16)) || !(car.word(0x16)&2) || !(car.word(0x64)&1) ||
               !collision_bounds_overlap(position(car),bounds(car),position(gate.state),bounds(gate.state)))continue;
            if(gate.callback_address==0x09937f){cruise_qualified_[i]=true;touched=true;}
            else if(cruise_qualified_[i] && !flyover_driver_){flyover_driver_=i;touched=true;}
        }
        if(touched)cruise_->touch(gate.command_offset);
    }
    const auto cells=active_cells();
    auto& target=cars_[flyover_driver_.value_or(0)].body;
    auto recipient=boost_owner_ ? boost_owner_ : boost_contact_;
    cruise_->cpu_step(target,cells,recipient?&cars_[*recipient].body:nullptr);
    if(std::ranges::find(cruise_->effects(),0x25)!=cruise_->effects().end())boost_owner_=boost_contact_;
    boost_contact_.reset();
    // Simultaneous touches use grid order, independently of which peer is rendering.
    for(unsigned i=0;i<cars_.size();++i)if(cruise_->can_collect(cars_[i].body)){boost_contact_=i;break;}
    cruise_->record_contact(cars_[boost_contact_.value_or(0)].body);
}

void LocalRace::step_falling_objects(){
    ice_->begin_phase();
    const auto touches=[](const VehicleState& car,const NativeProp& prop){
        return !(prop.state.word(0x1a)&car.word(0x16)) && (car.word(0x16)&2) && (car.word(0x64)&1) &&
            collision_bounds_overlap(position(car),bounds(car),position(prop.state),bounds(prop.state));
    };
    for(std::size_t i=0;i<grip_gates_.size();++i){
        for(std::size_t j=0;j<cars_.size();++j){
            auto& car=cars_[j].body;
            const bool contact=touches(car,grip_gates_[i]);
            const auto callback=grip_gates_[i].callback_address;
            const auto lap=car.word(0x7a);
            if(ravine_ && contact && !gate_contacts_[i][j]){
                if(callback==0x09aa33 && lap==0 && !ravine_traction_[j]){ravine_traction_[j]=true;car.word(0x8a)|=1;}
                else if(callback==0x09aabc && lap==0 && ravine_traction_[j])car.word(0x8a)|=1;
                else if(callback==0x09ab56 && lap!=2 && ravine_traction_[j])car.word(0x8a)&=0x00fe;
            }else if(!ravine_ && contact)update_white_land_grip(car,callback,!gate_contacts_[i][j],lap,phase_,storm_phases_[j]);
            gate_contacts_[i][j]=contact;
        }
    }
    for(const auto& prop:snowmen_){
        const bool active=std::ranges::find(stream_.active_command_offsets,prop.command_offset)!=stream_.active_command_offsets.end();
        const bool hit=active && std::ranges::any_of(cars_,[&](const Vehicle& car){
            return touches(car.body,prop);
        });
        ice_->update_solid(prop,active,hit);
    }
    // Both cars keep the falling objects active. On the leading lap, the first car
    // to approach the crest triggers the pair once for the whole lobby.
    const auto trigger=balls_->trigger_position();
    const VehicleState* driver=&cars_[0].body;
    const auto priority=[&](const VehicleState& b){
        const auto p=position(b);
        return std::pair{b.word(0x7a),-std::max(std::abs(int(p[0])-trigger[0]),std::abs(int(p[2])-trigger[2]))};
    };
    for(const auto& car:cars_)if(priority(car.body)>priority(*driver))driver=&car.body;
    const auto cells=active_cells();
    const auto cell=original_spatial_cell(trigger[0],trigger[2]);
    balls_->cpu_step(std::ranges::find(cells,cell,&ActivationCell::offset)!=cells.end(),*driver,driver->word(0x7a),cells);
    if(ravine_){
        for(const auto& p:balls_->fragment_origins())ice_->spawn_rock_chips(balls_->trigger_id(),p);
    }else ice_->spawn_snowball_fragments(*balls_);
    ice_->submit_fragments();
    if(ravine_)balls_->append_debris(ice_->meshes());
    const std::array rocks{balls_->body(SunsetRocks::first_body_id),balls_->body(SunsetRocks::first_body_id+1)};
    ice_->integrate(shapes_,stream_.active_command_offsets,snowmen_,cars_,
                    ravine_?std::span<const VehicleState* const>(rocks):std::span<const VehicleState* const>{});
    balls_->integrate(shapes_,cars_);
}

void LocalRace::step(const std::array<std::uint16_t,max_lan_drivers>& buttons,const std::array<std::optional<std::int16_t>,max_lan_drivers>& steering){
    if(result_)return;
    ++phase_;
    if(!streamed_course_.objects.empty()){
        // Either driver can activate scenery. Both peers step it once on the shared race clock.
        for(const auto& car:cars_){
            const auto p=position(car.body);
            refresh_collision_stream(streamed_course_,stream_,p[0],p[2]);
        }
        bridges_.advance(stream_,shapes_);
        update_collision_stream(streamed_course_,stream_);
        if(props_)props_->cpu_step(streamed_course_,stream_);
    }
    if(ice_)step_falling_objects();
    if(cruise_)step_cruise();
    if(aircraft_)step_aircraft();
    if(spray_)spray_->step(cars_);
    if(props_)props_->integrate(shapes_,stream_.active_command_offsets,cars_);
    std::array<VehicleState*,max_lan_drivers> participants{};
    std::array<std::array<bool,max_lan_drivers>,max_lan_drivers> touches{};
    for(unsigned i=0;i<count_;++i){
        participants[i]=&cars_[i].body;
        for(unsigned j=0;j<i;++j){
            const auto& a=cars_[j].body;const auto& b=cars_[i].body;
            touches[i][j]=touches[j][i]=collision_bounds_overlap(position(a),bounds(a),position(b),bounds(b)) && body_pair_overlap_filter(a,b);
        }
    }
    std::array<bool,max_lan_drivers> finished{},failed{};
    for(unsigned i=0;i<count_;++i){
        auto& car=cars_[i];auto& body=car.body;
        auto& ctx=contexts_[i];ctx.update_phase=std::uint16_t(phase_);ctx.boost_options=scene_.surface_flags;
        body.word(0x90)=body.word(0x40);
        note_input(i,buttons[i]);
        if(!lan_ && count_==2 && !used_[i] && phase_==70 && !computer_[i]){
            computer_[i]=true;
            body.word(0x64)|=8;body.word(0x40)|=0x0200;
        }
        if(!computer_[i]){
            RaceAudioOutput events;
            body.word(0x42)=prepare_player_input(inputs_[i],body,buttons[i],false,events);
        }
        auto& contacts=candidates_[i];
        refresh_vehicle_contact_candidates(car,shapes_,contacts);
        VehicleStepEnvironment env{};
        env.route=route_;env.racers=std::span(participants).first(count_);env.opponent_index=i;
        env.player_rank=1;env.wheel_candidate_lists=&contacts;env.steering_q15=steering[i];
        std::vector<BodyContactCandidate> prop_contacts;
        for(unsigned j=0;j<count_;++j)if(touches[i][j]){
            const auto& other=cars_[j].body;
            prop_contacts.push_back({{other.word(0x1e),other.word(0x22)},{other.word(0x2e),other.word(0x32)},&other});
        }
        if(props_ || ice_ || aircraft_){
            const auto include=[&](const VehicleState* object){
                if(object && !(body.word(0x1a)&object->word(0x16)) &&
                   collision_bounds_overlap(position(body),bounds(body),position(*object),bounds(*object)))
                    prop_contacts.push_back({{object->word(0x1e),object->word(0x22)},{object->word(0x2e),object->word(0x32)},object});
            };
            if(props_)for(const auto& pose:props_->poses())include(props_->body(pose.command_offset));
            if(cruise_)include(cruise_->body());
            if(aircraft_)include(aircraft_->body());
            if(ice_){
                for(const auto& prop:snowmen_)if(!ice_->solid_retired(prop.command_offset))include(&prop.state);
                for(unsigned i=0;i<2;++i)include(balls_->body(SunsetRocks::first_body_id+i));
                for(const auto id:ice_->overlapping(body))include(ice_->body(id));
            }
        }
        env.body_candidates=prop_contacts;
        body.word(0x18)=!env.body_candidates.empty();
        for(unsigned w=0;w<4;++w){
            car.wheels[w].word(0x1c)&=std::uint16_t(~4u);
            car.wheels[w].word(0x18)=!contacts[w].empty();
            env.wheel_candidates[w]=wheel_surface(i,w);
        }
        for(unsigned half=(body.word(0x1c)&8)?0:1;half<2;++half){
            ctx.damage_mode=half;
            update_vehicle(car,tuning_[i],wheels_[i],opponents_,ctx,env);
        }
        body.word(0x1c)&=std::uint16_t(~4u);
        finished[i]=body.word(0x7a)>=3;
        failed[i]=race_failure(body.word(0x40))!=RaceFailure::none;
    }
    if(!streamed_course_.objects.empty())finish_collision_stream(streamed_course_,stream_);
    if(count_==1){
        if(failed[0])result_=2;
        else if(finished[0])result_=0;
        return;
    }
    // Battle regenerates boost between uses, with a faster refill for the trailing car.
    if(!lan_ && phase_%8==0){
        const auto ranks=standings();
        for(unsigned i=0;i<2;++i)if(!(buttons[i]&0x400))
            apply_race_pickup(cars_[i].body,RacePickup::boost,std::uint8_t(ranks[i]));
    }
    unsigned survivors=0,finishers=0,winner=count_,survivor=count_;
    for(unsigned i=0;i<count_;++i)if(!failed[i]){
        ++survivors;survivor=i;
        if(finished[i]){++finishers;winner=i;}
    }
    if(finishers)result_=finishers==1?winner:count_;
    else if(survivors<=1)result_=survivor;
}
}
