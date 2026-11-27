#include "srf/local_race.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void check(const srf::Rom& rom,srf::LocalCourse course,srf::DrivingPhysics physics=srf::DrivingPhysics::original){
    using namespace srf;
    const bool battle=course.index!=practice_course.index;
    LocalRace race(rom,course,PlayerCar::four_wd,PlayerCar::coupe,false,{},physics);
    require(race.count()==(battle?2u:1u),"Participant count");
    const auto world=extract_world(rom,race.scene());
    require(!world.instances.empty() && !world.models.empty(),"Empty local course");
    require(!extract_local_sprites(rom,course.index).empty(),"Missing course sprites");
    extract_local_backdrop(rom,course.index);
    extract_local_audio(rom,course.index);
    const auto initial=race.car(0).body;
    auto drive=[&]{
        for(unsigned n=0;n<36;++n)race.step({0x10,0});
        require(!race.result() && race.car(0).body.word(0x4a)>0,"P1 cannot accelerate");
        if(battle)require(race.car(1).body.word(0x4a)<race.car(0).body.word(0x4a),"P2 copied P1 input");
        return race.car(0).body;
    };
    const auto moved=drive();
    race.reset();require(race.phase()==0 && race.car(0).body.word(0x1e)==initial.word(0x1e),"Restart grid");
    require(drive().word(0x22)==moved.word(0x22),"Deterministic restart");
    if(!battle){
        race.reset();
        const auto route=extract_route(race.scene());
        // Test Run has two widely separated checkpoints across its infield.
        // Drive through the junctions between them with solid barriers enabled.
        constexpr std::array<RacePosition,6> junctions{{{-24128,0,1792},{-5888,0,1792},
            {-5888,0,-7808},{1792,0,-7808},{1792,0,-24128},{-24128,0,-24128}}};
        unsigned junction=0;
        for(unsigned step=0;step<9000 && !race.result();++step){
            const auto& body=race.car(0).body;
            auto target=route.at(body.word(0x58)%route.size()).position;
            if(physics==DrivingPhysics::enhanced) {
                const auto& p=junctions[junction];
                if(std::hypot(double(p[0])-std::int16_t(body.word(0x1e)),double(p[2])-std::int16_t(body.word(0x22)))<3000)
                    junction=(junction+1)%junctions.size();
                target=junctions[junction];
            }
            const double angle=std::atan2(double(target[0])-std::int16_t(body.word(0x1e)),double(target[2])-std::int16_t(body.word(0x22)));
            const auto error=std::int16_t(int(angle*65536/6.28318530718)+body.word(0x24));
            auto buttons=std::uint16_t(error>600?2:error< -600?1:0);
            const int target_speed=physics==DrivingPhysics::enhanced?90:std::abs(int(error))>4800?145:275;
            const int speed=physics==DrivingPhysics::enhanced?int(std::int16_t(body.word(0x4a))):int(body.word(0x4a));
            if(speed<target_speed)buttons|=0x10;
            else if(physics==DrivingPhysics::enhanced && speed>target_speed+15)buttons|=0x40;
            race.step({buttons,0});
        }
        std::cout<<"Practice phase "<<race.phase()<<" lap "<<race.car(0).body.word(0x7a)<<" point "<<race.car(0).body.word(0x58)<<std::endl;
        require(race.result()==0,"Practice did not finish three laps");
    }
    if(battle){
        race.reset();
        for(unsigned i=0;i<36;++i)race.step({0,0x10});
        require(race.car(1).body.word(0x4a)>race.car(0).body.word(0x4a),"P2 cannot accelerate independently");
        LocalRace solo(rom,course,PlayerCar::four_wd,PlayerCar::coupe,true);
        for(unsigned i=0;i<10000 && !solo.result();++i)solo.step({0x40,0});
        std::cout<<"Course "<<course.index<<" phase "<<solo.phase()<<" CPU laps "<<solo.car(1).body.word(0x7a)<<std::endl;
        require(solo.result()==1 && solo.car(1).body.word(0x7a)==3,"CPU did not complete three laps");
        LocalRace unattended(rom,course,PlayerCar::four_wd,PlayerCar::coupe,false);
        for(unsigned i=0;i<5000 && !unattended.result();++i)unattended.step({});
        require(unattended.computer(0) && unattended.computer(1) && unattended.result().has_value(),
                "Unattended Battle did not finish");
        const auto finish=solo.phase();
        solo.step({0x40,0});require(solo.phase()==finish,"Finished race advanced");
        solo.reset();
        for(unsigned i=0;i<finish;++i)solo.step({0x40,0});
        require(solo.result()==1 && solo.phase()==finish,"Restart did not reproduce the finish");
    }
}

