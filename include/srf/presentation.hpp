#pragma once

#include "srf/world.hpp"

namespace srf {
// Presentation only. Original coordinates and simulation state stay unchanged.
using SurfaceOffset = std::array<float,3>;
std::vector<SurfaceOffset> coplanar_surface_offsets(const WorldModel& model,
    std::size_t frame, const std::array<bool,256>& textured_materials);
std::vector<bool> missing_barrier_backfaces(const WorldModel& model,std::size_t frame);

std::uint8_t interpolated_angle_byte(std::uint16_t before, std::uint16_t after, float blend);
int wheel_sprite_bank(std::uint8_t roll, std::uint8_t relative_yaw, std::uint8_t camera_roll=0);
unsigned original_sprite_diameter(std::uint16_t radius, std::uint8_t scale, std::uint8_t size);
bool road_support_surface(std::uint8_t scene_opcode, std::int16_t descriptor_bias, std::span<const Vertex> vertices,
    const Polygon& face);
float road_painter_depth(float camera_z, float vertical_delta, std::int16_t descriptor_bias, bool background_priority);
}
