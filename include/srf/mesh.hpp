#pragma once
#include "srf/course.hpp"

#include "srf/rom.hpp"
#include <array>

namespace srf {
struct Vertex {
    std::int32_t x, y, z;
    bool operator==(const Vertex&) const = default;
};
struct Polygon {
    std::vector<std::uint32_t> indices;
    std::array<std::uint8_t, 6> attributes;
};
struct Mesh {
    std::uint32_t rom_offset;
    std::vector<std::vector<Vertex>> frames;
    std::vector<Polygon> polygons;
};

std::vector<Mesh> extract_meshes(const Rom& rom);
std::vector<std::uint8_t> encode_meshes(const std::vector<Mesh>& meshes);
std::vector<Mesh> decode_meshes(std::span<const std::uint8_t> bytes);
void generate_assets(const Rom& rom, const std::filesystem::path& output_directory, CourseId course = CourseId::easy_ride);
}
