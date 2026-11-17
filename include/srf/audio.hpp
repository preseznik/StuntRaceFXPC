#pragma once
#include "srf/rom.hpp"
#include "srf/course.hpp"
#include "srf/stunt_course.hpp"
#include <array>
#include <memory>

namespace srf {
struct RaceAudioOutput;
struct SoundSample {
    std::uint8_t directory_index{};
    std::uint16_t original_start{};
    // UINT32_MAX means no loop. BRR history persists when a voice loops.
    std::uint32_t loop_byte_offset = UINT32_MAX;
    std::vector<std::uint8_t> brr;
};
struct Instrument {
    std::uint8_t sample{}, adsr1{}, adsr2{}, gain{};
    std::uint16_t pitch_multiplier{};
};
struct EngineProfile {
    std::uint8_t volume_divisor{}, volume_base{}, pitch_base{}, pitch_slope{};
};
struct MusicTrack {
    std::uint16_t base{};
    std::vector<std::uint8_t> bytes;
};
struct SoundBank {
    std::vector<SoundSample> samples;
    std::vector<Instrument> instruments=std::vector<Instrument>(27);
    std::array<std::uint8_t,8> note_gates{};
    std::array<std::uint8_t,16> note_volumes{};
    std::array<std::uint16_t,13> pitch_table{};
    std::array<std::uint8_t,22> pan_table{};
    std::array<std::array<std::int8_t,8>,4> echo_filters{};
    std::array<std::uint8_t,152> engine_curve{};
    std::array<EngineProfile,2> engine_profiles{}; // Main4WD, secondary engine.
    std::array<std::uint16_t,12> effect_pitch_table{};
    std::array<std::uint16_t,122> effect_pointers{};
    std::vector<std::uint8_t> effects; // BCA0..C734, only sound-sequence data.
    std::array<std::uint8_t,252> effect_noise_seed{};
    // Native sequence data only. No sound CPU program is included.
    std::uint16_t music_base = 0xEE40;
    std::vector<std::uint8_t> music;
};
struct BrrHistory { std::int16_t last{}, previous{}; };
std::array<std::int16_t,16> decode_brr_block(std::span<const std::uint8_t,9> block,
                                           BrrHistory& history);
SoundBank extract_easy_ride_audio(const Rom& rom);
SoundBank extract_speed_bonus_audio(const Rom& rom);
SoundBank extract_stunt_audio(const Rom& rom,StuntCourse course);
MusicTrack extract_trailer_loading_music(const Rom& rom);
MusicTrack extract_bonus_tally_music(const Rom& rom);
MusicTrack extract_bonus_tally_outro(const Rom& rom);
SoundBank extract_course_map_audio(const Rom& rom);
SoundBank extract_title_audio(const Rom& rom);
SoundBank extract_course_audio(const Rom& rom, CourseId course);
std::vector<std::uint8_t> encode_audio(const SoundBank& bank);
SoundBank decode_audio(std::span<const std::uint8_t> bytes);
void generate_audio_assets(const Rom& rom, const std::filesystem::path& directory,
                           CourseId course = CourseId::easy_ride);

struct MusicEvent {
    std::uint32_t tick{};
    std::uint8_t channel{}, command{};
    std::array<std::uint8_t,3> parameters{};
    std::uint8_t parameter_count{};
};
struct MusicChannel {
    std::uint16_t cursor{}, repeat_start{}, repeat_return{};
    std::uint8_t remaining=1, duration=0, gate=0, volume=0;
    std::uint8_t repeat_count=0, instrument=0, transpose=0;
    std::uint8_t slide_remaining=0, slide_delay=0, envelope_length=0, envelope_delay=0;
};
struct MusicArticulation { std::uint8_t duration{}, gate{}, volume{}; };
struct EffectChannelState {
    std::uint16_t cursor{}, pitch{};
    std::uint8_t id{}, remaining{}, left{}, right{}, sample{}, adsr1{}, adsr2{};
};
struct EchoState {
    unsigned mask{}, amount{}, stage{};
    int feedback{}, left{}, right{};
    bool operator==(const EchoState&) const = default;
};
// Native data sequencer. Events retain effects instead of silently discarding
// them. A complete audio renderer must implement those effects and SFX mixing.
class MusicSequence {
public:
    explicit MusicSequence(const SoundBank& bank, const std::array<MusicArticulation,8>& inherited = {});
    std::vector<MusicEvent> tick();
    void restart();
    // An empty selection restores the bank's course music.
    void select_track(MusicTrack track);
    const std::array<MusicChannel,8>& channels() const { return channels_; }
    bool playing() const { return playing_; }
    std::uint32_t ticks() const { return ticks_; }
private:
    std::uint8_t read(std::uint16_t& cursor) const;
    std::uint16_t word(std::uint16_t& cursor) const;
    bool next_section();
    const SoundBank* bank_;
    MusicTrack alternate_;
    std::array<MusicChannel,8> channels_{};
    std::uint16_t section_cursor_{};
    std::uint8_t section_repeat_=0;
    std::uint32_t ticks_=0;
    bool playing_=true;
};
// A native sample player for the recovered music data. No sound CPU, register
// interpreter, or console memory is present. Output is stereo signed16 at32kHz.
class MusicRenderer {
public:
    explicit MusicRenderer(const SoundBank&);
    ~MusicRenderer();
    MusicRenderer(const MusicRenderer&)=delete;
    MusicRenderer& operator=(const MusicRenderer&)=delete;
    void reset();
    void set_engine(std::uint8_t port, bool throttle, bool boost, bool enabled=true);
    void play_effect(std::uint8_t id);
    void play_race_effect(std::uint8_t port_code);
    void sound_command(std::uint8_t command);
    void update_race(const RaceAudioOutput&, bool throttle, bool boost,
                     bool engine_enabled, bool music_enabled);
    void set_music_enabled(bool enabled);
    void play_music(MusicTrack track = {}, bool fade_in_engine = false, MusicTrack following = {});
    std::array<EffectChannelState,8> effect_states() const;
    EchoState echo_state() const;
    void render(std::span<std::int16_t> interleaved);
    std::uint32_t ticks() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
