#include "srf/rock_field.hpp"
#include <algorithm>

namespace srf {
namespace {
struct IntroEye { short tick,y,z,pitch; };
#include "rock_field_intro.inc"
namespace up_down {
#include "up_n_down_intro.inc"
}
namespace ice_dance {
#include "ice_dance_intro.inc"
}
struct Intro {std::span<const short> fields;std::span<const IntroEye> eye;unsigned release_field,clock_field;};
constexpr Intro intros[]{
    {fields0,eye0,453,454},{fields1,eye1,465,466},{fields2,eye2,448,449},
    {up_down::fields0,up_down::eye0,457,458},{up_down::fields1,up_down::eye1,456,457},
    {up_down::fields2,up_down::eye2,455,456},
    {up_down::after_finish::fields0,up_down::after_finish::eye0,453,453},
    {up_down::after_finish::fields1,up_down::after_finish::eye1,454,455},
    {up_down::after_finish::fields2,up_down::after_finish::eye2,454,454},
    {up_down::after_timeout::fields0,up_down::after_timeout::eye0,454,455},
    {up_down::after_timeout::fields1,up_down::after_timeout::eye1,454,455},
    {up_down::after_timeout::fields2,up_down::after_timeout::eye2,458,458},
    {ice_dance::first::fields0,ice_dance::first::eye0,451,452},
    {ice_dance::first::fields1,ice_dance::first::eye1,461,461},
    {ice_dance::first::fields2,ice_dance::first::eye2,457,457},
    {ice_dance::after_finish::fields0,ice_dance::after_finish::eye0,453,453},
    {ice_dance::after_finish::fields1,ice_dance::after_finish::eye1,460,460},
    {ice_dance::after_finish::fields2,ice_dance::after_finish::eye2,462,463},
    {ice_dance::after_timeout::fields0,ice_dance::after_timeout::eye0,450,451},
    {ice_dance::after_timeout::fields1,ice_dance::after_timeout::eye1,453,453},
    {ice_dance::after_timeout::fields2,ice_dance::after_timeout::eye2,466,466}
};

Scene original_scene(const Rom& rom,StuntCourse course) {
    const auto pointer=rom.slice(Rom::lorom_offset(0x03f4a5)+3*stunt_rom_index(course),3);
    return extract_scene(rom,Rom::lorom_offset(pointer[0]|(pointer[1]<<8)|(pointer[2]<<16)));
}
RacePosition point(Vertex v) {return {std::int16_t(v.x),std::int16_t(v.y),std::int16_t(v.z)};}
CollisionShape actor_surface(const Rom& rom,const SceneObject& object,std::uint16_t descriptor,
                            std::array<std::uint16_t,3> bounds,std::uint16_t flags) {
    CollisionShape shape{object.command_offset,descriptor,point(object.position),bounds,object.orientation,flags,{}};
    const auto model=extract_world_model(rom,shape.descriptor_address);
    std::vector<std::vector<CollisionPlane>> frames(model.frames.size());
    for(std::size_t f=0;f<frames.size();++f)for(const auto& face:model.primitives) {
        if(!face.attributes[0])continue;
        CollisionPlane plane{point(model.frames[f].at(face.indices.front())),{},face.attributes[0]};
        for(unsigned i=0;i<3;++i)
            plane.normal[i]=static_cast<std::int16_t>(-static_cast<std::int8_t>(face.attributes[3+i])*256);
        frames[f].push_back(plane);
    }
    shape.planes=frames.front();
    if(frames.size()>1)shape.animated_planes=std::make_shared<const decltype(frames)>(std::move(frames));
    return shape;
}
}

RockFieldDrive::RockFieldDrive(const Rom& rom,PlayerCar car,StuntCourse course,StuntAttempt attempt)
    :scene_(original_scene(rom,course)),directions_(extract_opponent_tuning(rom)),
     shapes_(extract_collision(rom,scene_)),intro_(static_cast<unsigned>(car)) {
    const auto car_index=static_cast<unsigned>(car);
    if(course==StuntCourse::up_n_down)intro_+=3*(1+static_cast<unsigned>(attempt));
    if(course==StuntCourse::ice_dance)intro_+=3*(4+static_cast<unsigned>(attempt));
    if(car_index>=3 || intro_>=std::size(intros))throw std::invalid_argument("Unsupported Stunt Trax launch");
    const auto marker=std::ranges::find(scene_.objects,0x08edddu,&SceneObject::callback_address);
    const auto spawn=std::ranges::find(scene_.objects,0x08af0fu,&SceneObject::callback_address);
    if(marker==scene_.objects.end() || spawn==scene_.objects.end())
        throw std::runtime_error("Stunt Trax trailer or player placement missing");
    trailer_[0]={marker->command_offset,marker->template_word,marker->position,marker->orientation,0,0,15};
    trailer_[1]={marker->command_offset,rom.u16(Rom::lorom_offset(0x08eddf)+1),marker->position,marker->orientation};
    // The truck has no regular scene-command collision descriptor.
    const auto floor=Rom::lorom_offset(0x08ee24);
    shapes_.insert(shapes_.begin(),actor_surface(rom,*marker,rom.u16(floor+3),
        {rom.u16(floor+22),rom.u16(floor+31),rom.u16(floor+40)},0x4181));
    // The three arch callbacks install ECD0 and 0500 bounds at runtime. In
    // particular the inner faces of the pillars deflect the wheels on impact.
    unsigned gate=0;
    for(const auto& object:scene_.objects) {
        if(object.callback_address!=0x09b02a && object.callback_address!=0x09b133 && object.callback_address!=0x09b21f)continue;
        const auto entry=Rom::lorom_offset(object.callback_address);
        gate_shapes_.at(gate++)=shapes_.size();
        shapes_.push_back(actor_surface(rom,object,rom.u16(entry+0x21),
            {rom.u16(entry+0x4c),rom.u16(entry+0x55),rom.u16(entry+0x5e)},8));
    }
    if(gate!=gate_shapes_.size())throw std::runtime_error("Missing Stunt Trax section arch");
    const auto deck=std::ranges::find(scene_.objects,0x08caf6u,&SceneObject::callback_address);
    if(deck!=scene_.objects.end()) {
        const auto shape=std::ranges::find(shapes_,deck->command_offset,&CollisionShape::command_offset);
        if(shape==shapes_.end() || !shape->animated_planes)throw std::runtime_error("Stunt deck collision missing");
        const auto timing=rom.slice(deck->command_offset+22,3);
        auto streaming=extract_streaming(rom,scene_);
        const auto stream_object=std::ranges::find(streaming.objects,deck->command_offset,&StreamObject::command_offset);
        if(stream_object==streaming.objects.end())throw std::runtime_error("Stunt deck activation cell missing");
        auto stream=reset_collision_stream(streaming);
        deck_.emplace(MovingDeck{{deck->command_offset,deck->template_word,deck->position,deck->orientation},
            static_cast<std::size_t>(shape-shapes_.begin()),static_cast<std::size_t>(stream_object-streaming.objects.begin()),
            {timing[0],timing[1],timing[2]},
            std::move(streaming),std::move(stream)});
        deck_->animation.reset();
    }
    auto start=point(spawn->position);
    start[1]-=160;
    const auto definition=rom.u16(Rom::lorom_offset(0x03b6f1)+2*car_index);
    const auto surface=std::uint16_t(rom.u16(scene_.start_offset+5)|rom.u16(scene_.start_offset+10));
    player_=create_vehicle(rom.bytes(),definition,start,spawn->orientation<<8,1,
        rom.bytes()[Rom::lorom_offset(0x03b7b1)+car_index]&3,surface);
    player_.body.word(0x1c)|=8;
    tuning_=extract_vehicle_tuning(rom.bytes(),player_.body.word(0x6e));
    wheels_=extract_wheel_contact_tuning(rom,tuning_.contact_parameters);
    drive_.boost_options=surface;
    drive_.update_phase=1;
    const auto fields=intros[intro_].fields;
    while(phase()<fields.size()+1 && fields[phase()-1]<=0)scripted_step();
}

bool RockFieldDrive::drive_started() const {return field_>=intros[intro_].release_field;}
unsigned RockFieldDrive::first_clock_field() const {return intros[intro_].clock_field;}

std::array<std::int16_t,5> RockFieldDrive::trailer_camera() const {
    const auto samples=intros[intro_].eye;
    const auto eye=std::prev(std::ranges::upper_bound(samples,static_cast<int>(field_),{},&IntroEye::tick));
    return {std::int16_t(trailer_[0].position.x),eye->y,eye->z,eye->pitch,0};
}

bool RockFieldDrive::advance_intro(std::uint16_t requested,std::optional<std::int16_t> steering) {
    if(drive_started())return false;
    ++field_;
    const auto schedule=intros[intro_].fields;
    if(phase()>=schedule.size()+1 || schedule[phase()-1]>int(field_))return false;
    if(drive_started())step(requested,steering);
    else scripted_step();
    return true;
}

void RockFieldDrive::scripted_step() {
    const auto next=phase()+1;
    // 08:EE63 holds $1000 for 50 callbacks. EE9F opens the sixteen-position
    // door, applies automatic throttle, then EEE5 relinquishes the player.
    trailer_[0].frame=std::uint16_t(std::clamp(66-int(next),0,15));
    const auto control=next<=67?0x1000:next<=92?0x1010:next==93?0x0010:0x0040;
    advance_car(control);
    if(next>=93)for(auto& part:trailer_)part.visible=false;
}

void RockFieldDrive::step(std::uint16_t requested,std::optional<std::int16_t> steering) {
    if(!drive_started())return;
    RaceAudioOutput events;
    advance_car(prepare_player_input(input_,player_.body,requested,false,events),steering);
}

void RockFieldDrive::checkpoint_reward(std::uint32_t gate_command) {
    // 09:C095 grants $7F/128 of each gauge. An empty tank recovers 381/384.
    apply_race_pickup(player_.body,RacePickup::boost,0x7f);
    apply_race_pickup(player_.body,RacePickup::repair,0x7f);
    for(const auto index:gate_shapes_)
        if(shapes_[index].command_offset==gate_command)shapes_[index].frame=4;
}

void RockFieldDrive::advance_car(std::uint16_t control,std::optional<std::int16_t> steering) {
    auto& body=player_.body;
    ++drive_.update_phase;
    if(deck_) {
        auto& deck=*deck_;
        // The GSU consumes the last published deck pose. The CPU then writes
        // the next frame while this vehicle phase is still visible to NMI.
        shapes_[deck.shape].frame=static_cast<std::uint8_t>(deck.pose.frame);
        // Up'n Down sets WRAM $1AEC bit 7; CPU02:A27A uses its compact mask.
        refresh_collision_stream(deck.streaming,deck.stream,std::int16_t(body.word(0x1e)),std::int16_t(body.word(0x22)),true);
        const auto& object=deck.stream.objects[deck.stream_index];
        if(object.initialized && object.timer>=2 && object.timer<0x80) {
            deck.animation.advance();
            deck.pose.frame=deck.animation.visible;
        }
        update_collision_stream(deck.streaming,deck.stream);
        finish_collision_stream(deck.streaming,deck.stream);
    }
    for(const auto index:gate_shapes_) {
        auto& frame=shapes_[index].frame;
        if(frame && frame<8)++frame;
    }
    body.word(0x90)=body.word(0x40);
    body.word(0x42)=control;
    auto surfaces=std::span<const CollisionShape>(shapes_).subspan(trailer_[0].visible?0:1);
    if(deck_) {
        // GSU allocations prepend to the collision list. Where neighbouring
        // ramp pieces meet, the first candidate determines the wheel's plane.
        streamed_shapes_.clear();
        if(trailer_[0].visible)streamed_shapes_.push_back(shapes_.front());
        for(const auto command:deck_->stream.active_command_offsets) {
            const auto shape=std::ranges::find(shapes_,command,&CollisionShape::command_offset);
            if(shape!=shapes_.end())streamed_shapes_.push_back(*shape);
        }
        for(const auto index:gate_shapes_)streamed_shapes_.push_back(shapes_[index]);
        surfaces=streamed_shapes_;
    }
    refresh_vehicle_contact_candidates(player_,surfaces,candidates_);
    VehicleStepEnvironment env{};
    env.wheel_candidate_lists=&candidates_;env.player_rank=1;env.steering_q15=steering;
    std::array<VehicleState*,1> participant{&body};env.racers=participant;
    env.frozen=phase()==2;
    for(unsigned i=0;i<4;++i) {
        auto& wheel=player_.wheels[i];
        wheel.word(0x1c)&=std::uint16_t(~4u);
        wheel.word(0x18)=!candidates_[i].empty();
        if(!candidates_[i].empty()) {
            const auto found=std::ranges::find(surfaces,candidates_[i].front(),&CollisionShape::command_offset);
            if(found!=surfaces.end())env.wheel_candidates[i]=&*found;
        }
    }
    for(unsigned half=0;half<2;++half) {
        drive_.damage_mode=half;
        update_player_vehicle(player_,tuning_,wheels_,directions_,drive_,env);
    }
    body.word(0x1c)&=std::uint16_t(~4u);
}
}