void check_walls(const srf::Rom& rom) {
    using namespace srf;
    LocalRace practice(rom,practice_course,PlayerCar::four_wd,PlayerCar::coupe,false,{},DrivingPhysics::enhanced);
    for(int i=0;i<25;++i) {
        practice.step({0x11});
        // Inside edge of the left curb from the starting grid into the T junction.
        require(std::int16_t(practice.car(0).body.word(0x1e))>=-26752+125,"4WD clipped through the Test Run curb");
    }
    require(std::int16_t(practice.car(0).body.word(0x22))>-1600,"Glancing curb strike stopped the car");

    const CourseWall wall{{0,0,-1000},{0,0,1000},-200,-200};
    const auto pose=create_4wd(rom.bytes(),{0,-130,0});
    const auto orientation=vehicle_orientation(pose.body,extract_4wd_tuning(rom.bytes()));
    auto cross=[&](RacePosition from,RacePosition to,std::span<const CourseWall> walls,
                   std::span<const CollisionShape> shapes=std::span<const CollisionShape>{}) {
        auto car=pose;
        for(unsigned i=0;i<3;++i) {
            car.body.word(0x1e + 2*i)=std::uint16_t(to[i]);
            car.body.word(0x2e + 2*i)=std::uint16_t(16*(to[i]-from[i]));
        }
        resolve_course_walls(car,from,orientation,walls,shapes);
        return car.body;
    };
    const std::span barrier(&wall,1);
    auto hit=cross({-600,-130,0},{600,-130,250},barrier);
    require(std::int16_t(hit.word(0x1e))<=-130 && hit.word(0x22)==250 && hit.word(0x32)==4000,
            "High-speed sweep failed to preserve wall clearance and tangent velocity");
    require(std::int16_t(cross({600,-130,0},{-600,-130,0},barrier).word(0x1e))>=130,"Wall leaked from its back");
    require(cross({-600,-500,0},{600,-500,0},barrier).word(0x1e)==600,"Airborne car hit a low barrier");
    require(std::int16_t(cross({-600,-700,0},{600,-30,0},barrier).word(0x1e))<=-130,
            "Descending car passed through the wall between height samples");
    require(cross({-600,-130,1400},{600,-130,1400},barrier).word(0x1e)==600,"Open wall end blocked the road");
    require(std::int16_t(cross({-130,-130,0},{-500,-130,0},barrier).word(0x1e))==-500,"Wall trapped a separating car");
    std::array<CourseWall,2> corner{{wall,{{-1000,0,0},{0,0,0},-200,-200}}};
    hit=cross({-600,-130,-600},{400,-130,400},corner);
    require(std::int16_t(hit.word(0x1e))<=-130 && std::int16_t(hit.word(0x22))<=-190,"Sweep cut a corner");
    auto moving=wall;
    moving.shape=0;moving.frame=1;
    std::array<CollisionShape,1> deck{};
    require(cross({-600,-130,0},{600,-130,0},{&moving,1},deck).word(0x1e)==600,"Invisible bridge pose blocked the car");
    deck[0].frame=1;
    require(std::int16_t(cross({-600,-130,0},{600,-130,0},{&moving,1},deck).word(0x1e))<=-130,"Active bridge pose leaked");
}

