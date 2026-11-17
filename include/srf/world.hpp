#pragma once

#include "srf/scene.hpp"

namespace srf {
// Original visual descriptor geometry. Byte coordinates have already received
// the descriptor's power-of-two scale; word coordinates remain unscaled.
// Two-index primitives are original line records, not degenerate triangles.
struct WorldModel {
    std::uint16_t descriptor_address{};
    std::uint32_t vertex_rom_offset{}, face_rom_offset{};
    std::uint8_t scale_exponent{}, control_opcode{};
    std::vector<std::vector<Vertex>> frames;
    std::vector<Polygon> primitives;
};
struct WorldInstance {
    std::uint32_t command_offset{};
    std::uint16_t visual_descriptor{};
    std::uint8_t yaw{}, opcode{};
    Vertex position{};
    std::uint8_t sign_phase{};
};
// CPU-controlled scenery only. Kept separate from the immutable ROM placement
// and every vehicle/collision record; rendering never advances this state.
struct SceneryPose {
    std::uint32_t command_offset{};
    std::uint16_t visual_descriptor{};
    Vertex position{};
    std::uint8_t yaw{}, pitch{}, roll{};
    std::uint16_t frame{};
    bool visible=true;
    bool operator==(const SceneryPose&) const = default;
};
struct World {
    std::vector<WorldModel> models;
    std::vector<WorldInstance> instances;
    std::vector<ScenePoint> route_points;
};
struct WorldMaterials {
    // 0: unresolved textured/special material, 1: flat, 2: original shade ramp.
    std::array<std::uint8_t,256> kinds{};
    std::array<std::uint16_t,256> source_words{}, palette_rgb555{};
    std::array<std::array<std::uint8_t,16>,256> palette_indices{};
    std::array<std::array<std::uint16_t,16>,256> colors_rgb555{};
};
struct WorldSprite {
    std::uint16_t material_table{}, phase{}, width{}, height{};
    std::uint32_t source_address{};
    // RGBA8 in memory byte order, alpha zero for the original transparent index.
    std::vector<std::uint32_t> pixels;
    // Original per-corner texture coordinates from ROM03:A079 metadata.
    std::array<std::array<std::uint8_t,2>,4> texcoords{};
    // Palette lookup before fog and weather. Present in SRFSPRT3 packages.
    std::vector<std::uint8_t> palette_indices{};
};

WorldModel extract_world_model(const Rom& rom, std::uint16_t descriptor);
World extract_world(const Rom& rom);
World extract_world(const Rom& rom, const Scene& scene);
using WorldPalette = std::array<std::uint16_t,256>;
WorldPalette extract_world_palette(const Rom& rom, CourseId course, bool dusk = false);
WorldMaterials extract_world_materials(const Rom& rom, CourseId course = CourseId::easy_ride);
std::vector<WorldSprite> extract_vehicle_sprites(const Rom& rom, CourseId course = CourseId::easy_ride,
                                                bool dusk = false, bool include_loading_car = false);
std::vector<WorldSprite> extract_easy_ride_demo_sprites(const Rom& rom);
std::vector<std::uint8_t> encode_world_sprites(const std::vector<WorldSprite>& sprites);
std::vector<WorldSprite> decode_world_sprites(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_world(const World& world);
World decode_world(std::span<const std::uint8_t> bytes);
void generate_world_assets(const Rom& rom, const std::filesystem::path& output_directory, CourseId course = CourseId::easy_ride);
// Original static yaw convention, source-derived from GSU matrix construction.
std::array<double,3> world_position(const Vertex& local, const WorldInstance& instance);
std::array<double,3> scenery_position(const Vertex& local, const SceneryPose& pose);
}
