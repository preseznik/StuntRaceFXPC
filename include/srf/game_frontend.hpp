#pragma once

#include "srf/championship.hpp"
#include "srf/stunt_course.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace srf {
struct GameCourseOption {
    std::string id;
    std::string name;
};

struct GameSessionChoice {
    std::string course_id;
    PlayerCar car = PlayerCar::four_wd;
    bool start = false;
    bool speed_trax = true;
    SpeedClass league = SpeedClass::novice;
    std::string initials = "AAA";
    bool stunt_trax = false;
    StuntCourse stunt_course = StuntCourse::rock_field;

    void select_stunt_course(StuntCourse course) {
        stunt_trax=true;
        speed_trax=false;
        stunt_course=course;
        course_id=stunt_course_id(course);
    }
};

// Native entry screen for a normal game launch. The caller supplies only courses
// that are playable in this build, and starts the chosen race when start is true.
// A screenshot renders the actual controls offscreen and returns without racing.
GameSessionChoice choose_game_session(std::span<const GameCourseOption> courses,
    const GameSessionChoice& initial, const ChampionshipProgress& progress,
    const std::filesystem::path& screenshot = {});

struct GameResultPage {
    std::wstring heading, subtitle, details, action, secondary = L"Back to menu";
};
// True when the driver chooses the primary action. Closing returns to selection.
bool show_game_result(const GameResultPage& page);
using ResultPresenter=std::function<bool(const GameResultPage&)>;
bool show_round_result(const Championship& championship,const RoundResult& result,
                       const std::array<std::uint32_t,3>& lap_times,const ResultPresenter& present=show_game_result);
void show_class_result(const Championship& championship,const ChampionshipProgress& progress,const ResultPresenter& present=show_game_result);
}
