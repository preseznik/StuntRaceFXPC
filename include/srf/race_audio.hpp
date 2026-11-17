#pragma once
#include "srf/vehicle.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace srf {
struct RaceAudioTuning {
    std::array<std::uint8_t,150> speed_curve{};
};
struct RaceAudioState {
    std::uint16_t rev{};        // Original CPU117E, initialized by03:E892.
    std::uint16_t loop_flags{}; // Original CPU1A5E, initialized by08:AF84.
    std::uint8_t surface_command{}; // Original CPU0E4F.
};
struct RaceAudioInput {
    bool prestart{}; // CPU0D53, including the original countdown.
    bool throttle{}; // Raw SNES B; independent of countdown's physics input gate.
    std::uint8_t race_mode{}; // CPU0DD6; >=6 bypasses prestart rev/smoothing.
    std::uint16_t cpu_flags{}; // Original player CPU2024/2025, not body40.
    std::uint16_t terrain_flags{}; // CPU1AEC; bit20 selects special wheel effects.
    std::uint16_t counter{}; // CPU05E9 for special terrain's original cadence.
};
struct RaceAudioOutput {
    std::uint8_t engine_parameter{}; // CPU0E49, latched to APU2143 at NMI.
    // These are separate original CPU queues. Effects are raw04:D656 codes,
    // not SPC table indices: the SPC handler maps a CPU code to code-5.
    // Commands belong to04:D69F and must not use that effect conversion.
    std::vector<std::uint8_t> effects;
    std::vector<std::uint8_t> commands;
};
// Apply to already-native replay words too: original08:C803 requires reserve.
std::uint16_t filter_player_boost(const VehicleState&,std::uint16_t controls);
// Keyboard/UI wishes use the existing low control bits. Brake40 and reverse20
// both represent the original A brake/reverse button. Apply before physics,
// preserving the requested throttle/boost separately for presentation/audio.
// CPU08:C760..C879 also emits the empty-boost warning through audio flag10.
std::uint16_t prepare_player_input(RaceAudioState&,const VehicleState& before,
                                  std::uint16_t requested,bool prestart,
                                  RaceAudioOutput&);
RaceAudioTuning extract_race_audio_tuning(std::span<const std::uint8_t> rom,
                                        std::uint16_t player_definition=0x8411);
std::uint8_t update_engine_audio(RaceAudioState&, const RaceAudioTuning&,
                                const VehicleState&, const RaceAudioInput&);
void update_player_audio_events(RaceAudioState&, const Vehicle&,
                               const RaceAudioInput&, RaceAudioOutput&);
// Invoke once after an original physics phase and CPU countdown update, never
// once per rendered frame or audio sample. No vehicle state is modified.
RaceAudioOutput update_race_audio(RaceAudioState&, const RaceAudioTuning&,
                                 const Vehicle&, const RaceAudioInput&);
// NativeRace already owns the original countdown timer; only report its edges.
// Stage3/4/5 beep49; stage6 GO4A. Repeated presentation never retriggers them.
void append_countdown_audio(std::uint8_t previous_stage, std::uint8_t stage,
                            RaceAudioOutput&);
}
