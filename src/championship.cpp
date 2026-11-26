#include "srf/championship.hpp"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <system_error>

namespace srf {
namespace {
constexpr unsigned courses_per_class = 4;
constexpr unsigned records_per_class = 3;
constexpr std::array<std::string_view, 3> class_names{"Novice", "Expert", "Master"};
static_assert(courses.size() == class_names.size() * courses_per_class);

bool valid_initials(std::string_view initials) {
    return initials.size() == 3 && std::all_of(initials.begin(), initials.end(),
        [](char c) { return c >= ' ' && c <= '~'; });
}

[[noreturn]] void file_error(const char* action, DWORD code = GetLastError()) {
    throw std::system_error(static_cast<int>(code), std::system_category(), action);
}

struct FileHandle {
    HANDLE value;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    explicit FileHandle(HANDLE handle) : value(handle) {}
};

[[noreturn]] void invalid_save() {
    throw std::runtime_error("Invalid or incomplete Speed Trax progress file");
}

void insert_time(std::vector<ChampionshipRecord>& times, const ChampionshipRecord& entry) {
    // CPU07:B964 checks each place in turn. An equal time can take the next
    // place if that result is slower, including repeat runs by the same driver.
    const auto position = std::upper_bound(times.begin(), times.end(), entry.total(),
        [](unsigned time, const ChampionshipRecord& previous) { return time < previous.total(); });
    if (times.size() == records_per_class && position == times.end()) return;
    times.insert(position, entry);
    if (times.size() > records_per_class) times.pop_back();
}
}

std::string_view class_name(SpeedClass league) {
    return class_names.at(static_cast<unsigned>(league));
}

RoundResult championship_result(const NativeRace& race, bool retired) {
    if (!race.ended() && !retired) throw std::logic_error("Championship race is still running");
    return {race.course_id(), race.failure(), race.standings()[NativeRace::player_index()],
        race.remaining_seconds(), race.elapsed_centiseconds(), retired, race.standings()};
}

std::uint32_t ChampionshipRecord::total() const {
    return std::accumulate(course_times.begin(), course_times.end(), 0U);
}

bool ChampionshipProgress::available(SpeedClass league) const {
    // CPU02:E6B0 admits Master when the Expert clear bit in SRAM $70FC0B is set.
    return league == SpeedClass::novice || league == SpeedClass::expert ||
        (league == SpeedClass::master && cleared(SpeedClass::expert));
}

bool ChampionshipProgress::cleared(SpeedClass league) const {
    return !records(league).empty();
}

std::span<const ChampionshipRecord> ChampionshipProgress::records(SpeedClass league) const {
    return records_.at(static_cast<unsigned>(league));
}

std::optional<std::uint32_t> ChampionshipProgress::best_split(SpeedClass league, unsigned rounds) const {
    if (!rounds || rounds > courses_per_class) throw std::out_of_range("Invalid Speed Trax split");
    const auto times = records(league);
    if (times.empty()) return std::nullopt;
    // CPU07:B727 accumulates each leaderboard row. 07:D471 displays rank one's
    // cumulative split at the current round, even if another row drove that leg faster.
    const auto& splits = times.front().course_times;
    return std::accumulate(splits.begin(), splits.begin() + rounds, 0U);
}

std::optional<std::uint32_t> ChampionshipProgress::course_best(CourseId course) const {
    const auto time = course_bests_.at(static_cast<unsigned>(course));
    return time ? std::optional(time) : std::nullopt;
}

void ChampionshipProgress::record_course(CourseId course, std::uint32_t time) {
    auto& best = course_bests_.at(static_cast<unsigned>(course));
    if (time && (!best || time < best)) best = time;
}

void ChampionshipProgress::record(SpeedClass league, const ChampionshipRecord& entry) {
    const auto index = static_cast<unsigned>(league);
    insert_time(records_.at(index), entry);
    insert_time(pending_.at(index), entry);
}

ChampionshipProgress ChampionshipProgress::load(const std::filesystem::path& file) {
    // Allow an active reader to finish with the old file when the writer replaces it.
    FileHandle input(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        file_error("Opening Speed Trax progress", error);
    }
    std::array<char, 2048> bytes{};
    DWORD length = 0;
    if (!ReadFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &length, nullptr))
        file_error("Reading Speed Trax progress");
    if (length == bytes.size()) invalid_save();
    std::istringstream stream(std::string(bytes.data(), length));
    std::string header;
    unsigned version = 0, count = 0;
    if (!(stream >> header >> version >> count) || header != "SPEED-TRAX" || (version < 1 || version > 3) ||
        count > class_names.size() * records_per_class) invalid_save();
    ChampionshipProgress progress;
    for (unsigned i = 0; i < count; ++i) {
        unsigned league = 0, car = 0;
        ChampionshipRecord entry;
        if (!(stream >> league >> car >> std::quoted(entry.initials)) || league >= class_names.size() ||
            car > static_cast<unsigned>(PlayerCar::f_type) || !valid_initials(entry.initials)) invalid_save();
        entry.car = static_cast<PlayerCar>(car);
        for (auto& time : entry.course_times) {
            if (!(stream >> time) || !time || time > 59999) invalid_save();
        }
        auto& times = progress.records_[league];
        if (times.size() == records_per_class || (!times.empty() && times.back().total() > entry.total()))
            invalid_save();
        for (unsigned round = 0; round < courses_per_class; ++round)
            progress.record_course(courses[league * courses_per_class + round].id, entry.course_times[round]);
        times.push_back(std::move(entry));
    }
    if (version >= 2) {
        if (!(stream >> header) || header != "BESTS") invalid_save();
        for (const auto& course : courses) {
            std::uint32_t time;
            if (!(stream >> time) || time > 59999) invalid_save();
            progress.record_course(course.id, time);
        }
    }
    if (version == 3) {
        unsigned count;
        if (!(stream >> header >> progress.checkpoint_revision_ >> count) || header != "RESUME" || count > 3)
            invalid_save();
        if (count) {
            unsigned league, car, spares, carry, bonus;
            std::string initials;
            if (!(stream >> league >> car >> std::quoted(initials) >> spares >> carry >> bonus) ||
                league >= class_names.size() || car > 2 || !valid_initials(initials) ||
                spares > 255 || carry > 99 || bonus > 1 || (bonus && count != 2)) invalid_save();
            auto& session = progress.checkpoint_.emplace(static_cast<SpeedClass>(league), static_cast<PlayerCar>(car), initials);
            session.completed_ = count;
            session.round_index_ = count - bonus;
            session.spares_ = static_cast<std::uint8_t>(spares);
            session.carry_seconds_ = static_cast<std::uint16_t>(carry);
            session.outcome_ = bonus ? ChampionshipOutcome::bonus : ChampionshipOutcome::racing;
            const auto read_grid = [&] {
                GridOrder grid{};
                for (auto& place : grid) if (!(stream >> place)) invalid_save();
                auto sorted = grid;
                std::sort(sorted.begin(), sorted.end());
                if (sorted != GridOrder{1,2,3,4}) invalid_save();
                return grid;
            };
            for (unsigned i = 0; i < count; ++i) {
                auto& race = session.qualified_[i];
                race.course = courses[league * courses_per_class + i].id;
                if (!(stream >> race.place >> race.remaining_seconds >> race.elapsed_centiseconds) ||
                    race.place < 1 || race.place > 3 || !race.elapsed_centiseconds || race.elapsed_centiseconds > 59999)
                    invalid_save();
                race.finishing_places = read_grid();
            }
            if (session.round_index_ >= 2) session.saved_grid_ = read_grid();
        }
    }
    if (!(stream >> header) || header != "END" || (stream >> header)) invalid_save();
    return progress;
}

