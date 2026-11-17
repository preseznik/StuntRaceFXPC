#include "srf/rock_field_rules.hpp"
#include "srf/rock_field_session.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
void require(bool condition, const std::string& message) {
    if(!condition)throw std::runtime_error(message);
}

std::vector<std::uint16_t> finish_inputs(const std::filesystem::path& file) {
    std::ifstream input(file);
    std::string line;
    std::getline(input,line);
    std::vector<std::uint16_t> controls;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);
        unsigned count,buttons;
        require(bool(row>>count>>std::hex>>buttons) && count<=711,"Invalid driving input run");
        controls.insert(controls.end(),count,static_cast<std::uint16_t>(buttons));
    }
    require(controls.size()==711,"Incomplete Rock Field driving inputs");
    return controls;
}

void play_at_desktop_cadence(const srf::Rom& rom, const std::vector<std::uint16_t>& controls) {
    for(bool parked : {false,true}) {
        srf::RockFieldSession session(rom,srf::PlayerCar::four_wd);
        while(session.rules.outcome()==srf::RockFieldOutcome::driving && session.tick<4000) {
            const auto phase=session.drive.phase();
            session.advance(!parked && phase<controls.size()?controls[phase]:0);
        }
        if(parked) {
            require(session.rules.outcome()==srf::RockFieldOutcome::timeout && session.rules.stars()==1 &&
                    session.released==2413,
                    "Desktop parked/retry did not time out with its automatic trailer-exit star");
        } else {
            const auto& car=session.drive.player().body;
            require(session.released>=2624 && session.released<=2632 &&
                    session.rules.outcome()==srf::RockFieldOutcome::finished && session.rules.stars()==34 &&
                    session.rules.result_seconds()==86 && session.drive.phase()==711 &&
                    car.word(0x1e)==std::uint16_t(-11462) && car.word(0x20)==std::uint16_t(-686) &&
                    car.word(0x22)==std::uint16_t(-12458) && car.word(0x82)==34,
                    "Desktop 14 Hz finish differs from the cartridge");
        }
        std::cout<<(parked?"Parked":"Finished")<<" at "<<session.released<<" desktop clock fields\n";
    }
}

void compare(const srf::Rom& rom, const std::filesystem::path& fixture) {
    std::ifstream input(fixture);
    require(input.good(),"Missing Rock Field cartridge trace");
    const auto scene=srf::extract_scene(rom,0x6bd71);
    std::optional<srf::RockFieldRules> rules;
    std::optional<srf::RockFieldDrive> drive;
    std::optional<srf::RockFieldRules> driven_rules;
    const auto controls=finish_inputs(fixture.parent_path()/"rock_field_native_inputs.csv");
    std::string line, previous;
    std::getline(input,line);
    unsigned verified=0, finishes=0, timeouts=0, retries=0, split_transfers=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);
        std::string run;
        unsigned update,ticks,dx,dy,dz,stars,bank,transfer,section,seconds,fraction,display_bank;
        int x,y,z;
        char outcome;
        require(bool(row>>run>>update>>ticks>>x>>y>>z>>dx>>dy>>dz>>stars>>bank>>transfer
                     >>section>>seconds>>fraction>>display_bank>>outcome),"Invalid cartridge trace");
        const std::string where=run+" update "+std::to_string(update);
        if(run!=previous) {
            rules.emplace(rom,scene);
            drive.emplace(rom,srf::PlayerCar::four_wd);
            driven_rules.emplace(rom,drive->scene());
            require(update==1 && rules->stars()==0 && rules->clock().remaining_seconds==40,where+" initial state");
            if(run=="retry")++retries;
            previous=run;
        }
        srf::VehicleState body{};
        body.word(0x1e)=static_cast<std::uint16_t>(x);
        body.word(0x20)=static_cast<std::uint16_t>(y);
        body.word(0x22)=static_cast<std::uint16_t>(z);
        body.word(0xc)=static_cast<std::uint16_t>(dx);
        body.word(0xe)=static_cast<std::uint16_t>(dy);
        body.word(0x10)=static_cast<std::uint16_t>(dz);
        rules->step(body,static_cast<std::uint8_t>(ticks));
        require(rules->stars()==stars && rules->section()==section,where+" pickup or checkpoint");
        require(rules->clock().remaining_fraction==fraction,where+" clock fraction");
        const auto shown=rules->display_bank_seconds();
        // VRAM upload and GSU phase counters are asynchronous. The only multi-second
        // split in this trace is the same checkpoint transfer checked below.
        require(shown==display_bank || shown+1==display_bank ||
                (run=="finish" && update==564 && shown==display_bank+3),where+" displayed star seconds");
        if(rules->clock().remaining_seconds!=seconds || rules->bank_seconds()!=(transfer?transfer:bank)) {
            // In this capture the CPU posts one three-second transfer before the
            // GSU phase counter advances. The next sample has both sides in sync.
            require(run=="finish" && update==564 && seconds==rules->clock().remaining_seconds+3
                    && transfer+3==rules->bank_seconds(),where+" clock or bank");
            ++split_transfers;
        }
        const auto expected=outcome=='F'?srf::RockFieldOutcome::finished
            :outcome=='T'?srf::RockFieldOutcome::timeout:srf::RockFieldOutcome::driving;
        require(rules->outcome()==expected,where+" outcome");
        if(outcome=='F') {
            ++finishes;
            require(rules->result_seconds()==86 && stars==34,where+" results");
        }
        if(outcome=='T') {
            ++timeouts;
            require(stars==1 && seconds==0,where+" parked timeout");
        }
        // Cold-start each attempt and drive using just latched controller buttons.
        // Observed XYZ is a comparison target. It never enters the native vehicle.
        if(update>=drive->phase()) {
            const std::uint16_t buttons=run=="finish"?controls.at(update-1):0;
            if(update>drive->phase()) {
                drive->set_event_contact(driven_rules->body_contact(drive->player().body));
                if(drive->drive_started())drive->step(buttons);
                else while(!drive->advance_intro(buttons) && !drive->drive_started()) {}
            }
            require(drive->phase()==update,where+" native update cadence");
            const auto& car=drive->player().body;
            require(car.word(0x1e)==body.word(0x1e) && car.word(0x20)==body.word(0x20)
                    && car.word(0x22)==body.word(0x22),where+" native vehicle position");
            auto previous_section=driven_rules->section();
            driven_rules->step(car,static_cast<std::uint8_t>(ticks));
            if(driven_rules->section()!=previous_section)drive->checkpoint_reward(driven_rules->last_gate_command());
            require(driven_rules->stars()==stars && driven_rules->section()==section
                    && driven_rules->outcome()==expected,where+" native driving and game rules");
            if(outcome=='F')require(driven_rules->result_seconds()==86,where+" driven finish results");
        }
        ++verified;
    }
    require(verified==1534 && finishes==1 && timeouts==1 && retries==1 && split_transfers<=1,
            "Incomplete Rock Field cartridge comparison");
    play_at_desktop_cadence(rom,controls);
    std::cout<<verified<<" original object updates: finish, timeout and retry passed\n";
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        const auto* path=argc>1?argv[1]:_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        const auto file=argc>2?std::filesystem::path(argv[2]):std::filesystem::path(SRF_TEST_FIXTURES)/"rock_field_rules.csv";
        compare(srf::Rom::load(path),file);
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
