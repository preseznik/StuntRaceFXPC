#include "srf/audio_device.hpp"
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <stdexcept>

namespace srf {
struct AudioDevice::Impl {
    SoundBank bank;
    MusicRenderer renderer;
    SDL_AudioStream* stream=nullptr;
    mutable std::mutex mutex;
    std::string failure;
    std::uint64_t frames=0;
    explicit Impl(SoundBank input):bank(std::move(input)),renderer(bank) {
        renderer.set_music_enabled(false);
        if(!SDL_InitSubSystem(SDL_INIT_AUDIO))throw std::runtime_error(SDL_GetError());
        const SDL_AudioSpec spec{SDL_AUDIO_S16,2,32000};
        stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,callback,this);
        if(!stream) {const std::string why=SDL_GetError();SDL_QuitSubSystem(SDL_INIT_AUDIO);throw std::runtime_error(why);}
        if(!SDL_ResumeAudioStreamDevice(stream)) {
            const std::string why=SDL_GetError();SDL_DestroyAudioStream(stream);SDL_QuitSubSystem(SDL_INIT_AUDIO);
            throw std::runtime_error(why);
        }
    }
    ~Impl() {SDL_DestroyAudioStream(stream);SDL_QuitSubSystem(SDL_INIT_AUDIO);}
    static void SDLCALL callback(void* data,SDL_AudioStream* stream,int additional,int) noexcept {
        auto& self=*static_cast<Impl*>(data);
        std::lock_guard lock(self.mutex);
        try {
            std::array<std::int16_t,2048> samples{};
            while(additional>0) {
                const auto count=std::min<unsigned>(samples.size(),(unsigned(additional)+3)/4*2);
                auto block=std::span(samples).first(count);
                if(self.failure.empty()) {self.renderer.render(block);self.frames+=block.size()/2;}
                else std::fill(block.begin(),block.end(),0);
                if(!SDL_PutAudioStreamData(stream,block.data(),int(block.size_bytes()))) {
                    self.failure=SDL_GetError();return;
                }
                additional-=int(block.size_bytes());
            }
        } catch(const std::exception& e) {self.failure=e.what();}
        catch(...) {self.failure="Unknown native audio failure";}
    }
};
AudioDevice::AudioDevice(SoundBank bank):impl_(std::make_unique<Impl>(std::move(bank))) {}
AudioDevice::~AudioDevice()=default;
void AudioDevice::pause(bool paused) {
    if(!(paused?SDL_PauseAudioStreamDevice(impl_->stream):SDL_ResumeAudioStreamDevice(impl_->stream)))
        throw std::runtime_error(SDL_GetError());
}
void AudioDevice::restart() {
    // SDL owns the callback lock. Acquire it before our state mutex so a reset
    // cannot deadlock a callback which is submitting its next audio block.
    if(!SDL_LockAudioStream(impl_->stream))throw std::runtime_error(SDL_GetError());
    try {
        std::lock_guard lock(impl_->mutex);impl_->renderer.reset();impl_->renderer.set_music_enabled(false);impl_->failure.clear();
        if(!SDL_ClearAudioStream(impl_->stream))throw std::runtime_error(SDL_GetError());
    } catch(...) {SDL_UnlockAudioStream(impl_->stream);throw;}
    SDL_UnlockAudioStream(impl_->stream);
}
void AudioDevice::update(const RaceAudioOutput& state,bool throttle,bool boost,bool engine_enabled,bool music_enabled) {
    std::lock_guard lock(impl_->mutex);
    impl_->renderer.update_race(state,throttle,boost,engine_enabled,music_enabled);
}
void AudioDevice::play_music(MusicTrack track,bool fade_in_engine,MusicTrack following) {
    std::lock_guard lock(impl_->mutex);
    impl_->renderer.play_music(std::move(track),fade_in_engine,std::move(following));
}
void AudioDevice::play_race_effect(std::uint8_t port_code) {
    std::lock_guard lock(impl_->mutex);
    impl_->renderer.play_race_effect(port_code);
}
std::string AudioDevice::error() const {std::lock_guard lock(impl_->mutex);return impl_->failure;}
std::uint64_t AudioDevice::frames_rendered() const {std::lock_guard lock(impl_->mutex);return impl_->frames;}
}
