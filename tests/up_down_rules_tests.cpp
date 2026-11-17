#include "srf/rock_field_session.hpp"
#include "stunt_test_inputs.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
using namespace srf;

void check(bool value,const std::string& reason) {
    if(!value)throw std::runtime_error(reason);
}

struct Attempt {
    std::string car,run;
    unsigned phase,ticks;
    std::array<int,9> pose; // XYZ, yaw, pitch, speed, boost, damage, steering.
    std::array<unsigned,3> bounds;
    unsigned stars,bank,transfer,section,seconds,fraction,display;
    char outcome;
    std::array<unsigned,3> grip{}; // Ice surface flags and world X/Z velocities.
};

struct Car {
    const char* name;
    PlayerCar kind;
    unsigned start,stars,rest,finish_phase;
    unsigned desktop_rest,final_bank,finish_fields,park_fields;
    std::array<unsigned,3> first_clock; // First PLAY, retry after goal, retry after timeout.
};
constexpr std::array up_down_cars{
    Car{"4wd",PlayerCar::four_wd,50,31,70,972,71,24,3757,3014,{457,455,455}},
    Car{"coupe",PlayerCar::coupe,60,29,60,1199,58,22,4710,3615,{458,453,455}},
    Car{"f-type",PlayerCar::f_type,60,24,62,967,62,26,3722,3615,{456,454,458}}
};
constexpr std::array ice_cars{
    Car{"4wd",PlayerCar::four_wd,50,25,43,1139,42,13,4445,3015,{461,460,453}},
    Car{"coupe",PlayerCar::coupe,60,34,55,1433,49,19,5693,3615,{452,453,451}},
    Car{"f-type",PlayerCar::f_type,60,35,57,1446,53,19,5750,3616,{457,463,466}}
};
constexpr std::array<unsigned,9> body_offsets{0x1e,0x20,0x22,0x24,0x26,0x4a,0x6a,0x82,0x3a};

std::vector<Attempt> read_trace(const std::filesystem::path& path,bool ice) {
    std::ifstream input(path);
    check(input.good(),"Missing Stunt Trax cartridge trace");
    std::vector<Attempt> trace;
    std::string line;
    std::getline(input,line);
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);
        Attempt t{};
        row>>t.car>>t.run>>t.phase>>t.ticks;
        for(auto& value:t.pose)row>>value;
        for(auto& value:t.bounds)row>>value;
        check(bool(row>>t.stars>>t.bank>>t.transfer>>t.section>>t.seconds>>t.fraction>>t.display>>t.outcome),
              "Malformed Stunt Trax cartridge trace");
        if(ice)for(auto& value:t.grip)check(bool(row>>value),"Malformed ice grip sample");
        trace.push_back(t);
    }
    return trace;
}

RockFieldOutcome expected(char value) {
    return value=='F'?RockFieldOutcome::finished:value=='T'?RockFieldOutcome::timeout:RockFieldOutcome::driving;
}

void reference_step(RockFieldRules& rules,const VehicleState& body,const Attempt& sample) {
    // Full rows cover an entire GSU phase. The recorder stops a timeout row
    // on the video field the CPU accepts it, so its elapsed NMI ticks have
    // already reached the clock when the final object check runs.
    const auto ticks=static_cast<std::uint8_t>(sample.ticks);
    if(sample.outcome=='T') {
        rules.advance_clock(ticks);
        rules.step(body,0);
    } else rules.step(body,ticks);
}

