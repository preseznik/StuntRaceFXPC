#include "srf/scene.hpp"
#include <sstream>
#include <stdexcept>

namespace srf {
namespace {
// Verified against the original 65816 dispatcher at 08:8000 and its handlers.
// These are data command lengths, not a CPU execution table.
unsigned command_length(unsigned command) {
    switch(command) {
    case 0x02: return 22;
    case 0x04: case 0x06: return 1;
    case 0x08: return 2;
    case 0x0a: return 25;
    case 0x0c: return 22;
    case 0x0e: return 26;
    case 0x10: return 24;
    case 0x12: return 9;
    case 0x14: case 0x1a: return 3;
    case 0x16: case 0x18: return 4;
    default: throw std::runtime_error("Unsupported original scene command");
    }
}
int signed_word(const Rom& rom, std::size_t p) {
    const auto value = rom.u16(p);
    return value >= 32768 ? int(value) - 65536 : int(value);
}
Vertex position_at(const Rom& rom, std::size_t p) {
    return {signed_word(rom,p),signed_word(rom,p+2),signed_word(rom,p+4)};
}
void xyz(std::ostream& out, Vertex position) {
    out << position.x << ',' << position.y << ',' << position.z;
}
}

Scene extract_scene(const Rom& rom, std::uint32_t start_offset) {
    Scene scene{start_offset,0,0,0,{},{}};
    std::size_t p = scene.start_offset;
    std::uint8_t orientation = 0;
    bool ended = false;
    for(unsigned i=0; i<2048; ++i) {
        if(p >= scene.start_offset + 32768) throw std::runtime_error("Excessive scene command stream");
        const auto op = rom.slice(p,1)[0];
        const auto length = command_length(op);
        const auto command = rom.slice(p,length);
        ++scene.command_count;
        if(op==4) { ++p; ended=true; break; }
        if(op==8) orientation=command[1];
        if(op==0x12 && command[1]>=1 && command[1]<=4)
            scene.grid[command[1]-1]=GridSlot{position_at(rom,p+2),command[8]};
        if(op==0x14)scene.surface_flags|=rom.u16(p+1);
        if(op==2 || op==10 || op==12 || op==14 || op==16) {
            const auto descriptor = rom.u16(p+9);
            // Zero descriptors use original template/callback logic that is still
            // unresolved. Preserve these records without inventing a model pointer.
            std::uint32_t mesh_offset=0xffffffffu;
            std::uint16_t scale=0;
            if(descriptor) {
                if(descriptor<0x8000) throw std::runtime_error("Unexpected scene model descriptor bank");
                const std::size_t descriptor_offset=descriptor & 0x7fff;
                const auto model_address=rom.u16(descriptor_offset);
                const auto model_bank=rom.slice(descriptor_offset+2,1)[0];
                mesh_offset=static_cast<std::uint32_t>(Rom::lorom_offset((std::uint32_t(model_bank)<<16)|model_address));
                scale=rom.u16(descriptor_offset+7);
            }
            scene.objects.push_back({static_cast<std::uint32_t>(p),op,position_at(rom,p+1),descriptor,
                mesh_offset,orientation,scale,rom.u16(p+7),
                std::uint32_t(command[11])|(std::uint32_t(command[12])<<8)|(std::uint32_t(command[13])<<16)});
        }
        p+=length;
    }
    if(!ended) throw std::runtime_error("Scene did not reach its coordinate list");
    scene.coordinate_offset=static_cast<std::uint32_t>(p);
    const auto count=rom.u16(p); p+=2;
    // Stunt arenas such as Rock Field have an explicit zero-length AI route.
    if(count>1024) throw std::runtime_error("Invalid course coordinate count");
    rom.slice(p,std::size_t(count)*8);
    for(unsigned i=0; i<count; ++i,p+=8)
        scene.points.push_back({static_cast<std::uint32_t>(p),position_at(rom,p),rom.u16(p+6)});
    scene.end_offset=static_cast<std::uint32_t>(p);
    return scene;
}

Scene extract_easy_ride(const Rom& rom) {
    return extract_course_scene(rom,CourseId::easy_ride);
}
Scene extract_course_scene(const Rom& rom, CourseId id) {
    const auto& course=course_definition(id);
    auto scene=extract_scene(rom,course.scene_offset);
    if(scene.coordinate_offset!=course.coordinate_offset || scene.points.size()!=course.route_points)
        throw std::runtime_error("Course scene did not match its verified route");
    return scene;
}

std::string scene_json(const Scene& scene, std::string_view name) {
    std::ostringstream out;
    out << "{\n  \"schema_version\":1,\n  \"name\":" << json_string(std::string(name)) << ",\n  \"rom_sha256\":\"" << supported_sha256
        << "\",\n  \"source_range\":[" << scene.start_offset << ',' << scene.end_offset
        << "],\n  \"coordinate_offset\":" << scene.coordinate_offset << ",\n  \"command_count\":" << scene.command_count
        << ",\n  \"objects\":[\n";
    bool first=true;
    for(const auto& object:scene.objects) {
        if(!first) out << ",\n";
        first=false;
        out << "    {\"command_offset\":" << object.command_offset << ",\"opcode\":" << unsigned(object.opcode)
            << ",\"position_xyz\":["; xyz(out,object.position);
        out << "],\"descriptor_address\":" << object.descriptor_address << ",\"mesh_rom_offset\":" << object.mesh_rom_offset
            << ",\"orientation_byte\":" << unsigned(object.orientation) << ",\"descriptor_word_at_7\":" << object.scale_word
            << ",\"template_word\":" << object.template_word << ",\"callback_address\":" << object.callback_address << '}';
    }
    out << "\n  ],\n  \"coordinate_records\":[\n";
    first=true;
    for(const auto& point:scene.points) {
        if(!first) out << ",\n";
        first=false;
        out << "    {\"file_offset\":" << point.rom_offset << ",\"position_xyz\":["; xyz(out,point.position);
        out << "],\"unknown_word\":" << point.unknown_word << '}';
    }
    out << "\n  ],\n  \"native_race\":false,\n  \"complete_track_render\":false,\n"
        << "  \"field_roles\":{\"template_word\":\"Original visual descriptor\",\"descriptor_address\":\"Separate GSU descriptor; gameplay role unresolved\","
        << "\"mesh_rom_offset\":\"Resolved from descriptor_address, not the visual descriptor\"},\n"
        << "  \"unresolved\":[\"Width-aware visual geometry\",\"Runtime transform parity\",\"Material palette\",\"Collision and checkpoints\",\"Coordinate-list gameplay role\"]\n}\n";
    return out.str();
}

std::string scene_header(const Scene& scene) {
    std::ostringstream out;
    out << "#pragma once\n#include <array>\n#include \"srf/scene.hpp\"\nnamespace srf::generated {\n"
        << "inline constexpr std::array<SceneObject," << scene.objects.size() << "> easy_ride_objects = {{\n";
    for(const auto& object:scene.objects) {
        out << '{' << object.command_offset << ',' << unsigned(object.opcode) << ",{ "; xyz(out,object.position);
        out << "}," << object.descriptor_address << ',' << object.mesh_rom_offset << ',' << unsigned(object.orientation)
            << ',' << object.scale_word << ',' << object.template_word << ',' << object.callback_address << "},\n";
    }
    out << "}};\ninline constexpr std::array<ScenePoint," << scene.points.size() << "> easy_ride_points = {{\n";
    for(const auto& point:scene.points) {
        out << '{' << point.rom_offset << ",{ "; xyz(out,point.position);
        out << "}," << point.unknown_word << "},\n";
    }
    out << "}};\n}\n";
    return out.str();
}
}
