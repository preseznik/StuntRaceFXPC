#pragma once

#include "srf/body_collision.hpp"
#include "srf/race.hpp"
#include "srf/streaming.hpp"
#include <algorithm>
#include <string_view>

namespace srf {

// Easy Ride's first title demonstration uses a single Coupe with the route AI
// enabled on the player body. The cold-start film ends after GSU phase 463.
class DemoDrive {
public:
    static constexpr unsigned last_phase = 463;
    explicit DemoDrive(const Rom& rom) : history_(simulate(rom)) {}

    unsigned tick() const { return tick_; }
    unsigned phase() const { return phase_; }
    unsigned display_phase() const { return display_phase_; }
    const Vehicle& car_at(unsigned phase) const { return history_.at(phase); }
    const Vehicle& car() const { return car_at(display_phase_); }
    std::int16_t dashboard_speed() const {
        return static_cast<std::int16_t>(car_at(digit_phase_).body.word(0x4a));
    }

    void advance() {
        if(tick_ == field_clocks.size()) return;
        const unsigned clocks = field_clocks[tick_++] - '0';
        phase_ += clocks & 1;
        display_phase_ += (clocks >> 1) & 1;
        digit_phase_ += clocks >> 2;
    }

private:
    // The GSU, alternating background pages and OAM digit writes are separate
    // clocks. Only their measured VBlank edges are stored; all poses, wheel
    // transforms and velocities come from the ROM and native vehicle code.
    static constexpr std::string_view field_clocks =
#include "srf/demo_field_timing.inc"
        "1"; // first fully black exit field: final GSU update
    unsigned tick_{}, phase_{8}, display_phase_{5}, digit_phase_{3};
    std::vector<Vehicle> history_;

    static RacePosition position(const VehicleState& state) {
        return {static_cast<std::int16_t>(state.word(0x1e)), static_cast<std::int16_t>(state.word(0x20)),
                static_cast<std::int16_t>(state.word(0x22))};
    }
    static std::array<std::uint16_t,3> extent(const VehicleState& state) {
        return {state.word(0x0c),state.word(0x0e),state.word(0x10)};
    }

    static std::vector<Vehicle> simulate(const Rom& rom) {
        const auto scene=extract_easy_ride(rom);
        const auto route=extract_route(scene);
        const auto shapes=extract_collision(rom,scene);
        auto streaming=extract_streaming(rom,scene);
        auto props=extract_props(rom,scene);
        const auto first_prop=streaming.objects.size();
        for(const auto& prop:props) {
            const auto p=position(prop.state);
            streaming.objects.push_back({prop.command_offset,original_spatial_cell(p[0],p[2]),0});
        }
        auto active=reset_collision_stream(streaming);
        // Easy Ride command $12, first start spot. The constructor raises the
        // body 160 units. CPU03:B6F1 selects the Coupe's original definition.
        constexpr unsigned grid=0x280ea;
        const RacePosition start{static_cast<std::int16_t>(rom.u16(grid+2)),
                                 static_cast<std::int16_t>(rom.u16(grid+4)-160),
                                 static_cast<std::int16_t>(rom.u16(grid+6))};
        const auto options=static_cast<std::uint16_t>(0x100 | rom.u16(0x28005));
        auto car=create_vehicle(rom.bytes(),rom.u16(Rom::lorom_offset(0x03b6f1)),
                                start,static_cast<std::uint16_t>(rom.bytes()[grid+8]<<8),1,0,options);
        auto& body=car.body;
        body.word(0x1c)=8;         // two callbacks per update
        body.word(0x64)|=8;        // AI controls on the player body
        body.word(0x88)=1;
        const auto tuning=extract_vehicle_tuning(rom.bytes(),body.word(0x6e));
        const auto contact=extract_wheel_contact_tuning(rom,tuning.contact_parameters);
        const auto ai=extract_opponent_tuning(rom);
        VehicleDriveContext drive{1,options,0,0};
        std::array<VehicleState*,1> racers{&body};
        VehicleContactCandidates wheels;
        std::vector<bool> touched(props.size()), collected(props.size());
        std::vector<CollisionShape> visible;
        std::vector<std::uint32_t> overlapping,candidates;
        std::vector<BodyContactCandidate> contacts;
        auto previous=position(body);
        std::vector<Vehicle> history;
        history.reserve(last_phase+1);
        history.push_back(car);   // parked pose, before the scene update loop
        history.push_back(car);   // phase 1 finishes actor allocation
        for(unsigned phase=2;phase<=last_phase;++phase) {
            refresh_collision_stream(streaming,active,previous[0],previous[2]);
            update_collision_stream(streaming,active);
            for(std::size_t i=0;i<props.size();++i) {
                auto& prop=props[i];
                const bool live=active.objects[first_prop+i].keepalive;
                update_native_prop(prop,body.word(0x7a)&255,live,touched[i]);
                if(live && touched[i] && !collected[i] &&
                   (prop.callback_address==0x09ad5e || prop.callback_address==0x09aebf)) {
                    collected[i]=true;
                    apply_race_pickup(body,prop.callback_address==0x09ad5e?RacePickup::boost:RacePickup::repair);
                }
            }
            visible.clear();
            overlapping.clear();
            std::fill(touched.begin(),touched.end(),false);
            for(const auto id:active.active_command_offsets) {
                const auto surface=std::find_if(shapes.begin(),shapes.end(),[id](const auto& s){return s.command_offset==id;});
                if(surface!=shapes.end()) visible.push_back(*surface);
                for(std::size_t i=0;i<props.size();++i) {
                    if(props[i].command_offset!=id) continue;
                    const auto& target=props[i].state;
                    if(!collision_bounds_overlap(position(body),extent(body),position(target),extent(target))) continue;
                    touched[i]=!(target.word(0x1a)&body.word(0x16)) && (body.word(0x16)&2) && (body.word(0x64)&1);
                    if(!(body.word(0x1a)&target.word(0x16))) overlapping.push_back(id);
                }
            }
            previous=position(body);
            body.word(0x90)=body.word(0x40);
            ++drive.update_phase;
            VehicleStepEnvironment env{};
            env.route=route;
            env.racers=racers;
            env.player_rank=1;
            env.wheel_candidate_lists=&wheels;
            env.frozen=phase==2;
            if(!env.frozen) {
                refresh_vehicle_contact_candidates(car,visible,wheels);
                update_contact_candidates(candidates,overlapping);
                contacts.clear();
                for(const auto id:candidates) {
                    const auto& target=std::find_if(props.begin(),props.end(),[id](const auto& p){return p.command_offset==id;})->state;
                    contacts.push_back({{target.word(0x1e),target.word(0x22)},{target.word(0x2e),target.word(0x32)},&target});
                }
                body.word(0x18)=!contacts.empty();
                for(std::size_t i=0;i<wheels.size();++i) {
                    car.wheels[i].word(0x18)=!wheels[i].empty();
                    if(!wheels[i].empty()) {
                        const auto id=wheels[i].front();
                        env.wheel_candidates[i]=&*std::find_if(visible.begin(),visible.end(),[id](const auto& s){return s.command_offset==id;});
                    }
                }
            }
            for(auto& w:car.wheels) w.word(0x1c)&=~4;
            env.body_candidates=contacts;
            drive.damage_mode=0;
            update_vehicle(car,tuning,contact,ai,drive,env);
            drive.damage_mode=1;
            update_vehicle(car,tuning,contact,ai,drive,env);
            body.word(0x1c)&=~4;
            if(!body.word(0x18)) candidates.clear();
            finish_collision_stream(streaming,active);
            history.push_back(car);
        }
        return history;
    }
};
}