void compare_rules(const Rom& rom,const Scene& scene,const Car& car,const std::vector<Attempt>& trace) {
    std::string attempt;
    std::optional<RockFieldRules> rules;
    unsigned retries=0,finishes=0,timeouts=0,previous=0;
    for(const auto& t:trace) {
        if(t.car!=car.name)continue;
        const auto where=t.car+" "+t.run+" "+std::to_string(t.phase);
        if(attempt!=t.run) {
            attempt=t.run;
            rules.emplace(rom,scene,car.kind);
            previous=0;
            check(rules->clock().remaining_seconds==car.start && !rules->stars() &&
                  !rules->bank_seconds() && rules->section()==1,where+" initial rules");
        }
        check(t.phase==++previous && t.ticks<15,where+" update sequence");
        VehicleState body{};
        for(unsigned i=0;i<3;++i) {
            body.word(body_offsets[i])=std::uint16_t(t.pose[i]);
            body.word(12+2*i)=std::uint16_t(t.bounds[i]);
        }
        reference_step(*rules,body,t);
        check(rules->stars()==t.stars && rules->section()==t.section && rules->outcome()==expected(t.outcome),
              where+" stars, section or outcome");
        check(rules->clock().remaining_fraction==t.fraction,where+" clock fraction");
        const auto bank=t.transfer?t.transfer:t.bank;
        const auto moved=int(t.seconds)-int(rules->clock().remaining_seconds);
        check(int(rules->bank_seconds())-int(bank)==moved,where+" clock plus bank");
        // The CPU can transfer the next chunk before the sampled GSU phase advances.
        // Every other sample must match both counters individually.
        int early=0;
        if(t.run=="finish" && scene.start_offset==0x6cc71) {
            if(t.car=="4wd" && t.phase==758)early=17;
            if(t.car=="4wd" && t.phase==759)early=3;
            if(t.car=="coupe" && (t.phase==337 || t.phase==850))early=1;
        }
        if(t.run=="finish" && scene.start_offset==0xadb6f) {
            if(t.car=="coupe" && t.phase==460)early=17;
            if(t.car=="coupe" && t.phase==464)early=1;
            if(t.car=="coupe" && t.phase==916)early=3;
            if(t.car=="f-type" && t.phase==346)early=1;
            if(t.car=="f-type" && t.phase==660)early=3;
            if(t.car=="f-type" && t.phase==1244)early=17;
        }
        check(moved==0 || moved==early,where+" bank transfer");
        const auto ahead=int(t.display)-int(rules->display_bank_seconds());
        check(ahead==-moved || (!moved && ahead==1),where+" HUD bank count");
        if(t.outcome=='F') {
            ++finishes;
            check(t.phase==car.finish_phase && t.stars==car.stars && rules->result_seconds()==car.rest,
                  where+" goal and result");
        }
        if(t.outcome=='T') {
            ++timeouts;
            check(t.run=="park" && !t.stars && !t.seconds && !bank,where+" parked timeout");
        }
        if(t.run.starts_with("retry") && t.phase==160) {
            ++retries;
            check(t.seconds<car.start && !t.stars && !bank && t.section==1,where+" retry handoff");
        }
    }
    check(finishes==1 && timeouts==1 && retries==2,"Missing original finish, timeout or retry");
}

void drive_original(const Rom& rom,const Car& car,const std::vector<Attempt>& trace,
                    const std::vector<std::uint16_t>& controls,std::string_view attempt,StuntCourse course) {
    const auto start=attempt=="retry-f"?StuntAttempt::after_finish
        :attempt=="retry-t"?StuntAttempt::after_timeout:StuntAttempt::first;
    RockFieldDrive drive(rom,car.kind,course,start);
    RockFieldRules rules(rom,drive.scene(),car.kind);
    for(const auto& t:trace) {
        if(t.car!=car.name || t.run!=attempt || t.phase<drive.phase())continue;
        const auto buttons=attempt=="finish"?controls.at(t.phase-1):0;
        if(t.phase>drive.phase()) {
            drive.set_event_contact(rules.body_contact(drive.player().body));
            if(drive.drive_started())drive.step(buttons);
            else while(!drive.advance_intro(buttons) && !drive.drive_started()) {}
        }
        const auto where=t.car+" native "+t.run+" "+std::to_string(t.phase);
        check(drive.phase()==t.phase,where+" cadence");
        const auto& body=drive.player().body;
        const auto before=rules.section();
        reference_step(rules,body,t);
        if(rules.section()!=before)drive.checkpoint_reward(rules.last_gate_command());
        for(unsigned i=0;i<body_offsets.size();++i)
            check(std::uint16_t(t.pose[i])==body.word(body_offsets[i]),where+" vehicle word "+std::to_string(body_offsets[i]));
        if(course==StuntCourse::ice_dance) {
            const std::array<unsigned,3> grip{body.word(0x8a),body.word(0x2e),body.word(0x32)};
            check(grip==t.grip,where+" ice surface or horizontal velocity");
        }
        check(rules.stars()==t.stars && rules.section()==t.section && rules.outcome()==expected(t.outcome),
              where+" race events");
    }
    if(attempt=="finish")
        check(rules.outcome()==RockFieldOutcome::finished && rules.result_seconds()==car.rest,
              std::string(car.name)+" controller-driven result");
    else if(attempt=="park")
        check(rules.outcome()==RockFieldOutcome::timeout && !rules.result_seconds(),
              std::string(car.name)+" parked result");
    else check(drive.phase()==160 && drive.drive_started() && !rules.stars() && !rules.bank_seconds() &&
               rules.outcome()==RockFieldOutcome::driving,std::string(car.name)+" retry into driving");
}

