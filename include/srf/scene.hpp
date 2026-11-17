#pragma once

#include "srf/mesh.hpp"
#include "srf/course.hpp"

namespace srf {
// Original script values. The template field feeds the original render list;
// descriptor_address is a separate link whose role is not yet established.
// mesh_rom_offset resolves that separate link, not the visual layout.
// The raw word at descriptor+7 is preserved without applying it as a scale.
struct SceneObject {
    std::uint32_t command_offset;
    std::uint8_t opcode;
    Vertex position;
    std::uint16_t descriptor_address;
    std::uint32_t mesh_rom_offset;
    std::uint8_t orientation;
    std::uint16_t scale_word;
    std::uint16_t template_word;
    std::uint32_t callback_address;
    bool operator==(const SceneObject&) const = default;
};
struct ScenePoint {
    std::uint32_t rom_offset;
    Vertex position;
    std::uint16_t unknown_word;
    bool operator==(const ScenePoint&) const = default;
};
struct Scene {
    std::uint32_t start_offset, coordinate_offset, end_offset;
    std::size_t command_count;
    std::vector<SceneObject> objects;
    std::vector<ScenePoint> points;
};

Scene extract_easy_ride(const Rom& rom);
Scene extract_scene(const Rom& rom, std::uint32_t start_offset);
Scene extract_course_scene(const Rom& rom, CourseId course);
std::string scene_json(const Scene& scene, std::string_view name = "Easy Ride");
std::string scene_header(const Scene& scene);
}