void check_racer_contacts(const srf::Rom& rom) {
    using namespace srf;
    const auto tune=extract_4wd_tuning(rom.bytes());
    const std::array tuning{tune,tune,tune};
    const auto make=[&](int x,int y,int z,int vx,int vz) {
        auto car=create_4wd(rom.bytes(),{std::int16_t(x),std::int16_t(y),std::int16_t(z)});
        car.body.word(0x2e)=std::uint16_t(vx);car.body.word(0x32)=std::uint16_t(vz);
        return car;
    };
    const auto x=[](const Vehicle& car){return std::int16_t(car.body.word(0x1e));};
    const auto vx=[](const Vehicle& car){return std::int16_t(car.body.word(0x2e));};
    // Both cars would end the phase clear on the wrong side of one another.
    const std::array<RacePosition,2> approach{{{-450,-140,0},{450,-140,0}}};
    std::array crossing{make(450,-140,90,7200,720),make(-450,-140,90,-7200,720)};
    auto rival=crossing[1];
    crossing[0].body.word(0x2a)/=4;
    const int mount=std::int16_t(rival.wheels[0].word(0x1e)-rival.body.word(0x1e));
    resolve_racer_contacts(crossing,approach,tuning,{},{});
    require(x(crossing[1])-x(crossing[0])>=316,"Fast rivals crossed through each other's tyres");
    require(vx(crossing[0])<=vx(crossing[1]) && vx(crossing[1])<0,"Impact failed to separate velocities by mass");
    require(crossing[0].body.word(0x32)==720 && crossing[1].body.word(0x32)==720,"Side impact removed tangential speed");
    require(std::int16_t(crossing[1].wheels[0].word(0x1e)-x(crossing[1]))==mount,"Car correction detached a tyre");
    std::array rear_end{make(0,-140,400,0,8000),make(0,-140,0,0,0)};
    const std::array<RacePosition,2> chasing{{{0,-140,-600},{0,-140,0}}};
    resolve_racer_contacts(rear_end,chasing,tuning,{},{});
    require(std::int16_t(rear_end[1].body.word(0x22)-rear_end[0].body.word(0x22))>=490 &&
            std::int16_t(rear_end[1].body.word(0x32))>std::int16_t(rear_end[0].body.word(0x32)),
            "Rear-end collision passed through the bumper or failed to push the leading car");
    std::array stacked{make(450,-800,90,7200,720),rival};
    auto overpass=approach;overpass[0][1]=-800;
    resolve_racer_contacts(stacked,overpass,tuning,{},{});
    require(x(stacked[0])==450 && x(stacked[1])==-450,"Cars on separate decks collided");
    // Parked door-to-door against a wall, with a third car pushing the queue.
    const CourseWall wall{{0,0,-1000},{0,0,1000},-300,-300};
    std::array queue{make(-131,-140,0,0,0),make(-300,-140,0,0,0),make(-520,-140,0,0,0)};
    const auto settle=[&] {
        std::array<RacePosition,3> parked{};
        for(unsigned i=0;i<queue.size();++i)parked[i]={x(queue[i]),-140,0};
        resolve_racer_contacts(queue,parked,tuning,{&wall,1},{});
    };
    for(int i=0;i<3;++i)settle();
    require(x(queue[0])<=-130 && x(queue[0])-x(queue[1])>=314 && x(queue[1])-x(queue[2])>=314,"Pile-up crossed a car or wall");
    for(const auto& car:queue)require(vx(car)==0,"Resting contact manufactured speed");
    // Two ticks of left steering put the 4WD alongside the F-TYPE on
    // Easy Ride's starting straight. Their outer tyres must stay apart.
    NativeRace race(rom,PlayerCar::four_wd,CourseId::easy_ride,{},{},DrivingPhysics::enhanced);
    while(race.phase()<101) {
        const bool turn=race.phase()==87 || race.phase()==88;
        race.step(turn?0x411:0x410);
        if(race.phase()<100)continue;
        const auto& ai=race.racers().front().body;
        const auto& player=race.racers()[race.player_index()].body;
        const int dx=std::int16_t(player.word(0x1e)-ai.word(0x1e));
        const int dz=std::int16_t(player.word(0x22)-ai.word(0x22));
        require(std::abs(dz)<220 && dx>=285 && dx<360,"Easy Ride player clipped the AI during a boosted pass");
    }
    // On the LAN grid the first driver brakes and the Coupé turns into its
    // rear corner. Compare against the same braking input with a parked guest.
    LocalRace lan(rom,lan_courses[0],PlayerCar::four_wd,PlayerCar::coupe,false,{},DrivingPhysics::enhanced);
    const auto drive=[&](bool chase) {
        lan.reset();
        for(unsigned phase=0;phase<37;++phase) {
            const std::uint16_t host=phase<9?0x10:0x40;
            const std::uint16_t guest=chase && phase>=18?(phase<28?0x12:0x10):0;
            lan.step({host,guest});
        }
        return lan.car(0).body;
    };
    const auto parked=drive(false),bumped=drive(true);
    require(std::int16_t(bumped.word(0x22)-parked.word(0x22))>30 && std::int16_t(bumped.word(0x32))>100,
            "LAN driver passed through the braking 4WD without transferring momentum");
}