void desktop(const Rom& rom,const Car& car,const std::vector<std::uint16_t>& controls,bool park,StuntCourse course) {
    RockFieldSession session(rom,car.kind,course);
    while(session.tick<8000 && session.rules.outcome()==RockFieldOutcome::driving) {
        const auto phase=session.drive.phase();
        session.advance(!park && phase<controls.size()?controls[phase]:0);
    }
    const auto& rules=session.rules;
    check(session.tick+1-session.released==car.first_clock[0],std::string(car.name)+" first countdown field");
    if(park) {
        check(rules.outcome()==RockFieldOutcome::timeout && !rules.stars() && !rules.bank_seconds() &&
              session.released==car.park_fields,std::string(car.name)+" desktop parked timeout");
    } else {
        check(rules.outcome()==RockFieldOutcome::finished && rules.stars()==car.stars &&
              session.drive.phase()==car.finish_phase,std::string(car.name)+" desktop controller finish");
        check(rules.result_seconds()==car.desktop_rest && rules.bank_seconds()==car.final_bank &&
              session.released==car.finish_fields,std::string(car.name)+" desktop clock");
    }
    std::cout<<car.name<<(park?" parked":" finish")<<", fields "<<session.released
             <<", stars "<<rules.stars()<<", REST "<<rules.result_seconds()<<"\n";
}

void desktop_retry(const Rom& rom,const Car& car,StuntAttempt start,const Attempt& target,StuntCourse course) {
    RockFieldSession session(rom,car.kind,course,start);
    check(session.rules.clock().remaining_seconds==car.start,std::string(car.name)+" retry clock reset");
    while(session.drive.phase()<target.phase && session.tick<1200)session.advance(0);
    const auto where=std::string(car.name)+" "+target.run+" desktop handoff";
    const auto first_clock=car.first_clock[start==StuntAttempt::after_finish?1:2];
    check(session.tick+1-session.released==first_clock,where+" first countdown field");
    check(session.drive.phase()==target.phase && session.drive.drive_started(),where);
    for(unsigned i=0;i<body_offsets.size();++i)
        check(session.drive.player().body.word(body_offsets[i])==std::uint16_t(target.pose[i]),where+" car state");
    check(session.rules.outcome()==RockFieldOutcome::driving && !session.rules.stars() &&
          !session.rules.bank_seconds() && session.rules.section()==1,where+" rules reset");
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        const bool ice=argc>1 && std::wstring_view(argv[1])==L"--ice-dance";
        const auto* path=argc>1+ice?argv[1+ice]:_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        const std::filesystem::path fixtures=SRF_TEST_FIXTURES;
        const std::string name=ice?"ice_dance":"up_down";
        const auto course=ice?StuntCourse::ice_dance:StuntCourse::up_n_down;
        const auto trace=read_trace(fixtures/(name+"_rules.csv"),ice);
        check(trace.size()==(ice?8343:7108),"Incomplete original Stunt Trax comparisons");
        const auto rom=Rom::load(path);
        const auto scene=extract_scene(rom,ice?0xadb6f:0x6cc71);
        for(const auto& car:(ice?ice_cars:up_down_cars)) {
            const auto controls=read_stunt_controls(fixtures/(name+"_native_inputs.csv"),car.name);
            check(controls.size()==car.finish_phase,"Incomplete controller recording");
            compare_rules(rom,scene,car,trace);
            for(const auto* attempt:{"finish","park","retry-f","retry-t"})
                drive_original(rom,car,trace,controls,attempt,course);
            desktop(rom,car,controls,false,course);
            desktop(rom,car,controls,true,course);
            for(const auto& t:trace)if(t.car==car.name && t.phase==160 && t.run.starts_with("retry"))
                desktop_retry(rom,car,t.run=="retry-f"?StuntAttempt::after_finish:StuntAttempt::after_timeout,t,course);
        }
        std::cout<<trace.size()<<" original phase comparisons passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