void ChampionshipProgress::set_checkpoint(std::optional<Championship> session) {
    if (session && session->outcome() == ChampionshipOutcome::next_race) session->advance();
    if (session && (session->results().empty() ||
        (session->outcome() != ChampionshipOutcome::racing && session->outcome() != ChampionshipOutcome::bonus)))
        throw std::logic_error("A checkpoint must be between championship rounds");
    checkpoint_ = std::move(session);
    checkpoint_changed_ = true;
}

void ChampionshipProgress::save(const std::filesystem::path& file) {
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path());
    auto lock_path = file;
    lock_path += L".lock";
    FileHandle lock(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (lock.value == INVALID_HANDLE_VALUE) file_error("Opening Speed Trax progress lock");
    OVERLAPPED range{};
    if (!LockFileEx(lock.value, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range))
        file_error("Locking Speed Trax progress");

    auto merged = *this;
    if (std::filesystem::exists(file)) {
        merged = load(file);
        if (checkpoint_changed_ && checkpoint_revision_ == merged.checkpoint_revision_) {
            merged.checkpoint_ = checkpoint_;
            ++merged.checkpoint_revision_;
        }
        for (const auto& course : courses) merged.record_course(course.id, course_bests_[static_cast<unsigned>(course.id)]);
        for (unsigned i = 0; i < pending_.size(); ++i)
            for (const auto& entry : pending_[i]) insert_time(merged.records_[i], entry);
    }
    else if (checkpoint_changed_) ++merged.checkpoint_revision_;
    merged.checkpoint_changed_ = false;
    merged.pending_ = {};
    std::size_t count = 0;
    for (const auto& times : merged.records_) count += times.size();
    auto temporary = file;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << "SPEED-TRAX 3 " << count << '\n';
        for (unsigned i = 0; i < merged.records_.size(); ++i) {
            for (const auto& entry : merged.records_[i]) {
                output << i << ' ' << static_cast<unsigned>(entry.car) << ' ' << std::quoted(entry.initials);
                for (const auto time : entry.course_times) output << ' ' << time;
                output << '\n';
            }
        }
        output << "BESTS";
        for (const auto time : merged.course_bests_) output << ' ' << time;
        const auto& checkpoint = merged.checkpoint_;
        output << "\nRESUME " << merged.checkpoint_revision_ << ' ' << (checkpoint ? checkpoint->completed_ : 0);
        if (checkpoint) {
            const auto& session = *checkpoint;
            output << ' ' << unsigned(session.league_) << ' ' << unsigned(session.car_) << ' ' << std::quoted(session.initials_)
                   << ' ' << unsigned(session.spares_) << ' ' << session.carry_seconds_ << ' '
                   << (session.outcome_ == ChampionshipOutcome::bonus);
            for (const auto& race : session.results()) {
                output << '\n' << race.place << ' ' << race.remaining_seconds << ' ' << race.elapsed_centiseconds;
                for (const auto place : race.finishing_places) output << ' ' << place;
            }
            if (session.saved_grid_) for (const auto place : *session.saved_grid_) output << ' ' << place;
        }
        output << "\nEND\n";
        output.close();
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        file_error("Replacing Speed Trax progress");
    *this = std::move(merged);
}

