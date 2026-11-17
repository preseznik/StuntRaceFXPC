#include "srf/rock_field_rules.hpp"
#include "srf/stunt_course.hpp"
#include <algorithm>
#include <bit>

namespace srf {
namespace {
RacePosition coordinates(Vertex v) {
    return {static_cast<std::int16_t>(v.x),static_cast<std::int16_t>(v.y),static_cast<std::int16_t>(v.z)};
}
}

RockFieldRules::RockFieldRules(const Rom& rom, const Scene& scene, PlayerCar car)
    : tuning_(extract_easy_ride_clock(rom)) {
    // CPU03:EF5D..EF6E indexes four vehicle slots per Stunt course.
    const auto entry=std::ranges::find_if(playable_stunt_courses,[&](StuntCourse candidate) {
        const auto p=rom.slice(Rom::lorom_offset(0x03f4a5)+3*stunt_rom_index(candidate),3);
        return Rom::lorom_offset(p[0]|(p[1]<<8)|(p[2]<<16))==scene.start_offset;
    });
    if(entry==playable_stunt_courses.end())
        throw std::invalid_argument("Unsupported Stunt Trax scene");
    const auto course=*entry;
    const auto streaming=extract_streaming(rom,scene);
    mask_=course==StuntCourse::rock_field?streaming.normal:streaming.compact;
    constexpr std::array<unsigned,3> car_slots{1,2,0}; // COUPE, 4WD, F-TYPE -> 0D89
    const auto table=Rom::lorom_offset(0x03d8a3)+unsigned(course)*4;
    const auto packed=rom.bytes()[table+car_slots.at(static_cast<unsigned>(car))];
    clock_.remaining_seconds=10*(packed>>4)+(packed&15);
    unsigned triggers=0, gates=0, goals=0;
    for(const auto& obj:scene.objects) {
        const auto cb=obj.callback_address;
        if(cb==0x09bee8 || cb==0x09bfb0) {
            const auto offset=obj.command_offset;
            const auto pose=static_cast<unsigned>(star_poses_.size());
            star_poses_.push_back({offset,obj.template_word,obj.position,obj.orientation});
            pickups_.push_back({pose,cb==0x09bee8?2u:5u,
                {rom.u16(offset+14),rom.u16(offset+16),rom.u16(offset+18)},
                original_spatial_cell(static_cast<std::int16_t>(obj.position.x),static_cast<std::int16_t>(obj.position.z))});
        } else if(cb==0x09b02a || cb==0x09b133 || cb==0x09b21f) {
            auto& gate=gates_[cb==0x09b02a?0:cb==0x09b133?1:2];
            gate.pose=static_cast<unsigned>(gate_poses_.size());
            gate_poses_.push_back({obj.command_offset,obj.template_word,obj.position,obj.orientation});
            ++gates;
        } else if(cb==0x09b0be || cb==0x09b1aa || cb==0x09b296) {
            gates_[cb==0x09b0be?0:cb==0x09b1aa?1:2].trigger=obj.position;
            ++triggers;
        } else if(cb==0x09afb7) {
            finish_=obj.position;
            ++goals;
        }
    }
    if(pickups_.size()!=40 || gates!=3 || triggers!=3 || goals!=1)
        throw std::runtime_error("Incomplete Stunt Trax star or checkpoint data");
}

unsigned RockFieldRules::section() const {
    return 1+static_cast<unsigned>(std::count_if(gates_.begin(),gates_.end(),[](const Gate& g){return g.crossed;}));
}

bool RockFieldRules::body_contact(const VehicleState& body) const {
    // The original star and gate bodies reject radial impulses, but an
    // overlap still sets the chassis candidate list used by the damage code.
    const RacePosition position{std::int16_t(body.word(0x1e)),std::int16_t(body.word(0x20)),std::int16_t(body.word(0x22))};
    const std::array bounds{body.word(0xc),body.word(0xe),body.word(0x10)};
    for(const auto& star:pickups_) {
        const auto& pose=star_poses_[star.pose];
        if((!star.collected || star.retiring_updates) &&
           collision_bounds_overlap(position,bounds,coordinates(pose.position),star.extents))return true;
    }
    for(const auto& gate:gates_)
        if(collision_bounds_overlap(position,bounds,coordinates(gate_poses_[gate.pose].position),{1280,1280,1280}))return true;
    return false;
}

void RockFieldRules::advance_clock(std::uint8_t ticks) {
    if(outcome_==RockFieldOutcome::driving)advance_race_clock(clock_,tuning_,ticks);
}

void RockFieldRules::step(const VehicleState& body, std::uint8_t ticks) {
    if(outcome_!=RockFieldOutcome::driving)return;
    const bool timed_out=!clock_.remaining_seconds;
    advance_clock(ticks);
    if(timed_out) {
        outcome_=RockFieldOutcome::timeout;
        return;
    }
    // CPU07:8BBB..8C30 moves saved seconds into the live clock on successive updates.
    if(transferring_) {
        const unsigned amount=transferring_>=18?17:transferring_>=5?3:1;
        transferring_-=amount;
        display_bank_=transferring_;
        extend_race_clock(clock_,static_cast<std::uint16_t>(amount));
    } else if(display_bank_<bank_)++display_bank_; // 07:8BD3..8BE2, blue HUD counter
    const RacePosition at{std::bit_cast<std::int16_t>(body.word(0x1e)),std::bit_cast<std::int16_t>(body.word(0x20)),
                          std::bit_cast<std::int16_t>(body.word(0x22))};
    const std::array bounds{body.word(0xc),body.word(0xe),body.word(0x10)};
    const auto cells=activation_cells(mask_,at[0],at[2]);
    for(auto& star:pickups_) {
        if(star.collected) {
            if(star.retiring_updates)--star.retiring_updates;
            continue;
        }
        auto& pose=star_poses_[star.pose];
        // The 65816 consumes the preceding GSU update's overlap list.
        if(star.contact) {
            // CPU09:C072 always increments stars once. BF99 and C05B add different amounts to the bank.
            ++stars_;
            bank_+=star.reward;
            star.collected=true;
            // The CPU hides the star now. Its GSU broadphase node survives
            // the following unrefreshed object passes (4WD trace 514..515).
            star.retiring_updates=2;
            pose.visible=false;
            continue;
        }
        const bool active=std::ranges::any_of(cells,[&](ActivationCell c){return c.offset==star.cell;});
        // The streamed actor gets one more callback after its cell stops refreshing.
        // Its vertical velocity survives deactivation and resumes on re-entry.
        const bool advance=active || star.active_last_update;
        star.active_last_update=active;
        if(!advance)continue;
        star.contact=collision_bounds_overlap(at,bounds,coordinates(pose.position),star.extents);
        // CPU09:C11F animates the collision center as well as the visible star.
        star.velocity+=star.acceleration;
        if(star.velocity==14 || star.velocity==-14)star.acceleration=-star.acceleration;
        pose.position.y+=star.velocity;
        pose.yaw+=8;
        pose.frame=(pose.frame+1)%10;
    }
    for(auto& gate:gates_) {
        auto& pose=gate_poses_[gate.pose];
        if(gate.crossed) {
            // B059 starts the private counter at four with the visible frame at zero.
            if(pose.frame<8)pose.frame=pose.frame==0?5:pose.frame+1;
        } else if(gate.contact) {
            gate.crossed=true;
            last_gate_command_=pose.command_offset;
            transferring_=bank_;
            bank_=0;
        } else gate.contact=collision_bounds_overlap(at,bounds,coordinates(gate.trigger),{960,960,960});
    }
    // The invisible goal is beyond the arch. Its handler has no minimum-star or lap check.
    if(finish_contact_) {
        transferring_=bank_;
        bank_=0;
        outcome_=RockFieldOutcome::finished;
    } else finish_contact_=collision_bounds_overlap(at,bounds,coordinates(finish_),{1920,1920,1920});
}
}
