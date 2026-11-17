#include "srf/body_collision.hpp"
#include "srf/race.hpp"
#include "srf/vehicle_timestep.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Word = std::uint16_t;
Word word(int value) { return static_cast<Word>(value); }
int signed_word(Word value) { return value < 32768 ? int(value) : int(value) - 65536; }
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void unity_and_limits() {
    srf::VehicleTimestep step(1, 1);
    // Every original signed/wrapped input must survive unity pose and anchor
    // conversion. The impulse ledger must not change the original force clamp.
    for (unsigned raw = 0; raw < 65536; ++raw) {
        const auto value = static_cast<Word>(raw);
        const auto impulse = static_cast<Word>(raw * 37U + 6000U);
        require(step.pose_delta(value, raw % 6) == value, "Unity pose changed an original word");
        require(step.attachment_velocity(value, raw % 4, raw % 3) == word(signed_word(value) * 8),
                "Unity attachment changed original wrapped multiply");
        require(step.attachment_window_velocity(value, 1234, raw % 4, raw % 3) == word(signed_word(value) * 8),
                "Unity contact horizon changed original wrapped multiply");
        const auto measured=word(raw*19U);
        require(step.braking_attachment_velocity(measured,value,value,raw%4,raw%3)==measured,
                "Unity braking velocity changed an original word");
        for (unsigned axis = 0; axis < 6; ++axis) {
            const auto expected = axis == 1 || axis >= 3
                ? word(std::clamp(signed_word(value), -3000, 3000)) : value;
            require(step.force_delta(value, impulse, axis) == expected,
                    "Unity mixed force changed original limiter or modulo arithmetic");
        }
    }
    for (unsigned i = 0; i < 40; ++i) {
        require(step.begin_step() && step.ends_callback(), "Unity clock skipped an original callback");
        require(step.callback_end_position(word(-99), word(i)) == word(i), "Unity callback changed pose sample");
    }
    for (const auto ratio : {std::array<unsigned, 2>{0, 30}, {1, 0}, {31, 30}, {1, 1000001}}) {
        bool rejected = false;
        try { const srf::VehicleTimestep invalid(ratio[0], ratio[1]); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid timestep ratio was accepted");
    }
}

void longitudinal_measurement() {
    for(int sign:{-1,1}) {
        srf::VehicleTimestep step(1,4);
        // Translation 10 plus rotation 2 per substep -> 384 units/callback.
        auto measured=step.attachment_window_velocity(word(sign*12),word(sign*384),0,2);
        require(step.braking_attachment_velocity(measured,word(sign*10),word(sign*40),0,2)==word(sign*384),
                "Steady brake velocity changed");
        // Current translation halves before the position history catches up.
        measured=step.attachment_window_velocity(word(sign*12),0,0,2);
        require(step.braking_attachment_velocity(measured,word(sign*10),word(sign*20),0,2)==word(sign*224),
                "Brake velocity retained delayed translation");
        // Translation stops; retained rotational velocity is still 64.
        measured=step.attachment_window_velocity(word(sign*2),0,0,2);
        require(step.braking_attachment_velocity(measured,0,0,0,2)==word(sign*64),
                "Stopping removed rotation or retained translation");
        step.reset_attachment(0);
        measured=step.attachment_window_velocity(word(sign*12),word(sign*384),0,2);
        require(step.braking_attachment_velocity(measured,word(sign*10),word(sign*40),0,2)==word(sign*384),
                "Wheel reset retained translation history");
        step.reset();
        srf::VehicleTimestep fresh(1,4);
        require(step.braking_attachment_velocity(100,7,25,3,0)==fresh.braking_attachment_velocity(100,7,25,3,0),
                "Vehicle reset retained brake history");
    }
}

void longitudinal_force_override() {
    srf::VehicleTuning tuning;
    srf::WheelContactTuning contact;
    contact.sine_bytes[64]=127;contact.linear_transfer=128;contact.displacement_limit=100;
    const std::array<Word,9> orientation{32767,0,0,0,32767,0,0,0,32767};
    for(int speed:{-32000,-400,-1,0,1,400,32000}) {
        srf::VehicleState body,wheel;
        body.word(0x48)=16000;wheel.word(0x50)=1000;
        wheel.word(0x2e)=80;wheel.word(0x32)=word(speed);
        srf::WheelContactWork work;work.minimum_distance=0;work.friction=18000;
        auto explicit_body=body,explicit_wheel=wheel,stopped_body=body,stopped_wheel=wheel;
        auto explicit_work=work,stopped_work=work;
        srf::VehicleForces forces,explicit_forces,stopped_forces;
        srf::finish_wheel_contact(body,wheel,forces,work,tuning,contact,orientation);
        srf::finish_wheel_contact(explicit_body,explicit_wheel,explicit_forces,explicit_work,
            tuning,contact,orientation,srf::RacePosition{80,0,std::int16_t(speed)});
        require(body.words==explicit_body.words && wheel.words==explicit_wheel.words &&
                forces.words==explicit_forces.words && work.local_force==explicit_work.local_force,
                "Explicit original braking velocity changed native force/state");
        srf::finish_wheel_contact(stopped_body,stopped_wheel,stopped_forces,stopped_work,
            tuning,contact,orientation,srf::RacePosition{80,0,0});
        require(stopped_work.local_force[2]==0 && stopped_work.local_force[0]==work.local_force[0] &&
                stopped_work.local_force[1]==work.local_force[1] && stopped_wheel.words==wheel.words,
                "Brake override changed lateral forces or wheel animation/state");
    }
}

void wall_response_split() {
    for (unsigned orientation : {0u,64u,128u,192u})
        for (int distance : {0,1,100,1000,3000})
            for (int speed : {-16000,-400,0,400,16000}) {
                srf::ContactManifold manifold{};
                manifold[0]={1,std::int16_t(distance),{32767,0,0}};
                const srf::RacePosition velocity{std::int16_t(speed),71,std::int16_t(-speed)};
                srf::ContactResponseInput input{velocity,128,0,-110};
                const auto original=srf::respond_to_contacts(manifold,std::uint8_t(orientation),input);
                input.wall_step=srf::WallContactStep{velocity,true};
                const auto event=srf::respond_to_contacts(manifold,std::uint8_t(orientation),input);
                require(event.force==original.force && event.displacement==original.displacement &&
                        event.body_flags==original.body_flags && event.contact_flags==original.contact_flags,
                        "Wall split changed the original response at its event boundary");
                input.wall_step->apply_impulse=false;
                const auto held=srf::respond_to_contacts(manifold,std::uint8_t(orientation),input);
                input.wall_step.reset();input.velocity={};
                const auto spring=srf::respond_to_contacts(manifold,std::uint8_t(orientation),input);
                require(held.force==spring.force && held.impulse==srf::RacePosition{} &&
                        held.displacement==original.displacement,
                        "Intermediate wall step repeated a velocity kick or lost penetration response");
                for(unsigned a=0;a<3;++a)
                    require(word(event.force[a]-event.impulse[a])==word(held.force[a]),
                            "Wall force and impulse lost saturation residue");
            }
    // The override applies to vertical walls only. Ramps/suspension retain
    // their original velocity, and leaving a manifold has no cached impulse.
    for (const srf::RacePosition normal : {srf::RacePosition{0,-32768,0},{20000,-20000,0}}) {
        srf::ContactManifold manifold{};manifold[0]={1,50,normal};
        srf::ContactResponseInput input{{-700,100,0},128,0,110};
        const auto original=srf::respond_to_contacts(manifold,0,input);
        input.wall_step=srf::WallContactStep{{3000,-3000,3000},false};
        const auto overridden=srf::respond_to_contacts(manifold,0,input);
        require(original.force==overridden.force && overridden.impulse==srf::RacePosition{},
                "Wall policy changed a ramp or suspension force");
        const auto empty=srf::respond_to_contacts({},0,input);
        require(empty.force==srf::RacePosition{} && empty.impulse==srf::RacePosition{},
                "Leaving a wall retained its force/impulse");
    }
    // Constant sustained contact over complete clock periods: each original
    // kick appears once, spring integrates for elapsed time. Exit/re-entry and
    // reset cannot leave per-wheel contact latches or share event history.
    srf::VehicleTimestep clock;
    srf::ContactManifold contact{};contact[0]={1,10,{32767,0,0}};
    srf::ContactResponseInput input{{-400,0,0},128,0,-110};
    const auto original=srf::respond_to_contacts(contact,0,input);
    int sum=0,events=0;
    for(unsigned tick=0;tick<120;++tick) {
        const bool due=clock.begin_step(),active=tick<60 || tick>=90;
        input.wall_step=srf::WallContactStep{input.velocity,due};
        const auto response=srf::respond_to_contacts(active ? contact : srf::ContactManifold{},0,input);
        sum+=signed_word(clock.force_delta(word(response.force[0]),word(response.impulse[0]),0));
        if(response.impulse[0])++events;
    }
    require(events==21 && sum==21*original.force[0], "Sustained wall response depends on substep count");
    clock.reset();
    require(clock.begin_step(), "Reset lost the first wall response boundary");
}

void wall_transfer_ledger() {
    srf::VehicleTuning tuning;
    srf::WheelContactTuning contact;
    contact.sine_bytes[64]=127;contact.linear_transfer=180;contact.displacement_limit=120;
    const std::array<Word,9> orientation{32767,0,0,0,32767,0,0,0,32767};
    for(unsigned mode:{0u,2u,4u,6u,8u,10u}) for(int offset:{-150,110}) {
        contact.angular_mode=Word(mode);
        srf::VehicleState body,wheel;
        wheel.word(0x36)=wheel.word(0x3c)=word(offset);
        wheel.word(0x38)=wheel.word(0x3e)=180;
        wheel.word(0x3a)=wheel.word(0x40)=130;wheel.word(0x50)=1000;
        srf::WheelContactWork work;work.minimum_distance=0;work.friction=18000;
        work.force={3200,-230,5000};work.impulse={1200,0,2700};
        auto plain=work,spring=work;plain.impulse={};spring.impulse={};
        for(unsigned a=0;a<3;++a)spring.force[a]=std::int16_t(word(work.force[a]-work.impulse[a]));
        auto plain_body=body,plain_wheel=wheel,spring_body=body,spring_wheel=wheel;
        srf::VehicleForces forces;forces.words={100,200,300,400,500,600};
        forces.impulses={20,30,40,50,60,70};
        auto plain_forces=forces,spring_forces=forces;
        srf::finish_wheel_contact(body,wheel,forces,work,tuning,contact,orientation);
        srf::finish_wheel_contact(plain_body,plain_wheel,plain_forces,plain,tuning,contact,orientation);
        srf::finish_wheel_contact(spring_body,spring_wheel,spring_forces,spring,tuning,contact,orientation);
        require(forces.words==plain_forces.words && body.words==plain_body.words && wheel.words==plain_wheel.words,
                "Wall impulse bookkeeping changed the original transfer");
        srf::VehicleTimestep unity(1,1);
        for(unsigned a=0;a<6;++a) {
            require(word(forces.words[a]-forces.impulses[a])==word(spring_forces.words[a]-spring_forces.impulses[a]),
                    "Wall impulse lost a wheel transfer rounding or torque contribution");
            const int original=signed_word(plain_forces.words[a]);
            require(unity.force_delta(forces.words[a],forces.impulses[a],a)==word(a==1 || a>=3 ? std::clamp(original,-3000,3000) : original),
                    "Wall torque saturation broke unity identity");
        }
    }
}

void elapsed_time_and_event_clock() {
    srf::VehicleTimestep step;
    const std::vector<unsigned> expected{0, 5, 9, 13, 18, 22, 26};
    std::vector<unsigned> boundaries;
    std::array<int, 6> motion{}, force{};
    constexpr std::array<int, 6> rates{1, -1, 101, -101, 3000, -3000};
    for (unsigned i = 0; i < 30; ++i) {
        const bool due = step.begin_step();
        if (due) boundaries.push_back(i);
        for (unsigned axis = 0; axis < 6; ++axis) {
            motion[axis] += signed_word(step.pose_delta(word(rates[axis]), axis));
            force[axis] += signed_word(step.force_delta(word(rates[axis]), 0, axis));
        }
    }
    require(boundaries == expected, "7/30 original callback boundaries drifted");
    require(step.begin_step(), "Next aligned interval must start at substep30");
    for (unsigned axis = 0; axis < 6; ++axis) {
        require(motion[axis] == rates[axis] * 7, "Fractional pose lost elapsed-time motion");
        require(force[axis] == rates[axis] * 7, "Fractional continuous force lost elapsed-time impulse");
    }

    step.reset();
    std::array<int, 2> mixed{}, continuous{};
    for (unsigned i = 0; i < 30; ++i) {
        const bool due = step.begin_step();
        for (unsigned axis = 0; axis < 2; ++axis) {
            const int c = axis ? -101 : 101, j = due ? (axis ? -700 : 700) : 0;
            const auto value = signed_word(step.force_delta(word(c + j), word(j), axis));
            mixed[axis] += value;
            continuous[axis] += value - j;
        }
    }
    require(mixed == std::array<int, 2>{5607, -5607}, "Event impulses were scaled or repeated per substep");
    require(continuous == std::array<int, 2>{707, -707}, "Continuous force changed when event impulses were added");
}

void independent_histories_and_reset() {
    srf::VehicleTimestep step;
    for (unsigned i = 0; i < 4; ++i)
        require(step.pose_delta(1, 0) == 0, "Unexpected fractional pose carry");
    require(step.pose_delta(1, 1) == 0, "Pose axes share a fractional remainder");
    require(step.force_delta(1, 0, 0) == 0, "Pose and force share a fractional remainder");
    require(step.pose_delta(1, 0) == 1, "Pose carry was lost");

    // Each wheel/axis has its own inverse-dt remainder. At7/30, seven samples
    // of one coordinate unit reconstruct exactly240 original velocity units.
    std::array<std::array<int, 3>, 4> sums{};
    for (unsigned sample = 0; sample < 7; ++sample)
        for (unsigned wheel = 0; wheel < 4; ++wheel)
            for (unsigned axis = 0; axis < 3; ++axis) {
                const int sign = (wheel + axis) % 2 ? -1 : 1;
                const int value = signed_word(step.attachment_velocity(word(sign), wheel, axis));
                if (sample == 0) require(value == sign * 34, "Wheel histories leaked across axes or wheels");
                sums[wheel][axis] += value;
            }
    for (unsigned wheel = 0; wheel < 4; ++wheel)
        for (unsigned axis = 0; axis < 3; ++axis)
            require(sums[wheel][axis] == ((wheel + axis) % 2 ? -240 : 240),
                    "Attachment normalization lost fractional velocity");

    // Dirty every kind of remainder and the event clock, then compare a fresh
    // instance through enough calls to expose a leftover fractional carry.
    for (unsigned i = 0; i < 3; ++i) {
        step.begin_step();
        step.pose_delta(1, 4);
        step.force_delta(word(-1), 0, 5);
        step.attachment_velocity(1, 2, 1);
    }
    step.reset();
    srf::VehicleTimestep fresh;
    for (unsigned i = 0; i < 30; ++i) {
        require(step.begin_step() == fresh.begin_step(), "Reset retained event phase");
        for (unsigned axis = 0; axis < 6; ++axis) {
            require(step.pose_delta(word(-1), axis) == fresh.pose_delta(word(-1), axis), "Reset retained pose fraction");
            require(step.force_delta(1, 0, axis) == fresh.force_delta(1, 0, axis), "Reset retained force fraction");
        }
        for (unsigned wheel = 0; wheel < 4; ++wheel)
            for (unsigned axis = 0; axis < 3; ++axis)
                require(step.attachment_velocity(1, wheel, axis) == fresh.attachment_velocity(1, wheel, axis),
                        "Reset retained wheel history");
    }
}

void attachment_wrapping() {
    srf::VehicleTimestep forward, reverse;
    const Word before = 65530, after = 5;
    require(forward.attachment_velocity(static_cast<Word>(after - before), 0, 0) == 377,
            "World wrap was treated as a long positive displacement");
    require(reverse.attachment_velocity(static_cast<Word>(before - after), 0, 0) == word(-377),
            "World wrap was treated as a long negative displacement");
    srf::VehicleTimestep positive, negative;
    //5000*8 exceeds a signed16-bit word. Normalize the displacement first;
    // normalizing the already-wrapped velocity instead changes sign/magnitude.
    require(positive.attachment_velocity(5000, 0, 0) == word(171428), "Positive anchor delta narrowed before dt normalization");
    require(negative.attachment_velocity(word(-5000), 0, 0) == word(-171428), "Negative anchor delta narrowed before dt normalization");
}

void contact_measurement_horizon() {
    for (int sign : {-1, 1}) {
        srf::VehicleTimestep quarter(1, 4);
        for (unsigned i = 0; i < 40; ++i) {
            const auto velocity=quarter.attachment_window_velocity(word(i%4==0 ? sign : 0), 0, 0, 0);
            if(i>=3)require(velocity==word(sign*8), "Quantized constant motion produced wheel velocity pulses");
        }
        srf::VehicleTimestep rational;
        const std::array<int,7> expected{8,8,8,8,2,0,0};
        for(unsigned i=0;i<expected.size();++i)
            require(rational.attachment_window_velocity(word(i==0 ? sign : 0), 0, 0, 0)==word(sign*expected[i]),
                    "Fractional oldest contact sample has the wrong weight or sign");
        rational.reset();
        for(unsigned i=0;i<50;++i)
            for(unsigned wheel=0;wheel<4;++wheel)
                for(unsigned axis=0;axis<3;++axis) {
                    const int rate=sign*int(1+wheel*3+axis);
                    require(rational.attachment_window_velocity(word(7*rate),word(i ? 9999 : 240*rate),wheel,axis)==word(240*rate),
                            "Contact history leaked between wheels/axes or reseeded a moving wheel");
                }
        rational.reset_attachment(2);
        require(rational.attachment_window_velocity(0,0,2,0)==0, "Disabled wheel retained its contact history");
        require(rational.attachment_window_velocity(word(7*sign),word(240*sign),0,0)==word(240*sign),
                "Resetting one wheel erased a different wheel history");
        rational.reset();
        for(unsigned wheel=0;wheel<4;++wheel)
            for(unsigned axis=0;axis<3;++axis)
                require(rational.attachment_window_velocity(0,0,wheel,axis)==0, "Reset retained contact window state");
    }
    srf::VehicleTimestep wrapped;
    require(wrapped.attachment_window_velocity(word(5-65530),0,0,0)==88,
            "Contact horizon lost the short wrapped displacement");
    // Seven complete pulse responses conserve the fractional oldest sample.
    wrapped.reset();int sum=0;
    for(unsigned i=0;i<49;++i)
        sum+=signed_word(wrapped.attachment_window_velocity(i%7==0 ? 1 : 0,0,0,0));
    require(sum==240, "Contact horizon lost fractional velocity over repeated pulses");
}

void callback_boundary_pose() {
    srf::VehicleTimestep step;
    const std::vector<unsigned> expected{4,8,12,17,21,25,29};
    std::vector<unsigned> boundaries;
    for(unsigned i=0;i<30;++i) {
        step.begin_step();
        if(step.ends_callback()) {
            boundaries.push_back(i);
            const unsigned completed=(i+1)*7/30;
            const unsigned from_start=completed*30-i*7;
            require(step.callback_end_position(100,107)==100+from_start, "Callback sampled an overshot positive pose");
            require(step.callback_end_position(100,93)==100-from_start, "Callback sampled an overshot negative pose");
            require(step.callback_end_position(65533,4)==word(65533+int(from_start)), "Boundary interpolation lost world wrap");
        } else {
            bool rejected=false;
            try { step.callback_end_position(0,7); }
            catch(const std::logic_error&) { rejected=true; }
            require(rejected, "Non-boundary pose sample was accepted");
        }
    }
    require(boundaries==expected, "Callback completion clock drifted");
    step.reset();step.begin_step();
    require(!step.ends_callback(), "Reset retained callback completion phase");
}

void pose_force_order() {
    srf::VehicleTimestep unity(1,1);
    for(unsigned raw=0;raw<65536;++raw) {
        unity.force_delta(word(raw*37),word(raw*11),raw%6);
        require(unity.ordered_pose_delta(word(raw),word(raw),raw%5,raw%6)==word(raw),
                "Unity pose order changed signed rounding or wrapping");
    }
    // A constant force makes Euler's timestep-dependent position lead exact.
    // With known preceding force, each four substeps must reproduce the coarse
    // drift-before-kick position, not a faster semidiscrete acceleration curve.
    for(int sign : {-1,1}) {
        srf::VehicleTimestep ordered(1,4),plain(1,4);
        ordered.force_delta(word(sign*64),0,2);
        int velocity=sign*160,coarse_velocity=velocity,position=0,plain_position=0,expected=0;
        for(unsigned i=0;i<12;++i) {
            ordered.begin_step();plain.begin_step();
            if(i%4==0) {expected+=coarse_velocity/16;coarse_velocity+=sign*64;}
            const Word delta=word(velocity/16);
            position+=signed_word(ordered.ordered_pose_delta(delta,word(velocity),4,2));
            plain_position+=signed_word(plain.pose_delta(delta,2));
            velocity+=signed_word(ordered.force_delta(word(sign*64),0,2));
            if(i%4==3)require(position==expected, "Constant-force position does not preserve drift-before-kick order");
        }
        require(plain_position!=position, "Constant-force fixture failed to expose the original order error");
        // The production experiment ratio must also close exactly at common
        // endpoints, including negative motion and its fractional correction.
        srf::VehicleTimestep rational;
        rational.force_delta(word(sign*480),0,2);
        velocity=sign*1600;position=0;
        for(unsigned i=0;i<30;++i) {
            rational.begin_step();
            position+=signed_word(rational.ordered_pose_delta(word(velocity/16),word(velocity),4,2));
            velocity+=signed_word(rational.force_delta(word(sign*480),0,2));
        }
        require(position==sign*1330, "7/30 position correction lost the original seven-callback integral");
    }
    srf::VehicleTimestep continuous,mixed;
    continuous.force_delta(64,0,2);mixed.force_delta(464,400,2);
    for(unsigned i=0;i<30;++i) {
        continuous.begin_step();mixed.begin_step();
        require(continuous.ordered_pose_delta(20,320,4,2)==mixed.ordered_pose_delta(20,320,4,2),
                "Position correction delayed an event impulse");
    }
    srf::VehicleTimestep constrained,fresh;
    constrained.force_delta(1000,0,2);
    require(constrained.ordered_pose_delta(0,0,4,2)==0, "Stale force moved a stopped axis");
    constrained.force_delta(1000,0,2,true);
    require(constrained.ordered_pose_delta(20,320,4,2)==fresh.ordered_pose_delta(20,320,4,2),
            "Discarded force retained a position correction");
    for(unsigned axis=0;axis<6;++axis) {
        constrained.force_delta(word(700+axis),0,axis);
        constrained.ordered_pose_delta(17,272,4,axis);
    }
    constrained.reset();fresh.reset();
    for(unsigned i=0;i<30;++i) {
        constrained.begin_step();fresh.begin_step();
        for(unsigned axis=0;axis<6;++axis) {
            require(constrained.ordered_pose_delta(word(-17),word(-272),4,axis)==fresh.ordered_pose_delta(word(-17),word(-272),4,axis),
                    "Reset retained pose-order history or fraction");
            constrained.force_delta(word(-63),0,axis);fresh.force_delta(word(-63),0,axis);
        }
    }
    bool rejected=false;
    try {fresh.ordered_pose_delta(1,16,5,0);}
    catch(const std::invalid_argument&) {rejected=true;}
    require(rejected, "Unsupported pose divisor was accepted");
}

void stopping_after_accumulated_force() {
    srf::VehicleState original,candidate,early;
    original.word(0x34)=40;original.word(0x42)=0x10;
    candidate=early=original;
    srf::VehicleForces full;full.words[3]=40;
    const std::array<Word,9> orientation{};
    srf::integrate_vehicle_velocity(original,full,orientation);
    srf::VehicleTimestep clock(1,4);
    for(unsigned i=0;i<4;++i) {
        const bool start=clock.begin_step();
        srf::VehicleForces end_force,early_force;
        end_force.words[3]=early_force.words[3]=10;
        srf::integrate_vehicle_velocity(candidate,end_force,orientation,false,clock.ends_callback());
        srf::integrate_vehicle_velocity(early,early_force,orientation,false,start);
        require(candidate.word(0x34)==50+i*10, "Stopping discarded force before the callback was complete");
    }
    require(candidate.word(0x34)==original.word(0x34) && original.word(0x34)==80,
            "End-clocked threshold disagrees with the original accumulated force");
    require(early.word(0x34)==30, "Threshold fixture failed to expose the start-clock error");
}

void saturation_and_discard() {
    srf::VehicleTimestep step;
    // Gravity120 plus jump6000 clips to3000 in the legacy Y accumulator.
    // The proposed split keeps positive gravity and attributes2880 to the kick.
    require(step.force_delta(6120, 6000, 1) == 2908, "Mixed saturation fabricated negative gravity");
    step.reset();
    require(step.force_delta(2000, word(-2000), 1) == word(-300), "Opposing impulse must retain limiter residue");
    step.reset();
    require(step.force_delta(6120, 6000, 0) == 6028, "Original X force was incorrectly limited");
    step.reset();
    require(step.force_delta(word(-6120), word(-6000), 4) == word(-2908), "Negative torque limiter lost impulse sign");

    for (unsigned i = 0; i < 4; ++i)
        require(step.force_delta(1, 0, 2) == 0, "Unexpected force carry before discard");
    require(step.force_delta(701, 700, 2, true) == 0, "Overspeed discard retained force or impulse");
    for (unsigned i = 0; i < 4; ++i)
        require(step.force_delta(1, 0, 2) == 0, "Discard retained stale fractional force");
    require(step.force_delta(1, 0, 2) == 1, "Discard prevented later force accumulation");
}

void original_force_ledger() {
    srf::VehicleState body;
    srf::VehicleTuning tuning;
    srf::VehicleForces forces;
    srf::VehicleDriveContext context;
    tuning.words[(0x2670 - 0x2658) / 2] = 32767;
    tuning.words[(0x2672 - 0x2658) / 2] = word(-700);
    body.word(0x1c) = 8;
    body.word(0x4a) = 1000;
    forces.words.fill(99);
    forces.impulses.fill(88);
    srf::begin_vehicle_forces(body, tuning, forces);
    require(forces.words == std::array<Word, 6>{0, 619, 0, 0, 0, 0}, "Begin-force legacy gravity changed");
    require(forces.impulses == std::array<Word, 6>{}, "Begin-force retained a previous impulse");
    body.word(0x42) = 0x80;
    srf::update_vehicle_controls(body, tuning, forces, context);
    require(forces.words[1] == word(-81) && forces.impulses[1] == word(-700), "Original jump total or ledger changed");
    require(forces.continuous_word(1) == 619 && body.word(0x5e) == 32, "Jump erased gravity or original cooldown");
    srf::begin_vehicle_forces(body, tuning, forces);
    srf::update_vehicle_controls(body, tuning, forces, context);
    require(forces.words[1] == 619 && forces.impulses[1] == 0 && body.word(0x5e) == 31,
            "Held jump bypassed original callback cooldown");
    body.word(0x1c) = 0;
    srf::begin_vehicle_forces(body, tuning, forces);
    require(forces.words[1] == 679, "Full callback lost original extra60 gravity");

    body.word(0x16) = 0x10;
    body.word(0x1e) = 1000;
    body.word(0x2a) = 1000;
    srf::VehicleState target;
    target.word(0x2a) = 1000;
    const std::array candidates{srf::BodyContactCandidate{{0, 0}, {1200, 0}, &target}};
    forces.words = {32000, 120, word(-32700), 15, word(-15), 0};
    forces.impulses = {word(-8000), 0, 0, 0, 0, 0};
    const auto continuous = forces.continuous_word(0);
    for (unsigned event = 0; event < 2; ++event) {
        const auto before = forces;
        srf::BodyCollisionWork work;
        srf::update_body_collisions(body, forces, candidates, {1, 0}, work);
        require(work.force[0] != 0, "Body-contact ledger fixture had no response");
        for (unsigned axis = 0; axis < 6; ++axis) {
            const Word added = axis == 0 || axis == 2 ? work.force[axis] : 0;
            require(forces.words[axis] == Word(before.words[axis] + added), "Ledger altered original combined body response");
            require(forces.impulses[axis] == Word(before.impulses[axis] + added), "Body response missing from impulse ledger");
            require(forces.continuous_word(axis) == before.continuous_word(axis), "Body impulse changed continuous contribution");
        }
    }
    require(forces.continuous_word(0) == continuous, "Repeated wrapped body kicks corrupted continuous force");
    target.word(0x16) = 0x20;
    const auto before = forces;
    srf::BodyCollisionWork work;
    srf::update_body_collisions(body, forces, candidates, {1, 0}, work);
    require(forces.words == before.words && forces.impulses == before.impulses,
            "Non-solid candidate added a false impulse");
}

srf::VehicleTuning control_tuning() {
    srf::VehicleTuning tuning;
    const auto set = [&](unsigned address, int value) { tuning.words[(address - 0x2658) / 2] = word(value); };
    set(0x2658, 320); set(0x265a, 800); set(0x265c, 90);
    set(0x265e, 8000); set(0x2660, 30000);
    set(0x2662, 100); set(0x2664, 200); set(0x2666, 1000); set(0x2668, 400); set(0x266a, 50);
    set(0x2672, -700); set(0x267e, 3);
    tuning.gear_limits = {100, 200, 300, 400, 500};
    // A deterministic decreasing torque curve exposes stale demand when speed
    // changes between control ticks. It is test data, not replacement ROM data.
    for (auto& curve : tuning.gear_curves) {
        curve[0] = 16;
        for (unsigned bucket = 0; bucket < 16; ++bucket) curve[bucket + 1] = static_cast<std::uint8_t>(64 - 2 * bucket);
    }
    return tuning;
}

void held_controls_and_physical_demand() {
    const auto tuning = control_tuning();
    srf::VehicleDriveContext context;
    context.advance_timing = false;
    srf::VehicleState body;
    srf::VehicleForces forces;
    body.word(0x1c) = 8;
    body.word(0x3a) = 234;
    body.word(0x70) = 8;
    body.word(0x42) = 0x11;
    body.word(0x4e) = 1;
    body.word(0x5e) = 7;
    body.word(0x78) = 320;
    body.word(0x50) = 321;
    body.word(0x44) = 4;
    body.word(0x54) = 1;
    body.word(0x40) = 4;
    for (int requested : {-16000, 16000, 0, 32767}) {
        srf::begin_vehicle_forces(body, tuning, forces, false);
        srf::update_vehicle_controls(body, tuning, forces, context, static_cast<std::int16_t>(requested));
        require(body.word(0x3a) == 234, "Held proportional steering advanced or doubled between ticks");
        require(body.word(0x42) == 0x11, "Non-tick control stage replaced the carried filtered input");
        require(body.word(0x5e) == 7 && forces.impulses[1] == 0, "Jump cooldown advanced between ticks");
        require(body.word(0x50) == 321, "Engine filter50 advanced between ticks");
    }
    context.advance_timing = true;
    srf::update_vehicle_controls(body, tuning, forces, context, std::int16_t(-16000));
    require(body.word(0x42) == 0, "Original suppression failed on the actual control tick");
    require(body.word(0x3a) == 368, "Suppressed steering must center once, then apply original doubling");
    context.advance_timing = false;
    body.word(0x44) = body.word(0x54) = 0;
    body.word(0x40) = 0;
    srf::update_vehicle_controls(body, tuning, forces, context, std::int16_t(32767));
    require(body.word(0x42) == 0 && body.word(0x3a) == 368, "Intermediate stick request resurrected suppressed controls");

    body.word(0x42) = 0x80;
    body.word(0x5e) = 0;
    srf::begin_vehicle_forces(body, tuning, forces, false);
    srf::update_vehicle_controls(body, tuning, forces, context);
    require(body.word(0x5e) == 0 && forces.impulses[1] == 0 && forces.words[1] == 120,
            "Ready jump fired outside its callback boundary");

    body.word(0x42) = 0;
    body.word(0x2c) = 0xffff;
    body.word(0x8a) = 4;
    body.word(0x4a) = 80;
    forces.contact_friction = {};
    srf::update_vehicle_controls(body, tuning, forces, context);
    require(body.word(0x2c) == 64, "Physical contact status was not reset between control ticks");
    require(forces.contact_friction == std::array<Word, 2>{17500, 17700}, "Physical friction retained a stale/default pair");
    require(body.word(0x48) == 2048, "Coasting brake minimum changed");
    body.word(0x4a) = 400;
    body.word(0x8a) = 1;
    srf::update_vehicle_controls(body, tuning, forces, context);
    require(body.word(0x48) == 2929, "Coasting drag failed to reevaluate current speed");
    require(forces.contact_friction == std::array<Word, 2>{16000, 16500}, "Physical surface friction failed to refresh");
    body.word(0x42) = 0x40;
    srf::update_vehicle_controls(body, tuning, forces, context);
    require(body.word(0x48) == 8000 && (body.word(0x40) & 0x800), "Explicit braking failed between control ticks");
}

void boost_curve_and_engine_clocks() {
    const auto tuning = control_tuning();
    srf::VehicleDriveContext context;
    context.update_phase = 1;
    srf::VehicleState body;
    body.word(0x4e) = 1;
    body.word(0x6a) = 1;
    body.word(0x42) = 0x400;
    body.word(0x50) = 400;
    srf::vehicle_drivetrain(body, tuning, context, body.word(0x42));
    require(body.word(0x6a) == 0 && body.word(0x78) == 800 && body.word(0x3c) == 400,
            "Spending the last reserve unit lost the original boosted interval");
    require(body.word(0x50) == 300, "Original engine50 quarter approach changed");
    context.advance_timing = false;
    body.word(0x4a) = 80;
    srf::vehicle_drivetrain(body, tuning, context, body.word(0x42));
    require(body.word(0x78) == 800 && body.word(0x3c) == 275 && body.word(0x6a) == 0,
            "Boost latch or speed-dependent torque curve failed between ticks");
    require(body.word(0x50) == 300, "Reevaluating torque also advanced engine50");
    context.advance_timing = true;
    srf::vehicle_drivetrain(body, tuning, context, body.word(0x42));
    // Original GSU keeps ordinary drive for a still-latched boost bit with no
    // reserve; the CPU adapter may clear that bit at its separate input boundary.
    require(body.word(0x78) == 320 && body.word(0x3c) == 110, "Last boosted interval survived the next control tick");
    body.word(0x42) = 0;
    srf::vehicle_drivetrain(body, tuning, context, 0);
    require(body.word(0x78) == 0 && body.word(0x3c) == 0, "CPU input refresh failed to end drive demand");

    body.word(0x42) = 0x10;
    body.word(0x78) = 320;
    body.word(0x4a) = 260;
    body.word(0x50) = 321;
    context.update_phase = 4;
    context.advance_timing = false;
    srf::vehicle_drivetrain(body, tuning, context, 0x10);
    require(body.word(0x4e) == 1 && body.word(0x50) == 321, "Eligible phase bypassed the original callback clock");
    context.advance_timing = true;
    srf::vehicle_drivetrain(body, tuning, context, 0x10);
    require(body.word(0x4e) == 3 && body.word(0x50) == 352, "Gear selection or engine filter changed on its due tick");

    constexpr std::array<Word, 9> identity{32767, 0, 0, 0, 32767, 0, 0, 0, 32767};
    srf::VehicleForces forces;
    body.word(0x4a) = 0;
    body.word(0x50) = 400;
    body.word(0x52) = 100;
    body.word(0x44) = 4;
    srf::integrate_vehicle_velocity(body, forces, identity, false);
    require(body.word(0x52) == 100, "Engine52 advanced between ticks");
    srf::integrate_vehicle_velocity(body, forces, identity, true);
    require(body.word(0x52) == 450, "Engine52 lost original airborne target and half approach");
}

void transient_flags_and_default_clock() {
    const auto tuning = control_tuning();
    srf::VehicleState body;
    srf::VehicleForces forces;
    constexpr Word persistent = 0x2095, transient = 0x086a;
    body.word(0x40) = persistent | transient;
    srf::begin_vehicle_forces(body, tuning, forces, false);
    require(body.word(0x40) == Word(persistent | transient | 0x100), "Intermediate force setup erased an event flag");
    body.word(0x40) &= Word(~0x100);
    srf::begin_vehicle_forces(body, tuning, forces, false);
    require(body.word(0x40) & 0x100, "Airborne status was not reset for the next physical contact query");
    srf::begin_vehicle_forces(body, tuning, forces, true);
    require(body.word(0x40) == Word(persistent | 0x100), "Due callback did not clear exactly the original transient mask");

    // Exact known outcomes supplement the default/explicit-clock comparison.
    srf::VehicleState implicit, explicit_tick;
    implicit.word(0x1c) = 8;
    implicit.word(0x3a) = 100;
    implicit.word(0x42) = 0x11;
    implicit.word(0x4a) = 80;
    implicit.word(0x4e) = 1;
    implicit.word(0x50) = 400;
    explicit_tick = implicit;
    srf::VehicleForces a, b;
    srf::VehicleDriveContext default_context, tick_context;
    tick_context.advance_timing = true;
    srf::begin_vehicle_forces(implicit, tuning, a);
    srf::begin_vehicle_forces(explicit_tick, tuning, b, true);
    srf::update_vehicle_controls(implicit, tuning, a, default_context);
    srf::update_vehicle_controls(explicit_tick, tuning, b, tick_context);
    require(implicit.words == explicit_tick.words && a.words == b.words && a.impulses == b.impulses,
            "Default callback ceased to mean an original timing tick");
    require(implicit.word(0x3a) == 228 && implicit.word(0x3c) == 110 && implicit.word(0x50) == 460,
            "Original directional slew, torque curve or engine approach changed");
}

void damage_sample_clock() {
    srf::VehicleState body;
    srf::VehicleTuning tuning;
    tuning.words[(0x2678 - 0x2658) / 2] = 16384;
    body.word(0x44) = 2;
    srf::VehicleForces forces;
    forces.words[1] = 512;
    srf::VehicleDriveContext context;
    context.damage_accumulator = 999;
    context.damage_mode = 0;
    require(!srf::update_vehicle_damage(body, tuning, forces, context), "Mode0 emitted a crash");
    require(context.damage_accumulator == 32 && body.word(0x82) == 0 && body.word(0x62) == 128,
            "Mode0 did not reset then store its original raw-force sample");
    context.advance_timing = false;
    context.damage_mode = 1;
    forces.words = {9000, 12000, 9000, 0, 0, 0};
    const auto before = body;
    for (unsigned step = 0; step < 5; ++step) {
        require(!srf::update_vehicle_damage(body, tuning, forces, context), "Held mode1 emitted a crash between ticks");
        require(body.words == before.words && context.damage_accumulator == 32,
                "Intermediate damage sample changed state or accumulator");
    }
    context.advance_timing = true;
    forces.words = {0, 768, 0, 0, 0, 0};
    require(!srf::update_vehicle_damage(body, tuning, forces, context), "Moderate paired damage emitted a heavy crash");
    require(context.damage_accumulator == 80 && body.word(0x82) == 40 && body.word(0x62) == 192,
            "Mode1 did not combine the two callback samples exactly once");
    require((body.word(0x40) & 0x40) && (body.word(0x70) & 0x80), "Damage failed to set original recovery flags");
    context.damage_mode = 0;
    forces.words[1] = 512;
    require(!srf::update_vehicle_damage(body, tuning, forces, context) && body.word(0x82) == 40 && context.damage_accumulator == 32,
            "A final unpaired mode0 incorrectly committed damage");

    body = {};
    body.word(0x44) = 2;
    context.damage_mode = 1;
    context.damage_accumulator = 0;
    forces.words[1] = 3200;
    require(srf::update_vehicle_damage(body, tuning, forces, context), "Initial mode1 did not retain original heavy-crash semantics");
    require(body.word(0x62) == 800 && body.word(0x82) == 96 && context.damage_accumulator == 200,
            "Damage sampled clamped/scaled force instead of raw3200, or changed the96 cap");

    body.word(0x40) = 1;
    body.word(0x62) = 123;
    context.damage_mode = 0;
    context.damage_accumulator = 77;
    require(!srf::update_vehicle_damage(body, tuning, forces, context) && body.word(0x62) == 0 && context.damage_accumulator == 77,
            "Immune mode0 reset the accumulator before the original early exit");
    body.word(0x64) = 8;
    body.word(0x62) = 123;
    require(!srf::update_vehicle_damage(body, tuning, forces, context) && body.word(0x62) == 123 && context.damage_accumulator == 77,
            "Opponent early exit altered player damage state");
}

void motion_damping_and_bounds() {
    constexpr std::array<Word,9> orientation{};
    // Known results of the original three signed DIV2 operations, including
    // their special -1 -> 0 behavior; these are not C++ truncation toward zero.
    constexpr std::array<int,12> speeds{0,1,-1,7,-7,8,-8,9,-9,32767,-32768,-2};
    constexpr std::array<int,12> damped{0,1,-1,7,-6,7,-7,8,-7,28672,-28672,-2};
    for(std::size_t i=0;i<speeds.size();++i)for(bool due:{false,true}) {
        srf::VehicleState body;
        body.word(0x64)=8; // No player stopping rule in this damping fixture.
        body.word(0x4a)=600;
        body.word(0x2e)=body.word(0x32)=word(speeds[i]);
        srf::VehicleForces forces;
        forces.words={123,10,word(-123),0,0,0};
        srf::integrate_vehicle_velocity(body,forces,orientation,false,due);
        const auto expected=word(due ? damped[i] : speeds[i]);
        require(body.word(0x2e)==expected && body.word(0x32)==expected,
                "Overspeed damping advanced off-clock or changed original signed rounding");
        require(forces.words[0]==0 && forces.words[2]==0 && body.word(0x30)==10,
                "Physical overspeed suppression must retain Y force and reject X/Z on every substep");
    }
    for(int speed:{599,-599,600,-600}) {
        srf::VehicleState body;
        body.word(0x64)=8;body.word(0x4a)=word(speed);body.word(0x2e)=800;
        srf::VehicleForces forces;forces.words[0]=16;
        srf::integrate_vehicle_velocity(body,forces,orientation,false,true);
        require(body.word(0x2e)==(std::abs(speed)==599 ? 816 : 700), "Overspeed600 boundary moved");
    }
    for(bool due:{false,true})for(int angle:{8191,8192,8193,-8191,-8192,-8193}) {
        srf::VehicleState body;
        body.word(0x64)=8;body.word(0x26)=body.word(0x28)=word(angle);
        body.word(0x36)=body.word(0x38)=100;
        srf::VehicleForces forces;forces.words[4]=forces.words[5]=10;
        srf::integrate_vehicle_velocity(body,forces,orientation,false,due);
        require(body.word(0x26)==word(std::clamp(angle,-8192,8192)) && body.word(0x28)==body.word(0x26),
                "Hard angle bound was incorrectly deferred to the callback clock");
        require(body.word(0x36)==(angle>=8192 || angle< -8192 ? 10 : 110) && body.word(0x38)==body.word(0x36),
                "Angle clamp changed original asymmetric rate reset or post-clamp torque ordering");
    }
    srf::VehicleTimestep clock;
    srf::VehicleState positive,negative;
    positive.word(0x28)=4096;negative.word(0x28)=word(-4096);
    positive.word(0x44)=negative.word(0x44)=4;
    constexpr std::array<int,7> positive_roll{3072,2304,1728,1296,972,729,547};
    constexpr std::array<int,7> negative_roll{-3072,-2304,-1728,-1296,-972,-729,-546};
    unsigned tick=0;
    for(unsigned step=0;step<30;++step) {
        const bool due=clock.begin_step();
        srf::VehicleForces a,b;
        srf::integrate_vehicle_velocity(positive,a,orientation,false,due);
        srf::integrate_vehicle_velocity(negative,b,orientation,false,due);
        if(due)++tick;
        require(signed_word(positive.word(0x28))==positive_roll[tick-1] && signed_word(negative.word(0x28))==negative_roll[tick-1],
                "Airborne quarter approach lost its original time/rounding sequence");
    }
}

void stopping_thresholds_and_ordering() {
    constexpr std::array<Word,9> orientation{};
    for(unsigned axis=0;axis<6;++axis) {
        const int threshold=axis>=3 ? 64 : axis==1 ? 50 : 200;
        for(int value:{-threshold-1,-threshold,-threshold+1,-1,0,1,threshold-1,threshold,threshold+1}) {
            for(bool due:{false,true}) {
                srf::VehicleState body;
                // Force crosses to the measured threshold before the stopping decision.
                body.word(0x2e + axis*2)=word(value-7);
                srf::VehicleForces forces;forces.words[axis]=7;
                srf::integrate_vehicle_velocity(body,forces,orientation,false,due);
                require(body.word(0x2e + axis*2)==word(due && value>=-threshold && value<threshold ? 0 : value),
                        "Stopping rule lost its asymmetric boundary, post-force ordering or clock");
            }
        }
    }
    for(unsigned control:{0x10u,0x20u,0x400u}) {
        srf::VehicleState body;body.word(0x42)=Word(control);
        body.word(0x2e)=100;body.word(0x34)=63;
        srf::VehicleForces forces;
        srf::integrate_vehicle_velocity(body,forces,orientation,false,true);
        require(body.word(0x2e)==100 && body.word(0x34)==0, "Throttle/reverse/boost must exempt linear stopping only");
    }
    for(bool ai:{false,true}) {
        srf::VehicleState body;body.word(0x2c)=0x400;body.word(0x64)=ai ? 8 : 0;
        body.word(0x2e)=100;body.word(0x34)=63;
        srf::VehicleForces forces;
        srf::integrate_vehicle_velocity(body,forces,orientation,false,true);
        require(body.word(0x2e)==100 && body.word(0x34)==(ai ? 63 : 0), "Stopping exemptions changed");
    }
    // Sub-threshold torque must accumulate between callback boundaries; the
    // old every-substep snap erased every increment in this same sequence.
    srf::VehicleState body;body.word(0x42)=0x10;
    for(unsigned step=0;step<4;++step) {
        srf::VehicleForces forces;forces.words[3]=20;
        srf::integrate_vehicle_velocity(body,forces,orientation,false,step==3);
        require(body.word(0x34)==(step+1)*20, "Intermediate stopping destroyed accumulating angular force");
    }
}

void horizontal_rest_and_breakaway() {
    for(unsigned axis:{0u,2u}) {
        srf::VehicleTimestep clock(1,4);
        // Negative integer shifts used to cause creeping between callback
        // stops even though every completed callback left this axis at zero.
        clock.stop_horizontal_axis(axis);
        int velocity=0,displacement=0;
        for(unsigned step=0;step<4;++step) {
            const auto delta=word(velocity<0 ? -((-velocity+15)/16) : velocity/16);
            displacement+=signed_word(clock.resting_pose_delta(delta,word(velocity),axis,true));
            velocity+=signed_word(clock.force_delta(word(-400),0,axis));
        }
        require(velocity==-400 && displacement==-4,
                "Rest must suppress small motion but release at the original asymmetric threshold");
        // -200 remains held; -201 wakes. +200 wakes; +199 remains held.
        for(int speed:{-201,-200,-199,-1,0,1,199,200,201}) {
            clock.reset();clock.stop_horizontal_axis(axis);
            const auto delta=clock.resting_pose_delta(20,word(speed),axis,true);
            require(delta==(speed>=-200 && speed<200 ? 0 : 5),
                    "Rest constraint changed the original asymmetric threshold");
        }
        clock.reset();
        require(clock.resting_pose_delta(20,100,axis,true)==5,
                "Moving axes must not become resting before a callback stop");
        clock.stop_horizontal_axis(axis);
        require(clock.resting_pose_delta(20,100,axis,false)==5 &&
                clock.resting_pose_delta(20,100,axis,true)==5,
                "Drive/contact/AI exemption must release rest until another stop");

        clock.reset();clock.stop_horizontal_axis(axis);
        for(unsigned step=0;step<8;++step) {
            clock.force_delta(1,0,axis);
            require(clock.resting_pose_delta(word(-1),word(-10),axis,true)==0,
                    "Stopped horizontal axis accumulated displacement");
        }
        require(clock.resting_pose_delta(20,0,1,true)==5,
                "Horizontal rest constrained vertical suspension");
        const unsigned other=axis==0 ? 2 : 0;
        require(clock.resting_pose_delta(20,100,other,true)==5,
                "Rest constraint leaked between horizontal axes");

        clock.reset();
        for(unsigned i=0;i<3;++i)clock.pose_delta(1,axis);
        clock.force_delta(1,0,axis);
        clock.stop_horizontal_axis(axis);
        require(clock.resting_pose_delta(1,300,axis,true)==0,
                "Pre-stop pose fraction leaked into breakaway");
        require(clock.force_delta(3,0,axis)==1,
                "Rest stop erased accumulated force");
        clock.stop_horizontal_axis(axis);
        clock.reset();
        require(clock.resting_pose_delta(20,100,axis,true)==5,
                "Vehicle reset retained a rest constraint");
    }
    srf::VehicleTimestep clock;
    for(unsigned axis:{1u,3u,6u}) {
        bool rejected=false;
        try {clock.stop_horizontal_axis(axis);} catch(const std::invalid_argument&) {rejected=true;}
        require(rejected,"Invalid rest axis accepted");
    }
}

void recovery_heading_clock() {
    srf::VehicleState body;body.word(0x70)=0x82;body.word(0x24)=0xff00;
    srf::VehicleForces forces;
    srf::VehicleTimestep clock;
    constexpr std::array<Word,7> headings{0xff80,0xffe0,0x0028,0x005e,0x005e,0x005e,0x005e};
    unsigned tick=0;
    for(unsigned step=0;step<30;++step) {
        const bool due=clock.begin_step(),active=(body.word(0x70)&0x80)!=0;
        body.word(0x34)=90;forces.words[3]=75;forces.words[0]=123;
        srf::update_vehicle_recovery(body,forces,Word(0x100),due);
        if(due)++tick;
        require(body.word(0x24)==headings[tick-1], "Recovery heading advanced off-clock or lost shortest wrapped difference");
        require(body.word(0x70)==(tick<4 ? 0x82 : 2), "Recovery completion used wrong boundary or cleared direction flag");
        require(body.word(0x34)==(active ? 0 : 90) && forces.words[3]==(active ? 0 : 75) && forces.words[0]==123,
                "Active recovery must lock only yaw motion/torque throughout the interval");
    }
    for(int difference:{-257,-256,-1,0,511,512}) {
        body={};body.word(0x70)=0x80;body.word(0x24)=word(difference);
        srf::update_vehicle_recovery(body,forces,Word(0));
        require(bool(body.word(0x70)&0x80)==(difference== -257 || difference==512),
                "Recovery exit lost original high-byte/asymmetric tolerance");
    }
    body={};body.word(0x70)=0x82;body.word(0x24)=123;body.word(0x34)=90;
    srf::update_vehicle_recovery(body,forces,std::nullopt,false);
    require(body.word(0x70)==0x82 && body.word(0x24)==123 && body.word(0x34)==90, "Missing target advanced recovery off-clock");
    srf::update_vehicle_recovery(body,forces,std::nullopt,true);
    require(body.word(0x70)==2, "Missing target failed to end recovery at the original boundary");
}

void neutral_yaw_rest_and_wakeup() {
    {
        srf::VehicleTimestep clock(1,4);
        clock.prepare_yaw_rest(0,true);
        for(unsigned tick=0;tick<120;++tick) {
            require(clock.resting_yaw_force_delta(63,0,0,true)==0,
                    "Subthreshold neutral torque accumulated hidden yaw momentum");
            require(clock.resting_yaw_pose_delta(20)==0,
                    "A resting neutral axis rotated between callback stops");
        }
        require(clock.force_delta(63,0,0)==15, "Yaw rest erased horizontal force");

        for(int force:{-65,-64,-1,0,63,64,1000}) {
            clock.reset();clock.prepare_yaw_rest(0,true);
            const auto delta=clock.resting_yaw_force_delta(word(force),0,0,true);
            const bool held=force>=-64 && force<64;
            require(delta==word(held ? 0 : force/4), "Breakaway used scaled velocity instead of unscaled torque");
            require(clock.resting_yaw_pose_delta(20)==(held ? 0 : 5),
                    "Breakaway failed to release yaw pose on the next substep");
        }
        clock.reset();clock.prepare_yaw_rest(0,true);
        require(clock.resting_yaw_force_delta(1000,1000,0,true)==1000,
                "Yaw rest scaled or swallowed a collision impulse");

        clock.reset();clock.prepare_yaw_rest(0,true);
        clock.prepare_yaw_rest(0,false);
        require(clock.resting_yaw_pose_delta(20)==5,
                "Active input did not release pose before force integration");
        require(clock.resting_yaw_force_delta(63,0,0,false)==15,
                "Active input did not release a subthreshold demand immediately");

        clock.stop_yaw();
        require(clock.resting_yaw_force_delta(63,0,1,true)==16,
                "Existing yaw momentum was swallowed by a stale rest flag");
        clock.stop_yaw();
        clock.reset();
        clock.prepare_yaw_rest(100,true);
        require(clock.resting_yaw_pose_delta(20)==5,
                "Reset or initialization froze a moving yaw axis");

        // At the original duration, neutral rest must preserve the original
        // asymmetric post-force snap, including full-scale clamping/impulses.
        for(int force=-3100;force<=3100;++force) {
            srf::VehicleTimestep unit(1,1);
            unit.prepare_yaw_rest(0,true);
            const auto expected=force>=-64 && force<64 ? 0 : std::clamp(force,-3000,3000);
            require(unit.resting_yaw_force_delta(word(force),word(force/2),0,true)==word(expected),
                    "Neutral yaw rest changed original-rate stopping or saturation");
        }
    }
    // A continuously active vehicle must reproduce the retained force/pose
    // solver, including its fractional residues, resets and event impulses.
    srf::VehicleTimestep active,baseline;
    for(unsigned tick=0;tick<120;++tick) {
        active.prepare_yaw_rest(0,false);
        const auto force=word(int(tick*31)-1700);
        const auto impulse=word(tick%17==0 ? 300 : 0);
        require(active.resting_yaw_force_delta(force,impulse,0,false)==
                baseline.force_delta(force,impulse,3), "Active driving force changed");
        require(active.resting_yaw_pose_delta(force)==baseline.pose_delta(force,3),
                "Active driving pose changed");
        active.stop_yaw();
    }
}

void suspension_rest_and_wakeup() {
    srf::VehicleTimestep clock(1,4);
    clock.prepare_suspension_rest(0,{0,0},true);
    Word velocity=0;
    for(unsigned step=0;step<10;++step) {
        clock.prepare_suspension_rest(velocity,{0,0},true);
        require(clock.suspension_pose_delta(20,1)==0,"Stopped height crept before breakaway");
        velocity=word(velocity+clock.force_delta(20,0,1));
    }
    require(velocity==50,"Height rest erased continuing support force");
    clock.prepare_suspension_rest(velocity,{0,0},true);
    require(clock.suspension_pose_delta(20,1)==5,"Changed support load failed to wake height");
    for(int speed:{-51,-50,0,49,50}) {
        clock.reset();clock.prepare_suspension_rest(0,{0,0},true);
        clock.prepare_suspension_rest(word(speed),{0,0},true);
        require(clock.suspension_pose_delta(20,1)==(speed>=-50 && speed<50 ? 0 : 5),
                "Height lost original asymmetric stopping threshold");
    }
    for(unsigned axis:{4u,5u}) {
        for(int torque:{-65,-64,-1,0,63,64,1000}) {
            clock.reset();clock.prepare_suspension_rest(0,{0,0},true);
            const bool held=torque>=-64 && torque<64;
            require(clock.suspension_torque_delta(word(torque),0,0,axis,true)==word(held ? 0 : torque/4),
                    "Tilt breakaway used the scaled kick instead of torque demand");
            require(clock.suspension_pose_delta(20,axis)==(held ? 0 : 5),"Tilt pose failed to follow its rest state");
            require(clock.suspension_pose_delta(20,axis==4 ? 5 : 4)==0,"Tilt rest leaked across axes");
        }
        clock.reset();clock.prepare_suspension_rest(0,{0,0},true);
        require(clock.suspension_torque_delta(1000,1000,0,axis,true)==1000,"Rest swallowed a collision impulse");
        clock.stop_suspension_axis(axis);
        require(clock.suspension_torque_delta(63,0,1,axis,true)==15,"Rest swallowed existing tilt momentum");
        for(int torque=-3100;torque<=3100;++torque) {
            srf::VehicleTimestep unit(1,1);
            unit.prepare_suspension_rest(0,{0,0},true);
            const auto expected=torque>=-64 && torque<64 ? 0 : std::clamp(torque,-3000,3000);
            require(unit.suspension_torque_delta(word(torque),word(torque/2),0,axis,true)==word(expected),
                    "Tilt rest changed original-rate stop/clamp semantics");
        }
    }
    // Waking from neutral must immediately resume the existing integrator,
    // including its force/pose fractions. No angle or ride height is imposed.
    srf::VehicleTimestep active,baseline;
    for(unsigned tick=0;tick<120;++tick) {
        active.prepare_suspension_rest(0,{0,0},false);
        for(unsigned axis:{1u,4u,5u}) {
            const auto force=word(int(tick*31)-1700);
            const auto impulse=word(tick%17==0 ? 300 : 0);
            const auto actual=axis==1 ? active.force_delta(force,impulse,axis) :
                active.suspension_torque_delta(force,impulse,0,axis,false);
            require(actual==baseline.force_delta(force,impulse,axis),"Suspension rest altered active forces");
            require(active.suspension_pose_delta(force,axis)==baseline.pose_delta(force,axis),
                    "Suspension rest altered active pose");
            active.stop_suspension_axis(axis);
        }
    }
    active.reset();active.prepare_suspension_rest(1,{1,1},true);
    for(unsigned axis:{1u,4u,5u})require(active.suspension_pose_delta(30,axis)==7,"Reset froze moving suspension");
    for(unsigned axis:{0u,2u,3u,6u}) {
        bool stop_rejected=false,pose_rejected=false,torque_rejected=false;
        try {clock.stop_suspension_axis(axis);}catch(const std::invalid_argument&){stop_rejected=true;}
        try {clock.suspension_pose_delta(1,axis);}catch(const std::invalid_argument&){pose_rejected=true;}
        try {clock.suspension_torque_delta(1,0,0,axis,true);}catch(const std::invalid_argument&){torque_rejected=true;}
        require(stop_rejected && pose_rejected && torque_rejected,"Invalid suspension axis accepted");
    }
}

void settled_rest_and_wakeup() {
    const std::array<Word,6> zero{},load{0,word(-48),0,word(-8),word(-40),word(-24)};
    const auto arm=[&](srf::VehicleTimestep& step) {
        do {step.begin_step();}while(!step.ends_callback());
        step.finish_settled_rest(true,zero,zero);
        require(step.reset_settled_contacts(true,zero),"Stopped body did not request a fresh contact window");
        require(step.prepare_settled_rest(true,zero,load,zero),"Unchanged supported body failed to rest");
        require(!step.reset_settled_contacts(true,zero),"Stable rest repeatedly reset contact history");
    };
    srf::VehicleTimestep step;
    step.begin_step();step.finish_settled_rest(true,zero,zero);
    require(!step.reset_settled_contacts(true,zero),"Rest armed before an original stopping boundary");
    step.force_delta(1,0,1); // Pre-stop force fraction must not launch the car later.
    arm(step);
    for(unsigned tick=0;tick<1200;++tick) {
        step.begin_step();
        require(step.prepare_settled_rest(true,zero,load,zero),"Static load woke a stopped body");
        for(unsigned axis=0;axis<6;++axis) {
            require(step.force_delta(load[axis],0,axis)==0,"Rest produced momentum");
            require(step.pose_delta(0,axis)==0,"Rest produced displacement");
        }
    }
    require(!step.prepare_settled_rest(false,zero,load,zero),"Controls/lost support did not wake immediately");
    require(step.force_delta(30,0,1)==7,"Wake retained a stale force fraction or delayed acceleration");
    for(unsigned axis=0;axis<6;++axis) {
        const int limit=axis==1 ? 50 : axis>=3 ? 64 : 200;
        for(int demand:{-limit-1,-limit,limit-1,limit}) {
            step.reset();
            do {step.begin_step();}while(!step.ends_callback());
            step.finish_settled_rest(true,zero,zero);
            auto unstable=load;unstable[axis]=word(demand);
            const bool stable=demand>=-limit && demand<limit;
            require(step.prepare_settled_rest(true,zero,unstable,zero)==stable,
                    "Rest pinned an unsupported pose or changed the original deadband");
            if(!stable)require(step.force_delta(300,0,axis)==70,
                              "Rejected rest swallowed the next force");
        }
        step.reset();arm(step);
        auto changed=load;changed[axis]=word(signed_word(changed[axis])+1);
        require(!step.prepare_settled_rest(true,zero,changed,zero),"Changed support load did not wake");
        step.reset();arm(step);
        auto impulse=zero;impulse[axis]=300;
        require(!step.prepare_settled_rest(true,zero,load,impulse),"Collision impulse was swallowed");
        require(step.force_delta(300,300,axis)==300,"Wake scaled a collision impulse");
        step.reset();arm(step);
        auto motion=zero;motion[axis]=1;
        require(!step.reset_settled_contacts(true,motion),"Existing momentum was discarded on wake");
        require(!step.prepare_settled_rest(true,motion,load,zero),"Moving body remained asleep");
    }
    step.reset();arm(step);step.reset();
    require(!step.prepare_settled_rest(true,zero,load,zero),"Vehicle reset retained rest state");
    srf::VehicleTimestep unit(1,1);
    for(unsigned tick=0;tick<20;++tick) {
        unit.begin_step();unit.finish_settled_rest(true,zero,zero);
        require(!unit.reset_settled_contacts(true,zero),"Unity changed original contact history");
        require(!unit.prepare_settled_rest(true,zero,load,zero),"Unity enabled a new stop policy");
        require(unit.force_delta(load[1],0,1)==load[1],"Unity changed original forces");
    }
}
}

int main() {
    try {
        unity_and_limits();
        longitudinal_measurement();
        longitudinal_force_override();
        wall_response_split();
        wall_transfer_ledger();
        elapsed_time_and_event_clock();
        independent_histories_and_reset();
        attachment_wrapping();
        contact_measurement_horizon();
        callback_boundary_pose();
        pose_force_order();
        stopping_after_accumulated_force();
        saturation_and_discard();
        original_force_ledger();
        held_controls_and_physical_demand();
        boost_curve_and_engine_clocks();
        transient_flags_and_default_clock();
        damage_sample_clock();
        motion_damping_and_bounds();
        stopping_thresholds_and_ordering();
        horizontal_rest_and_breakaway();
        recovery_heading_clock();
        neutral_yaw_rest_and_wakeup();
        suspension_rest_and_wakeup();
        settled_rest_and_wakeup();
        std::cout << "Timestep arithmetic, attachment histories, force ledger, control/damage clocks and discrete motion rules passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