void check_drift(const srf::Rom& rom) {
    using namespace srf;
    for(auto model:{DrivingPhysics::original,DrivingPhysics::enhanced})for(int direction:{1,-1}) {
        LocalRace race(rom,practice_course,PlayerCar::four_wd,PlayerCar::coupe,false,{},model);
        const auto drive=[&](bool held) {
            race.reset();
            race.step({drift_button});
            require(!race.car(0).drift_ticks,"Drift engaged at rest");
            while(std::int16_t(race.car(0).body.word(0x4a))<135 && race.phase()<120)race.step({0x10});
            const unsigned turn=direction>0?1:2;
            for(unsigned i=0;i<7;++i)race.step({std::uint16_t(0x10|turn|(held?drift_button:0))});
            const auto sample=race.car(0);
            for(unsigned i=0;i<12;++i)race.step({0x10});
            require(!race.car(0).drift_ticks && !race.car(0).drift_direction,"Drift did not release");
            return sample;
        };
        const auto ordinary=drive(false),slide=drive(true);
        if(model==DrivingPhysics::original) {
            require(slide.body.words==ordinary.body.words,"Drift changed Original physics");
            continue;
        }
        const auto side_speed=[](const Vehicle& car) {
            const auto& b=car.body;
            const double yaw=std::int16_t(b.word(0x24))*6.28318530718/65536;
            return std::abs(std::int16_t(b.word(0x2e))*std::cos(yaw)+std::int16_t(b.word(0x32))*std::sin(yaw));
        };
        const int yaw=direction*std::int16_t(slide.body.word(0x24)-ordinary.body.word(0x24));
        std::cout<<"Drift yaw delta "<<yaw<<", lateral speed "<<side_speed(ordinary)<<" -> "<<side_speed(slide)<<'\n';
        require(slide.drift_direction==direction && slide.drift_ticks==4 && yaw>800 &&
                side_speed(slide)>side_speed(ordinary)+80 && std::int16_t(slide.body.word(0x4a))>70,
                "Drift failed to sustain a moving, directional slide");
    }
    for(auto model:{PlayerCar::four_wd,PlayerCar::coupe,PlayerCar::f_type,PlayerCar::kestrel,PlayerCar::cargo}) {
        NativeRace race(rom,model,CourseId::easy_ride,{},{},DrivingPhysics::enhanced);
        const auto turn=[&](bool countersteer) {
            race.reset();
            while(race.phase()<100 && (race.countdown_active() || std::int16_t(race.racers().back().body.word(0x4a))<100))
                race.step(0x410);
            race.step(0x10,4,-24576,nullptr,true);
            require(race.racers().back().drift_direction==1,"Quick Race analog drift");
            for(unsigned i=0;i<5;++i)race.step(0x10,4,countersteer?24576:-24576,nullptr,true);
            return race.racers().back().body.word(0x24);
        };
        const auto tight=turn(false),wide=turn(true);
        require(std::int16_t(tight-wide)>300,"Countersteering did not open the drift");
    }
    LocalRace lan(rom,lan_courses[0],PlayerCar::four_wd,PlayerCar::coupe,false,{},DrivingPhysics::enhanced);
    for(unsigned i=0;i<40;++i)lan.step({0,0x10});
    lan.step({0,std::uint16_t(0x12|drift_button)});
    require(!lan.car(0).drift_ticks && lan.car(1).drift_direction==-1,"LAN guest drift ownership");
    for(unsigned i=0;i<18;++i)lan.step({0,std::uint16_t(0x10|drift_button)});
    require(lan.car(1).drift_ticks==4 && !lan.car(1).drift_direction,"Held drift expired or ignored centered steering");
    lan.step({0,std::uint16_t(0x11|drift_button)});
    require(lan.car(1).drift_direction==1,"Held drift failed to change direction");
    for(unsigned i=0;i<4;++i)lan.step({0,0x10});
    require(!lan.car(1).drift_ticks,"Released drift did not regain grip");
}

