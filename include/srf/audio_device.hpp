#pragma once
#include "srf/audio.hpp"
#include "srf/race_audio.hpp"
#include <string>

namespace srf {
class AudioDevice {
public:
    explicit AudioDevice(SoundBank);
    ~AudioDevice();
    AudioDevice(const AudioDevice&)=delete;
    AudioDevice& operator=(const AudioDevice&)=delete;
    void pause(bool paused);
    void restart();
    void play_race_effect(std::uint8_t port_code);
    void play_music(MusicTrack track = {}, bool fade_in_engine = false, MusicTrack following = {});
    void update(const RaceAudioOutput&, bool throttle, bool boost,
                bool engine_enabled, bool music_enabled);
    std::string error() const;
    std::uint64_t frames_rendered() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
