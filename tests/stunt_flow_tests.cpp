#include "srf/audio.hpp"
#include "srf/boot_title.hpp"
#include "srf/boot_menu.hpp"
#include "srf/game_frontend.hpp"
#include "srf/rock_field_session.hpp"
#include "srf/stunt_board.hpp"
#include "srf/stunt_results.hpp"
#include "srf/stunt_scores.hpp"
#include "stunt_test_inputs.hpp"
#include <windows.h>
#include <iostream>

namespace {
using namespace srf;
namespace fs=std::filesystem;
constexpr unsigned accept=0x1000,up=0x0800,down=0x0400,left=0x0200,right=0x0100;

void require(bool ok,const char* what) {if(!ok)throw std::runtime_error(what);}

// Convert libretro masks from the cold-boot recording into the console button
// levels consumed by BootTitle, BootMenu and StuntBoard.
std::vector<std::uint16_t> menu_inputs(const fs::path& path) {
    std::ifstream file(path);
    require(file.good(),"Missing recorded Stunt Trax cold boot inputs");
    std::vector<std::uint16_t> tape(2800);
    std::string line;
    while(std::getline(file,line)) {
        if(line.empty() || line.front()=='#')continue;
        std::replace(line.begin(),line.end(),',',' ');
        unsigned first,last,mask;
        std::istringstream row(line);
        require(bool(row>>first>>last>>std::hex>>mask) && first && first<=last && last<tape.size(),
                "Malformed cold boot recording");
        std::uint16_t held=0;
        for(unsigned i=0;i<12;++i)if(mask&(1u<<i))held|=std::uint16_t(0x8000u>>i);
        std::fill(tape.begin()+first,tape.begin()+last+1,held);
    }
    return tape;
}

GameSessionChoice boot(std::vector<std::uint16_t> tape,PlayerCar car,StuntCourse course) {
    // These extra single-field edges choose the requested car and enter BAA.
    if(car!=PlayerCar::four_wd)tape.at(1290)=car==PlayerCar::coupe?right:left;
    tape.at(1470)=up;
    tape.at(1490)=right;
    BootTitle title;
    BootMenu menu;
    std::optional<StuntBoard> board;
    GameSessionChoice selection;
    bool splash=false,title_logo=false,initials=false;
    for(unsigned field=1;field<tape.size();++field) {
        const auto held=tape[field];
        if(!title.finished()) {
            title.advance((held&accept)!=0,(held&0x8000)!=0);
            splash|=title.frame().nintendo;
            title_logo|=title.frame().logo;
        } else if(!board) {
            menu.advance(held);
            initials|=menu.frame().screen==BootMenuScreen::initials;
            if(menu.event()==BootMenuEvent::board) {
                require(menu.frame().mode==1 && menu.car()==car && menu.initials()=="BAA",
                        "Stunt Trax car/initials handoff");
                selection.car=menu.car();selection.initials=menu.initials();
                board.emplace(selection.car);
            }
        } else {
            board->advance(held);
            if(board->ready_to_drive()) {
                selection.select_stunt_course(static_cast<StuntCourse>(board->course));
                require(splash && title_logo && initials && selection.course_id==stunt_course_id(course) && selection.stunt_trax,
                        "Normal boot did not reach the selected Stunt course");
                return selection;
            }
        }
    }
    throw std::runtime_error("Recorded menu input did not start the course");
}

void wait_for_page(StuntResults& flow,StuntPage page,PlayerCar& car) {
    for(unsigned i=0;i<400 && flow.page!=page;++i)flow.advance(0,car);
    require(flow.page==page,"Wrong Stunt Trax results page");
}
std::optional<StuntExit> press(StuntResults& flow,PlayerCar& car,std::uint16_t key) {
    flow.advance(0,car);
    return flow.advance(key,car);
}

struct Expected {const char* name;PlayerCar car;unsigned clock,stars,rest;};
constexpr std::array up_down{Expected{"4wd",PlayerCar::four_wd,50,31,71},Expected{"coupe",PlayerCar::coupe,60,29,58},
                             Expected{"f-type",PlayerCar::f_type,60,24,62}};
constexpr std::array ice_dance{Expected{"4wd",PlayerCar::four_wd,50,25,42},Expected{"coupe",PlayerCar::coupe,60,34,49},
                               Expected{"f-type",PlayerCar::f_type,60,35,53}};

void play(const Rom& rom,GameSessionChoice selection,const Expected& car,const fs::path& score_dir,
          const std::vector<std::uint16_t>& controls) {
    const auto score_file=score_dir/(selection.course_id+".sav");
    StuntAttempt attempt=StuntAttempt::first;
    for(unsigned run=0;run<3;++run) {
        RockFieldSession session(rom,selection.car,selection.stunt_course,attempt);
        const auto& rules=session.rules;
        require(!session.drive.drive_started() && rules.clock().remaining_seconds==car.clock &&
                !rules.stars() && !rules.bank_seconds() && rules.section()==1,"Trailer did not reset the course");
        while(rules.outcome()==RockFieldOutcome::driving && session.tick<8500) {
            const auto phase=session.drive.phase();
            session.advance(!run && phase<controls.size()?controls[phase]:0);
        }
        require(session.drive.drive_started() && session.released,"Trailer never handed off to driving");
        StuntResults flow;
        flow.end_run(rules,selection.initials,selection.car);
        if(!run) {
            require(rules.outcome()==RockFieldOutcome::finished && flow.score.stars==car.stars &&
                    flow.score.seconds==car.rest && flow.score.initials=="BAA","Incorrect native finish");
            save_stunt_score(score_file,selection.stunt_course,flow.score);
            wait_for_page(flow,StuntPage::finish,selection.car);
            for(unsigned i=0;i<flow.score.stars+flow.score.seconds;++i)flow.advance(0,selection.car);
            press(flow,selection.car,accept);
            require(flow.page==StuntPage::records,"Finish did not open records");
            const auto records=load_stunt_scores(score_file,selection.stunt_course);
            require(std::any_of(records.begin(),records.end(),[&](const auto& r){
                return r.car==car.car && r.initials=="BAA" && r.stars==car.stars && r.seconds==car.rest;
            }),"Accepted initials or score changed on disk");
            for(int i=0;i<15;++i)flow.advance(0,selection.car);
            press(flow,selection.car,accept);
            require(flow.page==StuntPage::retry,"Records did not open retry menu");
            // CHANGE CAR -> return -> TRY AGAIN, restoring the selected car before the retry.
            press(flow,selection.car,down);press(flow,selection.car,down);press(flow,selection.car,accept);
            require(flow.page==StuntPage::change_car,"Change Car menu");
            press(flow,selection.car,right);require(selection.car!=car.car,"Change Car did not select another car");
            press(flow,selection.car,left);press(flow,selection.car,accept);
            require(selection.car==car.car && press(flow,selection.car,accept)==StuntExit::retry_finish,"Finish retry");
            attempt=StuntAttempt::after_finish;
        } else {
            require(rules.outcome()==RockFieldOutcome::timeout && !flow.score.stars && !flow.score.seconds,
                    "Stationary car did not time out");
            wait_for_page(flow,StuntPage::retry,selection.car);
            if(run==1) {
                require(press(flow,selection.car,accept)==StuntExit::retry_timeout,"Timeout retry");
                attempt=StuntAttempt::after_timeout;
            } else {
                press(flow,selection.car,down);
                require(press(flow,selection.car,accept)==StuntExit::course,"Change Course menu");
                StuntBoard board(selection.car);
                while(!board.outline_visible())board.advance(0);
                require(board.course==static_cast<unsigned>(StuntCourse::rock_field),"Returned course-board cursor");
            }
        }
    }
    std::cout<<stunt_course_name(selection.stunt_course)<<" "<<car.name<<" BAA "<<car.stars<<"/40, "<<car.rest<<" s; finish, timeout, both retries and course return passed\n";
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        const auto* source=argc>=2?argv[1]:_wgetenv(L"SRF_ROM");
        if(!source)return 77;
        if(argc>3)throw std::invalid_argument("Usage: srf_stunt_flow_tests [ROM [new score directory]]");
        const auto rom=Rom::load(source);
        const std::filesystem::path fixtures=SRF_TEST_FIXTURES;
        const std::filesystem::path out=argc==3?std::filesystem::path(argv[2]):
            std::filesystem::temp_directory_path()/("srf-stunt-flow-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        require(std::filesystem::create_directory(out),"Choose a new directory for the test score file");
        const auto lake=StuntCourse::blue_lake;
        require(extract_stunt_audio(rom,lake).music.size()==3985,"Blue Lake course soundtrack");
        for(const auto car:{PlayerCar::four_wd,PlayerCar::coupe,PlayerCar::f_type}) {
            for(const auto attempt:{StuntAttempt::first,StuntAttempt::after_finish,StuntAttempt::after_timeout}) {
                RockFieldSession session(rom,car,lake,attempt);
                const auto trailer=session.drive.poses().front().position;
                const auto eye=session.drive.trailer_camera();
                require(eye[0]==trailer.x && std::abs(eye[2]-trailer.z)<600,"Blue Lake trailer camera");
                require(session.rules.star_poses().size()==40 && session.rules.gate_poses().size()==3 &&
                        session.rules.clock().remaining_seconds==(car==PlayerCar::four_wd?50u:60u),"Blue Lake start grid");
                while(session.tick<600 && !session.drive.drive_started())session.advance(0);
                require(session.drive.drive_started(),"Blue Lake trailer release");
            }
        }
        for(const auto course:{StuntCourse::up_n_down,StuntCourse::ice_dance}) {
            const std::string prefix=course==StuntCourse::ice_dance?"ice_dance":"up_down";
            const auto tape=menu_inputs(fixtures/(prefix+"_menu_inputs.csv"));
            for(const auto& car:course==StuntCourse::ice_dance?ice_dance:up_down)
                play(rom,boot(tape,car.car,course),car,out,read_stunt_controls(fixtures/(prefix+"_native_inputs.csv"),car.name));
        }
        for(const auto course:{StuntCourse::up_n_down,StuntCourse::ice_dance}) {
            const auto file=out/(std::string(stunt_course_id(course))+".sav");
            require(load_stunt_scores(file,course).size()==3,"Expected one record per car and course");
            if(argc!=3) {
                std::filesystem::remove(file);
                std::filesystem::remove(file.wstring()+L".lock");
            }
        }
        if(argc==3)std::wcout<<L"Scores: "<<out.wstring()<<L"\n";
        else std::filesystem::remove(out);
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