void check_drift_boost(const srf::Rom& rom) {
    using namespace srf;
    LocalRace practice(rom,practice_course,PlayerCar::four_wd,PlayerCar::coupe,false,{},DrivingPhysics::enhanced);
    const auto accelerate=[&] {
        practice.reset();
        while(practice.phase()<100 && std::int16_t(practice.car(0).body.word(0x4a))<135)practice.step({0x10});
    };
    const auto exit_speed=[&](int ticks,bool release) {
        accelerate();
        for(int i=0;i<ticks;++i) {
            const std::int16_t steer=(i/5)%2?12000:-12000;
            practice.step({0x10|drift_button},{steer});
        }
        require(practice.car(0).drift_charge_ticks==std::min(ticks,28),"Drift charge duration or cap");
        const auto reserve=practice.car(0).body.word(0x6a);
        practice.step({std::uint16_t(0x10|(release?0:drift_button))});
        const auto& car=practice.car(0);
        require(car.body.word(0x6a)==reserve,"Drift release spent regular boost");
        if(release)require(car.drift_charge_ticks==0,"Drift charge survived release");
        return int(std::int16_t(car.body.word(0x4a)));
    };
    // Compare the same trajectory at the release phase. Rear grip recovers on the next phase.
    for(const auto [ticks,expected]:{std::pair{9,0},{10,24},{19,54},{28,84},{35,84}}) {
        const int kick=exit_speed(ticks,true)-exit_speed(ticks,false);
        require(std::abs(kick-expected)<=1,"Drift release speed did not scale with held time");
    }
    accelerate();
    for(unsigned i=0;i<28;++i)practice.step({0x10|drift_button});
    require(!practice.car(0).drift_charge_ticks,"Straight driving charged a drift boost");
    for(unsigned i=0;i<20;++i) {
        practice.step({0x11|drift_button});
        practice.step({0x10});
        require(!practice.car(0).drift_charge_ticks,"Short drifts banked charge across releases");
    }
    for(auto physics:{DrivingPhysics::enhanced,DrivingPhysics::original}) {
        NativeRace race(rom,PlayerCar::coupe,CourseId::easy_ride,{},{},physics);
        const auto quick_exit=[&](bool release) {
            race.reset();
            while(race.phase()<100 && (race.countdown_active() || std::int16_t(race.racers().back().body.word(0x4a))<110))
                race.step(0x410);
            for(int i=0;i<14;++i)race.step(0x10,4,(i/5)%2?12000:-12000,nullptr,true);
            race.step(0x10,4,0,nullptr,!release);
            return int(std::int16_t(race.racers().back().body.word(0x4a)));
        };
        const int gain=quick_exit(true)-quick_exit(false);
        require(physics==DrivingPhysics::enhanced?gain>=36:gain==0,"Quick Race drift release or Original isolation");
    }
    LocalRace lan(rom,lan_courses[0],PlayerCar::four_wd,PlayerCar::coupe,false,{},DrivingPhysics::enhanced);
    const auto guest_exit=[&](bool release) {
        lan.reset();
        for(int i=0;i<40;++i)lan.step({0,0x10});
        for(int i=0;i<14;++i)lan.step({0,0x10|drift_button},{std::nullopt,(i/5)%2?12000:-12000});
        lan.step({0,std::uint16_t(0x10|(release?0:drift_button))});
        require(!lan.car(0).drift_charge_ticks,"Guest charged another driver's boost");
        return int(std::int16_t(lan.car(1).body.word(0x4a)));
    };
    require(guest_exit(true)-guest_exit(false)>=36,"LAN guest drift release");
}

