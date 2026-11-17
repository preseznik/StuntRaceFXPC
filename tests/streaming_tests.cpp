#include "srf/streaming.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void unit_tests() {
    require(srf::original_spatial_cell(-32768, -32768) == 0, "Grid minimum");
    require(srf::original_spatial_cell(32767, 32767) == 2046, "Grid maximum");
    require(srf::original_spatial_cell(-1, -1) == 990, "Negative floor division");
    require(srf::original_spatial_cell(0, 0) == 1056, "Grid origin");
    const srf::ActivationMask mask{3, 3, 1, 1, std::vector<std::uint8_t>(9, 0x23)};
    const auto clipped = srf::activation_cells(mask, -32768, -32768);
    require(clipped == std::vector<srf::ActivationCell>{{0x23, 64}, {0x23, 66}, {0x23, 0}, {0x23, 2}},
            "Row order and edge clipping");
    srf::StreamingCourse course{mask, mask, {{10, 1056, 0}, {20, 1056, 0}, {30, 1056, 0x10}}};
    auto state = srf::reset_collision_stream(course);
    srf::refresh_collision_stream(course, state, 0, 0);
    srf::update_collision_stream(course, state);
    require(state.active_command_offsets == std::vector<std::uint32_t>{20, 10}, "Prepend creation order and exclusion flag");
    require(state.objects[0].timer == 2 && state.objects[0].keepalive, "First callback activation");
    srf::finish_collision_stream(course, state);
    require(!state.objects[0].keepalive, "GSU clears keepalive");
    srf::update_collision_stream(course, state);
    srf::finish_collision_stream(course, state);
    require(state.active_command_offsets.size() == 2 && state.objects[0].timer == 1, "One unrefreshed update retained");
    srf::update_collision_stream(course, state);
    require(state.active_command_offsets.size() == 2 && !state.objects[0].attached, "CPU expiration precedes GSU cleanup");
    srf::finish_collision_stream(course, state);
    require(state.active_command_offsets.empty(), "Expired objects removed");
    srf::refresh_collision_stream(course, state, 0, 0);
    srf::update_collision_stream(course, state);
    require(state.active_command_offsets.empty() && state.objects[0].timer == 255, "Reactivation callback delay");
    srf::refresh_collision_stream(course, state, 0, 0);
    require(state.objects[0].timer == 255, "Refresh preserves pending initialization");
    srf::update_collision_stream(course, state);
    require(state.active_command_offsets == std::vector<std::uint32_t>{20, 10}, "Reactivation order");
    auto invalid = state;
    invalid.objects.pop_back();
    bool rejected = false;
    try { srf::update_collision_stream(course, invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Mismatched state rejected");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        unit_tests();
        std::size_t trace_checks = 0;
        if (argc > 1) {
            const auto rom = srf::Rom::load(argv[1]);
            const auto course = srf::extract_easy_ride_streaming(rom);
            require(course.objects.size() == 35, "Original static collision object count");
            require(srf::activation_cells(course.normal, -19968, 1472).size() == 69, "Original starting activation cells");
            auto state = srf::reset_collision_stream(course);
            srf::refresh_collision_stream(course, state, -19968, 1472);
            srf::update_collision_stream(course, state);
            require(state.active_command_offsets == std::vector<std::uint32_t>{0x288a0, 0x2885e, 0x286e0, 0x286b2, 0x28196, 0x28128},
                    "Original starting-grid active order");
            if (argc > 2) {
                std::ifstream file{std::filesystem::path(argv[2])};
                if (!file) throw std::runtime_error("Trace fixture missing");
                state = srf::reset_collision_stream(course);
                std::string line;
                while (std::getline(file, line)) {
                    std::istringstream row(line);
                    int x, z;
                    std::size_t count;
                    if (!(row >> x >> z >> count) || count > 35) throw std::runtime_error("Invalid trace row");
                    std::vector<std::uint32_t> expected(count);
                    for (auto& id : expected) if (!(row >> id)) throw std::runtime_error("Truncated trace row");
                    srf::refresh_collision_stream(course, state, static_cast<std::int16_t>(x), static_cast<std::int16_t>(z));
                    srf::update_collision_stream(course, state);
                    srf::finish_collision_stream(course, state);
                    require(state.active_command_offsets == expected, "Original live active order differs");
                    ++trace_checks;
                }
                require(trace_checks > 0, "Empty trace fixture");
            }
        }
        std::cout << "{\"streaming_checks_passed\":true,\"original_order_updates\":" << trace_checks << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
