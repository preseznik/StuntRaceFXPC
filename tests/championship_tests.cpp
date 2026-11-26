#include "srf/championship.hpp"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <future>
#include <iostream>

namespace {
using namespace srf;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<typename Function> void rejects(Function call, const char* message) {
    try { call(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

void clear_class(ChampionshipProgress& progress, SpeedClass league, PlayerCar car,
                 const std::string& initials, unsigned time) {
    Championship session(league, car, initials);
    for (unsigned i = 0; i < 4; ++i) {
        check(session.round() == i + 1 && static_cast<unsigned>(session.course()) ==
            static_cast<unsigned>(league) * 4 + i, "Wrong championship round");
        const auto outcome = session.finish({session.course(), RaceFailure::none,
            static_cast<std::uint16_t>(1 + i % 3), 108, time + i}, progress);
        if (i == 3) {
            check(outcome == ChampionshipOutcome::class_complete && session.results().size() == 4 &&
                session.total_centiseconds() == 4 * time + 6 && progress.cleared(league), "Class result mismatch");
            rejects([&] { session.advance(); }, "Completed class advanced a fifth round");
        } else {
            check(outcome == (i == 1 ? ChampionshipOutcome::bonus : ChampionshipOutcome::next_race),
                "Missing bonus or next race result");
            session.advance();
            check(session.carry_seconds() == 99 && session.credits() == 2, "Carry cap or bonus skip incorrect");
        }
    }
}

void race_three_retries() {
    ChampionshipProgress progress;
    constexpr GridOrder entry{2,4,1,3}, terminal{3,1,2,4}, at_retirement{1,3,4,2};
    for(const auto failure:{RaceFailure::none,RaceFailure::time_out,RaceFailure::wrecked,RaceFailure::out_of_bounds}) {
        Championship run(SpeedClass::expert,PlayerCar::coupe);
        run.finish({run.course(),{},1,12,10000},progress);
        run.advance();
        run.finish({run.course(),{},3,26,11000,false,entry},progress);
        run.finish_bonus(3,1);
        check(run.starting_grid()==entry,"Race three lost the qualifying order");
        run.finish({run.course(),failure,4,0,12000,false,terminal},progress);
        run.advance();
        const auto& saved=failure==RaceFailure::time_out?terminal:entry;
        check(run.starting_grid()==saved,"Wrong saved lineup after race-three failure");
        run.finish({run.course(),{},2,0,600,true,at_retirement},progress);
        run.advance();
        check(run.starting_grid()==saved && run.credits()==1 && run.carry_seconds()==29,
            "Menu retirement overwrote the last saved lineup or changed the race clock");
    }
}

void race_four_handoff() {
    constexpr GridOrder previous{1,4,3,2}, qualified{4,2,1,3}, expired{3,1,2,4};
    constexpr std::uint16_t remaining[]{125,95,84};
    for(const auto league:{SpeedClass::novice,SpeedClass::expert,SpeedClass::master}) {
        ChampionshipProgress progress;
        Championship run(league,PlayerCar::four_wd);
        run.finish({run.course(),{},1,12,10000},progress);
        run.advance();
        run.finish({run.course(),{},2,30,11000,false,previous},progress);
        run.finish_bonus(7,1);
        const auto seconds=remaining[unsigned(league)];
        check(run.finish({run.course(),{},3,seconds,12000,false,qualified},progress)==ChampionshipOutcome::next_race,
              "Third qualifying result did not offer race four");
        run.advance();
        const auto bank=std::min<std::uint16_t>(99,seconds);
        const auto check_entry=[&](const GridOrder& order,unsigned spares) {
            check(run.round()==4 && run.starting_grid()==order && run.carry_seconds()==bank &&
                  run.credits()==spares && run.total_centiseconds()==33000 && run.results().size()==3,
                  "Race-four places, bank, spare count or qualifying results changed");
        };
        check_entry(qualified,3);
        const auto retry=[&](RoundResult result) {
            check(run.finish(result,progress)==ChampionshipOutcome::retry,"Missing fourth-race retry");
            run.advance();
        };
        retry({run.course(),RaceFailure::wrecked,4,0,4000,false,expired});
        check_entry(qualified,2);
        retry({run.course(),RaceFailure::time_out,4,0,4000,false,expired});
        check_entry(expired,1);
        retry({run.course(),{},2,45,4000,true,previous});
        check_entry(expired,0);
        check(run.finish({run.course(),{},4,0,5000,false,previous},progress)==ChampionshipOutcome::game_over &&
              !progress.cleared(league),"Rank Out with zero spares awarded a class clear or retry");
    }
}

void qualification() {
    ChampionshipProgress progress;
    Championship run(SpeedClass::novice, PlayerCar::four_wd);
    check(progress.available(SpeedClass::novice) && progress.available(SpeedClass::expert) &&
        !progress.available(SpeedClass::master), "Wrong fresh class selection");
    check(run.round() == 1 && run.credits() == 2 && !run.carry_seconds(), "Wrong fresh grid");
    check(!progress.best_split(SpeedClass::novice, 1), "Erased SRAM must show a blank benchmark");
    rejects([&] { run.finish({CourseId::night_owl, RaceFailure::none, 2, 3, 300}, progress); },
        "Accepted result for different course");
    rejects([&] { run.finish({run.course(), RaceFailure::none, 0, 2, 300}, progress); }, "Accepted missing rank");
    run.finish({run.course(), RaceFailure::none, 3, 23, 10100}, progress);
    rejects([&] { run.finish({run.course(), RaceFailure::none, 3, 23, 10100}, progress); },
        "Counted the same round twice");
    run.advance();
    check(run.course() == CourseId::aqua_tunnel && run.carry_seconds() == 23, "Incorrect first carry");
    check(run.finish({run.course(), RaceFailure::none, 4, 5, 12312}, progress) == ChampionshipOutcome::retry,
        "Fourth place must rank out");
    run.advance();
    check(run.credits() == 1 && run.round() == 2 && run.carry_seconds() == 23 && run.total_centiseconds() == 10100,
        "Retry changed entry time, course or cumulative result");
    check(run.finish({run.course(), RaceFailure::time_out}, progress) == ChampionshipOutcome::retry, "Missing last retry");
    run.advance();
    check(run.credits() == 0, "Wrong last spare");
    check(run.finish({run.course(), RaceFailure::out_of_bounds}, progress) == ChampionshipOutcome::game_over,
        "Out of cars must end class");
    rejects([&] { run.advance(); }, "Infinite retries");
    check(!progress.cleared(SpeedClass::novice), "Unfinished class was saved");

    Championship bonus(SpeedClass::expert, PlayerCar::coupe);
    bonus.finish({bonus.course(), RaceFailure::none, 1, 12, 10000}, progress);
    bonus.advance();
    bonus.finish({bonus.course(), RaceFailure::none, 2, 26, 11000}, progress);
    bonus.finish_bonus(13, 2);
    check(bonus.course() == CourseId::white_land && bonus.credits() == 4 && bonus.carry_seconds() == 39 &&
        bonus.total_centiseconds() == 21000, "Bonus rewards changed total time or went to wrong grid");
    rejects([&] { bonus.finish_bonus(13, 2); }, "Duplicate trailer awards");
    check(bonus.finish({bonus.course(), RaceFailure::none, 0, 0, 0, true}, progress) == ChampionshipOutcome::retry,
        "Voluntary retirement must cost a car");
    bonus.advance();
    check(bonus.credits() == 3 && bonus.carry_seconds() == 39, "Retiring dropped trailer award");
}

void course_records(const std::filesystem::path& file) {
    {
        // Old files only stored the best three completed classes.
        std::ofstream old(file);
        old << "SPEED-TRAX 1 2\n0 1 \"AAA\" 9000 9100 9200 9300\n"
               "0 0 \"BBB\" 8900 9600 9700 9800\nEND\n";
    }
    auto fast = ChampionshipProgress::load(file);
    auto slow = ChampionshipProgress::load(file);
    check(fast.course_best(CourseId::easy_ride) == 8900 && fast.course_best(CourseId::aqua_tunnel) == 9100,
          "Legacy individual course bests were lost");
    Championship first(SpeedClass::expert, PlayerCar::four_wd);
    first.finish({first.course(), RaceFailure::none, 1, 10, 8000}, fast);
    fast.save(file);
    Championship later(SpeedClass::expert, PlayerCar::coupe);
    later.finish({later.course(), RaceFailure::none, 2, 5, 8200}, slow);
    slow.save(file);
    auto reopened = ChampionshipProgress::load(file);
    check(reopened.course_best(first.course()) == 8000 && !reopened.cleared(SpeedClass::expert),
          "Independent slower run replaced a best or unlocked a class");
    first.advance();
    first.finish({first.course(), RaceFailure::time_out, 0, 0, 2500}, reopened);
    check(!reopened.course_best(first.course()), "Incomplete run set a best");
    first.advance();
    first.finish({first.course(), RaceFailure::none, 4, 1, 9999}, reopened);
    reopened.save(file);
    check(ChampionshipProgress::load(file).course_best(first.course()) == 9999,
          "Completed race lost its course time on rank out");
    std::filesystem::remove(file);
}

void checkpoints(const std::filesystem::path& file) {
    ChampionshipProgress progress;
    Championship session(SpeedClass::expert, PlayerCar::f_type, "DRV");
    const GridOrder grid{3,2,4,1};
    const auto finish = [&](unsigned time) {
        session.finish({session.course(), RaceFailure::none, 2, 38, time, false, grid}, progress);
        progress.set_checkpoint(session);
        progress.save(file);
        auto reopened = ChampionshipProgress::load(file);
        check(reopened.checkpoint().has_value(), "Missing class after reopening save");
        return *reopened.checkpoint();
    };
    session = finish(8200);
    check(session.round()==2 && session.car()==PlayerCar::f_type && session.initials()=="DRV" &&
          session.credits()==2 && session.carry_seconds()==38 && session.total_centiseconds()==8200,
          "Wrong grid after resuming round two");
    session = finish(8100);
    check(session.outcome()==ChampionshipOutcome::bonus && session.total_centiseconds()==16300,
          "Quit on results lost pending bonus");
    session.finish_bonus(7,1);
    progress.set_checkpoint(session);
    progress.save(file);
    session = *ChampionshipProgress::load(file).checkpoint();
    check(session.round()==3 && session.carry_seconds()==45 && session.credits()==3 &&
          session.starting_grid()==grid, "Bonus rewards changed across restart");
    session.finish({session.course(), RaceFailure::time_out, 4, 0, 5000, false, {2,1,4,3}}, progress);
    session.advance();
    progress.set_checkpoint(session);
    progress.save(file);
    session = *ChampionshipProgress::load(file).checkpoint();
    check(session.credits()==2 && session.starting_grid()==GridOrder{2,1,4,3}, "Retry regained a spare or lost grid");
    session = finish(8000);
    check(session.round()==4 && session.results().size()==3, "Lost final round");
    auto stale=ChampionshipProgress::load(file);
    progress.set_checkpoint(std::nullopt);
    progress.save(file);
    stale.set_checkpoint(session);
    stale.save(file);
    check(!ChampionshipProgress::load(file).checkpoint(), "Stale writer resurrected a completed class");
    std::filesystem::remove(file);
}

void records(const std::filesystem::path& file) {
    auto first = ChampionshipProgress::load(file);
    clear_class(first, SpeedClass::novice, PlayerCar::coupe, "A B", 14001);
    check(!first.available(SpeedClass::master), "Novice unlocked Master");
    auto second = ChampionshipProgress::load(file);
    clear_class(second, SpeedClass::expert, PlayerCar::f_type, "FTY", 9500);
    check(second.available(SpeedClass::master) && !second.cleared(SpeedClass::novice), "Expert alone must unlock Master");
    auto save_one = std::async(std::launch::async, [&] { first.save(file); });
    auto save_two = std::async(std::launch::async, [&] { second.save(file); });
    save_one.get();
    save_two.get();
    auto loaded = ChampionshipProgress::load(file);
    check(loaded.cleared(SpeedClass::novice) && loaded.available(SpeedClass::master) &&
        loaded.records(SpeedClass::novice).front().initials == "A B", "Concurrent independent records lost");
    clear_class(loaded, SpeedClass::master, PlayerCar::four_wd, "4WD", 12000);
    Championship faster(SpeedClass::master, PlayerCar::f_type, "FTY");
    for (unsigned round = 1; round <= 4; ++round) {
        faster.finish({faster.course(), RaceFailure::none, 1, 19, 9500}, loaded);
        check(faster.previous_best() == round * 12000 + round * (round - 1) / 2,
            "Result lost previous class leader split when setting a record");
        if (round < 4) faster.advance();
    }
    for (const auto time : {14003, 14002, 15000, 13588})
        clear_class(loaded, SpeedClass::novice, PlayerCar::four_wd, "WIN", time);
    loaded.save(file);
    first.save(file); // The older session cannot overwrite a faster time or a Master clear.
    loaded = ChampionshipProgress::load(file);
    const auto times = loaded.records(SpeedClass::novice);
    check(times.size() == 3 && times[0].course_times[0] == 13588 && times[1].course_times[0] == 14001 &&
        times[2].course_times[0] == 14002 && loaded.cleared(SpeedClass::master), "Leaderboard merge/order mismatch");
    auto same_time = loaded;
    clear_class(loaded, SpeedClass::expert, PlayerCar::f_type, "FTY", 9500);
    clear_class(same_time, SpeedClass::expert, PlayerCar::f_type, "FTY", 9500);
    loaded.save(file);
    loaded.save(file); // Saving twice adds this attempt only once.
    check(loaded.records(SpeedClass::expert).size() == 2, "Save duplicated or erased a repeat performance");
    same_time.save(file);
    check(same_time.records(SpeedClass::expert).size() == 3, "Equal finish in independent session disappeared");
    {
        std::ofstream damaged(file);
        damaged << "SPEED-TRAX 1 1\n";
    }
    rejects([&] { loaded.save(file); }, "Overwrote a damaged progress file");
    rejects([&] { ChampionshipProgress::load(file); }, "Read incomplete progress");
}
}

int main() {
    try {
        qualification();
        race_three_retries();
        race_four_handoff();
        const auto dir = std::filesystem::temp_directory_path() /
            ("srf speed trax " + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directory(dir);
        const auto file = dir / "championship.sav";
        course_records(file);
        checkpoints(file);
        records(file);
        std::filesystem::remove(file);
        std::filesystem::remove(dir / "championship.sav.lock");
        std::filesystem::remove(dir);
        std::cout << "Speed Trax classes, finishes, rank out, retries, bonus rewards and durable records passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
