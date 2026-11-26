#pragma once

#include "srf/native_race.hpp"
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace srf {
enum class SpeedClass : std::uint8_t { novice, expert, master };
enum class ChampionshipOutcome { racing, next_race, bonus, retry, game_over, class_complete };
std::string_view class_name(SpeedClass league);

struct RoundResult {
    CourseId course{};
    RaceFailure failure{};
    std::uint16_t place{}; // One-based position at the player's finish.
    std::uint16_t remaining_seconds{};
    std::uint32_t elapsed_centiseconds{};
    bool retired{};
    GridOrder finishing_places{}; // One-based place for each actor in NativeRace::racers() order.
};
RoundResult championship_result(const NativeRace& race, bool retired = false);

struct ChampionshipRecord {
    PlayerCar car{};
    std::array<std::uint32_t, 4> course_times{}; // Centiseconds, in race order.
    std::string initials{"AAA"};
    std::uint32_t total() const;
    bool operator==(const ChampionshipRecord&) const = default;
};

class ChampionshipProgress;

class Championship {
public:
    Championship(SpeedClass league, PlayerCar car, std::string initials = "AAA");
    CourseId course() const;
    SpeedClass league() const { return league_; }
    PlayerCar car() const { return car_; }
    std::string_view initials() const { return initials_; }
    unsigned round() const { return round_index_ + 1; }
    unsigned credits() const { return spares_; } // Extra cars, excludes the car on the grid.
    std::uint16_t carry_seconds() const { return carry_seconds_; }
    std::optional<GridOrder> starting_grid() const;
    ChampionshipOutcome outcome() const { return outcome_; }
    std::optional<std::uint32_t> previous_best() const { return previous_best_; }
    std::span<const RoundResult> results() const { return {qualified_.data(), completed_}; }
    std::uint32_t total_centiseconds() const;
    ChampionshipOutcome finish(const RoundResult& race, ChampionshipProgress& progress);
    // Accept a result. At the bonus choice this takes the original SKIP branch.
    void advance();
    void finish_bonus(unsigned extra_seconds, unsigned extra_cars);

private:
    friend class ChampionshipProgress;
    SpeedClass league_;
    PlayerCar car_;
    std::string initials_;
    unsigned round_index_{}, completed_{};
    std::uint8_t spares_{2};
    std::uint16_t carry_seconds_{};
    std::optional<GridOrder> saved_grid_;
    ChampionshipOutcome outcome_{ChampionshipOutcome::racing};
    std::optional<std::uint32_t> previous_best_;
    std::array<RoundResult, 4> qualified_{};
};

class ChampionshipProgress {
public:
    bool available(SpeedClass league) const;
    bool cleared(SpeedClass league) const;
    std::span<const ChampionshipRecord> records(SpeedClass league) const;
    std::optional<std::uint32_t> best_split(SpeedClass league, unsigned rounds) const;
    std::optional<std::uint32_t> course_best(CourseId course) const;
    const std::optional<Championship>& checkpoint() const { return checkpoint_; }
    void set_checkpoint(std::optional<Championship> session);
    static ChampionshipProgress load(const std::filesystem::path& file);
    // Merge independent game sessions before atomically publishing the new records.
    void save(const std::filesystem::path& file);

private:
    friend class Championship;
    std::optional<Championship> checkpoint_;
    std::uint64_t checkpoint_revision_{};
    bool checkpoint_changed_{};
    void record_course(CourseId course, std::uint32_t time);
    std::array<std::uint32_t, courses.size()> course_bests_{};
    void record(SpeedClass league, const ChampionshipRecord& record);
    std::array<std::vector<ChampionshipRecord>, 3> records_{}, pending_{};
};
}