void check_enhanced_physics(const srf::Rom& rom) {
    using namespace srf;
    auto measure=[&](DrivingPhysics physics) {
        LocalRace race(rom,practice_course,PlayerCar::four_wd,PlayerCar::coupe,false,{},physics);
        auto speed=[&]{return std::int16_t(race.car(0).body.word(0x4a));};
        while(speed()<120 && race.phase()<120)race.step({0x10});
        require(speed()>=120,"Practice failed to reach reference speed");
        const unsigned acceleration=race.phase();
        while(speed()>4 && race.phase()<acceleration+120)race.step({0x40});
        require(speed()<=4,"Service brake failed to stop the car");
        return std::pair{acceleration,race.phase()-acceleration};
    };
    const auto original=measure(DrivingPhysics::original),enhanced=measure(DrivingPhysics::enhanced);
    std::cout<<"Practice acceleration/braking phases: original "<<original.first<<'/'<<original.second
             <<", enhanced "<<enhanced.first<<'/'<<enhanced.second<<'\n';
    require(enhanced.first<original.first && enhanced.second<original.second,"Enhanced acceleration or braking regression");

    VehicleState car{},other{};
    car.word(0x16)=0x10;
    car.word(0x2a)=other.word(0x2a)=256;
    car.word(0x1e)=100;
    auto impact=[&](int closing,DrivingPhysics model) {
        BodyContactCandidate contact{{0,0},{std::uint16_t(closing),2000},&other};
        VehicleForces forces{};BodyCollisionWork work{};
        update_body_collisions(car,forces,{&contact,1},{0,0,model},work);
        require(forces.words[2]==0,"Glancing impact changed tangential speed");
        return std::int16_t(forces.words[0]);
    };
    require(impact(-100,DrivingPhysics::enhanced)==0,"Separating bodies received another kick");
    const int bump=impact(100,DrivingPhysics::enhanced);
    require(bump>0 && bump<100 && bump<impact(100,DrivingPhysics::original),"Low speed bump produced a launch impulse");

    // A launched car with no wheel supports must cross an apex under constant gravity.
    auto jumping=create_4wd(rom.bytes(),{0,-2000,0});
    jumping.body.word(0x1c)|=8; // Practice and direct race run the player's two half steps per phase.
    jumping.body.word(0x30)=std::uint16_t(-600);
    jumping.body.word(0x32)=1800;
    jumping.body.word(0x36)=600;
    const auto tune=extract_4wd_tuning(rom.bytes());
    const auto wheels=extract_wheel_contact_tuning(rom,tune.contact_parameters);
    const auto opponent=extract_opponent_tuning(rom);
    VehicleDriveContext context{};
    VehicleStepEnvironment air{};
    air.physics=DrivingPhysics::enhanced;
    for(int i=0;i<10;++i) {
        const auto before=std::int16_t(jumping.body.word(0x30));
        const auto result=update_player_vehicle(jumping,tune,wheels,opponent,context,air);
        require(jumping.body.word(0x44)==4 && result.forces.words[1]==120 &&
            std::int16_t(jumping.body.word(0x30))==before+120,"Airborne gravity changed with speed or pitch");
    }
    require(std::int16_t(jumping.body.word(0x30))>0 && std::abs(std::int16_t(jumping.body.word(0x36)))<600,
            "Jump did not descend with damped pitch");

    LocalRace ground(rom,practice_course,PlayerCar::four_wd,PlayerCar::four_wd,false);
    for(int i=0;i<28;++i)ground.step({});
    auto touchdown=[&](DrivingPhysics physics) {
        auto dropped=ground.car(0);
        dropped.body.word(0x20)-=200;
        dropped.body.word(0x30)=200;
        dropped.body.word(0x44)=4;
        VehicleStepEnvironment env{};
        env.physics=physics;
        VehicleDriveContext drive{};
        const auto orientation=vehicle_orientation(dropped.body,tune);
        for(unsigned wheel=0;wheel<4;++wheel) {
            env.wheel_candidates[wheel]=ground.wheel_surface(0,wheel);
            position_vehicle_attachment(dropped.body,dropped.wheels[wheel],orientation);
        }
        for(int i=0;i<40;++i) {
            update_player_vehicle(dropped,tune,wheels,opponent,drive,env);
            if(dropped.body.word(0x44)<4)return std::int16_t(dropped.body.word(0x30));
        }
        throw std::runtime_error("Drop missed the practice surface");
    };
    require(std::abs(touchdown(DrivingPhysics::enhanced))<std::abs(touchdown(DrivingPhysics::original)),
            "Enhanced suspension did not absorb the landing");
}
}
int wmain(){
    try{
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        for(const auto course:srf::battle_courses)check(rom,course);
        check(rom,srf::practice_course);
        check(rom,srf::practice_course,srf::DrivingPhysics::enhanced);
        check_enhanced_physics(rom);
        check_drift(rom);
        check_drift_boost(rom);
        check_racer_contacts(rom);
        check_walls(rom);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
