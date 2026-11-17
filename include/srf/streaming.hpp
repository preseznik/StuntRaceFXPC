#pragma once

#include "srf/scene.hpp"
#include <array>
#include <span>

namespace srf {
struct ActivationMask {
    std::uint8_t width = 0, height = 0, origin_x = 0, origin_z = 0;
    std::vector<std::uint8_t> cells;
};
struct ActivationCell {
    std::uint16_t kind = 0, offset = 0;
    bool operator==(const ActivationCell&) const = default;
};
struct StreamObject {
    std::uint32_t command_offset = 0;
    std::uint16_t cell_offset = 0;
    std::uint8_t activation_flags = 0; // Original CPU object +2024.
};
struct StreamingCourse {
    ActivationMask normal, compact;
    std::vector<StreamObject> objects; // Original CPU creation/list order.
};
struct StreamObjectState {
    std::uint8_t timer = 0; // Original CPU +2036.
    bool initialized = false; // CD2D has installed CD7E/CD55 callbacks.
    bool attached = false; // CPU +2022 points to an allocated GSU object.
    bool keepalive = false; // GSU object +1C, bit 4.
};
// CPU08:CAF6 / CB15. Streaming may suspend this clock between active calls.
struct CollisionAnimation {
    std::uint8_t frames, period, hold;
    std::uint8_t current{}, counter{}, pause{}, visible{};
    bool reversed{};
    void reset();
    void advance();
};
struct CollisionStreamState {
    std::vector<StreamObjectState> objects;
    // Projection of GSU 2332/+4 onto these static collision objects.
    // Vehicles and other callbacks keep their own relative positions elsewhere.
    std::vector<std::uint32_t> active_command_offsets;
};

StreamingCourse extract_easy_ride_streaming(const Rom& rom);
StreamingCourse extract_streaming(const Rom& rom, const Scene& scene);
// Original CPU08:83F8, signed 16-bit world coordinates and 2048-unit cells.
std::uint16_t original_spatial_cell(std::int16_t x, std::int16_t z);
// Original ROM02:A2B4 (relocated to7E:C58D), preserving clipping and row order.
std::vector<ActivationCell> activation_cells(const ActivationMask& mask,
                                            std::int16_t x, std::int16_t z);
CollisionStreamState reset_collision_stream(const StreamingCourse& course);
// CPU02:CE08: call at the original activation-refresh phase, once per update.
void refresh_collision_stream(const StreamingCourse& course, CollisionStreamState& state,
                              std::int16_t x, std::int16_t z, bool compact = false);
// CPU03:D388 with original static callbacks08:CD2D/CD55/CD7E/D070.
// New allocations prepend to GSU2332. Does not perform the later GSU cleanup.
void update_collision_stream(const StreamingCourse& course, CollisionStreamState& state);
// GSU04:903D..9068, after collision/vehicle processing for that GSU iteration.
void finish_collision_stream(const StreamingCourse& course, CollisionStreamState& state);
}