Championship::Championship(SpeedClass league, PlayerCar car, std::string initials)
    : league_(league), car_(car), initials_(std::move(initials)) {
    if (static_cast<unsigned>(league) >= class_names.size() || car > PlayerCar::f_type ||
        !valid_initials(initials_)) throw std::invalid_argument("Invalid Speed Trax selection");
}

CourseId Championship::course() const {
    return courses[static_cast<unsigned>(league_) * courses_per_class + round_index_].id;
}

std::optional<GridOrder> Championship::starting_grid() const {
    return saved_grid_;
}

std::uint32_t Championship::total_centiseconds() const {
    std::uint32_t total = 0;
    for (const auto& race : results()) total += race.elapsed_centiseconds;
    return total;
}

ChampionshipOutcome Championship::finish(const RoundResult& race, ChampionshipProgress& progress) {
    if (outcome_ != ChampionshipOutcome::racing || race.course != course())
        throw std::logic_error("Unexpected championship race result");
    if (race.failure == RaceFailure::none && !race.retired &&
        (race.place < 1 || race.place > 4 || !race.elapsed_centiseconds || race.elapsed_centiseconds > 59999))
        throw std::invalid_argument("Invalid championship finish");

    if (race.failure == RaceFailure::none && !race.retired) progress.record_course(race.course, race.elapsed_centiseconds);
    previous_best_ = progress.best_split(league_, round());
    if (race.retired || race.failure != RaceFailure::none || race.place == 4) {
        outcome_ = spares_ ? ChampionshipOutcome::retry : ChampionshipOutcome::game_over;
        // Time Over sets 0E59 and runs the 08:B863 save. Rank Out, a wreck,
        // leaving the course and retiring from the menu keep the last saved grid.
        if (saved_grid_ && outcome_ == ChampionshipOutcome::retry &&
            !race.retired && race.failure == RaceFailure::time_out)
            saved_grid_ = race.finishing_places;
        return outcome_;
    }
    qualified_[completed_++] = race;
    if (completed_ == courses_per_class) {
        ChampionshipRecord record;
        record.car = car_;
        record.initials = initials_;
        for (unsigned i = 0; i < courses_per_class; ++i)
            record.course_times[i] = qualified_[i].elapsed_centiseconds;
        progress.record(league_, record);
        outcome_ = ChampionshipOutcome::class_complete;
    } else {
        outcome_ = completed_ == 2 ? ChampionshipOutcome::bonus : ChampionshipOutcome::next_race;
    }
    return outcome_;
}

void Championship::advance() {
    switch (outcome_) {
    case ChampionshipOutcome::retry:
        --spares_;
        break;
    case ChampionshipOutcome::bonus:
        finish_bonus(0, 0);
        return;
    case ChampionshipOutcome::next_race:
        carry_seconds_ = std::min<std::uint16_t>(99, qualified_[round_index_].remaining_seconds);
        if (round() == 3) saved_grid_ = qualified_[round_index_].finishing_places;
        ++round_index_;
        break;
    default:
        throw std::logic_error("No championship race to advance");
    }
    outcome_ = ChampionshipOutcome::racing;
}

void Championship::finish_bonus(unsigned extra_seconds, unsigned extra_cars) {
    if (outcome_ != ChampionshipOutcome::bonus) throw std::logic_error("No pending trailer bonus");
    // 08:B863 saved all four ranks when race two ended. Bonus Trax has its own actors.
    saved_grid_ = qualified_[round_index_].finishing_places;
    const unsigned time = qualified_[round_index_].remaining_seconds + std::min(99U, extra_seconds);
    carry_seconds_ = static_cast<std::uint16_t>(std::min(99U, time));
    // CPU07:DE8B increments the eight-bit spare count once per bonus lap.
    spares_ = static_cast<std::uint8_t>(spares_ + extra_cars);
    ++round_index_;
    outcome_ = ChampionshipOutcome::racing;
}
}
