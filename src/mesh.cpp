#include "srf/mesh.hpp"
#include "srf/scene.hpp"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace srf {
namespace {
constexpr std::size_t max_package_bytes = 128U * 1024U * 1024U;
constexpr std::array<std::uint8_t, 8> magic = {'S','R','F','M','E','S','H','1'};
using Bytes = std::span<const std::uint8_t>;
struct InvalidMesh : std::runtime_error { using std::runtime_error::runtime_error; };

void require(bool condition, const char* message) {
    if (!condition) throw InvalidMesh(message);
}
void need(Bytes bytes, std::size_t p, std::size_t count, std::size_t start) {
    require(p >= start && p <= bytes.size() && p - start <= 16384 &&
            count <= bytes.size() - p && count <= 16384 - (p - start), "Truncated or excessive mesh");
}
int signed_byte(std::uint8_t v) { return v >= 128 ? int(v) - 256 : int(v); }
int signed_word(Bytes bytes, std::size_t p) {
    const unsigned v = unsigned(bytes[p]) | (unsigned(bytes[p + 1]) << 8);
    return v >= 32768 ? int(v) - 65536 : int(v);
}
bool vertex_command(std::uint8_t op) { return op == 4 || op == 8 || op == 0x34 || op == 0x38; }

struct VertexStream { std::vector<Vertex> vertices; std::size_t end; unsigned frame_count; };
VertexStream vertices_at(Bytes bytes, std::size_t start, unsigned frame = 0) {
    VertexStream result{{}, 0, 1};
    std::set<std::size_t> visited;
    auto p = start;
    for (unsigned commands = 0; commands < 128; ++commands) {
        need(bytes, p, 1, start);
        require(visited.insert(p).second, "Cyclic vertex stream");
        const auto op = bytes[p++];
        if (vertex_command(op)) {
            need(bytes, p, 1, start);
            const auto count = bytes[p++];
            const bool mirror = op == 0x34 || op == 0x38;
            const unsigned width = (op == 8 || op == 0x34) ? 2 : 1;
            require(count > 0 && result.vertices.size() + count * (mirror ? 2 : 1) <= 256,
                    "Invalid vertex count");
            need(bytes, p, count * 3 * width, start);
            for (unsigned i = 0; i < count; ++i) {
                int xyz[3];
                for (auto& value : xyz) {
                    value = width == 1 ? signed_byte(bytes[p]) : signed_word(bytes, p);
                    p += width;
                }
                result.vertices.push_back({xyz[0], xyz[1], xyz[2]});
                if (mirror) result.vertices.push_back({-xyz[0], xyz[1], xyz[2]});
            }
        } else if (op == 0x0C) {
            require(result.vertices.size() >= 3, "Too few vertices");
            result.end = p;
            return result;
        } else if (op == 0x1C) {
            need(bytes, p, 1, start);
            const unsigned count = bytes[p++];
            require(count >= 1 && count <= 64 && frame < count, "Invalid animation frames");
            require(result.frame_count == 1 || result.frame_count == count, "Mixed animation counts");
            result.frame_count = count;
            need(bytes, p, count * 2, start);
            const auto word = p + 2 * frame;
            const auto target = static_cast<std::int64_t>(word) + 1 + signed_word(bytes, word);
            require(target >= static_cast<std::int64_t>(start), "Out-of-range vertex branch");
            p = static_cast<std::size_t>(target);
        } else if (op == 0x20) {
            need(bytes, p, 2, start);
            const auto target = static_cast<std::int64_t>(p) + 1 + signed_word(bytes, p);
            require(target >= static_cast<std::int64_t>(start), "Out-of-range vertex jump");
            p = static_cast<std::size_t>(target);
        } else throw InvalidMesh("Unsupported vertex command");
    }
    throw InvalidMesh("Vertex command limit exceeded");
}

double alignment(const std::vector<Vertex>& vertices, std::array<unsigned, 3> triangle,
                 const std::array<std::uint8_t, 6>& attrs) {
    const auto a = vertices[triangle[0]], b = vertices[triangle[1]], c = vertices[triangle[2]];
    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    const double x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx;
    const double nx = signed_byte(attrs[3]), ny = signed_byte(attrs[4]), nz = signed_byte(attrs[5]);
    const double length = std::sqrt(x*x + y*y + z*z), nlength = std::sqrt(nx*nx + ny*ny + nz*nz);
    require(nlength >= 110 && nlength <= 140, "Invalid stored normal");
    return length == 0 ? -1 : std::abs(x*nx + y*ny + z*nz) / (length * nlength);
}

std::pair<Mesh, std::size_t> mesh_at(Bytes bytes, std::size_t start) {
    auto first = vertices_at(bytes, start);
    Mesh mesh{static_cast<std::uint32_t>(start), {first.vertices}, {}};
    auto p = first.end;
    need(bytes, p, 2, start);
    require(bytes[p] == 0x30 && bytes[p + 1] > 0, "Missing planes");
    const unsigned count = bytes[p + 1];
    p += 2;
    need(bytes, p, count * 3 + 1, start);
    std::vector<std::array<unsigned, 3>> planes;
    for (unsigned i = 0; i < count; ++i) {
        const std::array<unsigned, 3> triangle{bytes[p], bytes[p+1], bytes[p+2]};
        require(std::all_of(triangle.begin(), triangle.end(), [&](auto j) { return j < first.vertices.size(); }),
                "Plane vertex outside mesh");
        planes.push_back(triangle);
        p += 3;
    }
    require(bytes[p++] == 0x14, "Unsupported polygon control stream");
    unsigned comparable = 0, aligned = 0;
    bool ended = false;
    for (unsigned i = 0; i < 256; ++i) {
        need(bytes, p, 2, start);
        if (bytes[p] == 0xFE) {
            require(bytes[p + 1] == 0 && !mesh.polygons.empty(), "Invalid mesh terminator");
            p += 2; ended = true; break;
        }
        const unsigned n = bytes[p];
        require(n >= 3 && n <= 32, "Invalid polygon size");
        need(bytes, p, 7 + n, start);
        Polygon face;
        std::copy_n(bytes.begin() + p + 1, 6, face.attributes.begin());
        require(face.attributes[1] < planes.size(), "Polygon plane outside mesh");
        std::set<unsigned> distinct;
        for (unsigned j = 0; j < n; ++j) {
            const unsigned index = bytes[p + 7 + j];
            require(index < first.vertices.size(), "Polygon vertex outside mesh");
            face.indices.push_back(index); distinct.insert(index);
        }
        require(distinct.size() >= 3, "Degenerate polygon");
        const double agreement = alignment(first.vertices, planes[face.attributes[1]], face.attributes);
        if (agreement >= 0) { ++comparable; if (agreement >= 0.95) ++aligned; }
        mesh.polygons.push_back(std::move(face));
        p += 7 + n;
    }
    require(ended && comparable > 0 && double(aligned) / comparable >= 0.95, "Incomplete or invalid geometry");
    for (unsigned frame = 1; frame < first.frame_count; ++frame) {
        auto next = vertices_at(bytes, start, frame);
        require(next.end == first.end && next.vertices.size() == first.vertices.size(), "Animation topology changed");
        mesh.frames.push_back(std::move(next.vertices));
    }
    return {std::move(mesh), p};
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
struct Reader {
    Bytes data;
    std::size_t p = 0;
    Bytes take(std::size_t n) {
        require(p <= data.size() && n <= data.size() - p, "Truncated mesh package");
        auto result = data.subspan(p, n); p += n; return result;
    }
    std::uint32_t u32() {
        auto b = take(4);
        return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
               (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
    }
};

void write_new(const std::filesystem::path& path, Bytes bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create output; existing files are never overwritten");
    DWORD written = 0;
    const bool success = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    if (!success) throw std::runtime_error("Failed to write generated assets");
}
void write_new(const std::filesystem::path& path, const std::string& value) {
    write_new(path, Bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}
}

std::vector<Mesh> extract_meshes(const Rom& rom) {
    const auto bytes = rom.bytes();
    std::vector<Mesh> result;
    for (std::size_t p = 0x50000; p < bytes.size();) {
        if (!vertex_command(bytes[p]) && bytes[p] != 0x1C) { ++p; continue; }
        try {
            auto [mesh, end] = mesh_at(bytes, p);
            const auto face = vertices_at(bytes, p).end;
            const auto vertex_address = static_cast<unsigned>((p & 0x7FFF) | 0x8000);
            const auto face_address = static_cast<unsigned>((face & 0x7FFF) | 0x8000);
            const std::array<std::uint8_t, 5> descriptor{
                static_cast<std::uint8_t>(vertex_address), static_cast<std::uint8_t>(vertex_address >> 8),
                static_cast<std::uint8_t>(p >> 15), static_cast<std::uint8_t>(face_address),
                static_cast<std::uint8_t>(face_address >> 8)};
            const auto table_end = bytes.begin() + 0x8000;
            require(std::search(bytes.begin(), table_end, descriptor.begin(), descriptor.end()) != table_end,
                    "No original model descriptor");
            result.push_back(std::move(mesh)); p = end;
        } catch (const InvalidMesh&) { ++p; }
    }
    return result;
}

std::vector<std::uint8_t> encode_meshes(const std::vector<Mesh>& meshes) {
    require(!meshes.empty() && meshes.size() <= 4096, "Invalid package model count");
    std::vector<std::uint8_t> bytes(magic.begin(), magic.end());
    append_u32(bytes, static_cast<std::uint32_t>(meshes.size()));
    for (const auto& mesh : meshes) {
        require(mesh.rom_offset < rom_size && !mesh.frames.empty() && mesh.frames.size() <= 64,
                "Invalid model metadata");
        const auto vertex_count = mesh.frames[0].size();
        require(vertex_count >= 3 && vertex_count <= 256 && !mesh.polygons.empty() && mesh.polygons.size() <= 256,
                "Invalid model dimensions");
        append_u32(bytes, mesh.rom_offset);
        append_u32(bytes, static_cast<std::uint32_t>(mesh.frames.size()));
        append_u32(bytes, static_cast<std::uint32_t>(vertex_count));
        append_u32(bytes, static_cast<std::uint32_t>(mesh.polygons.size()));
        for (const auto& frame : mesh.frames) {
            require(frame.size() == vertex_count, "Animation topology mismatch");
            for (const auto& v : frame) {
                for (const auto coordinate : {v.x, v.y, v.z}) {
                    require(coordinate >= -32768 && coordinate <= 32768, "Coordinate outside ROM format range");
                    append_u32(bytes, std::bit_cast<std::uint32_t>(coordinate));
                }
            }
        }
        for (const auto& face : mesh.polygons) {
            require(face.indices.size() >= 3 && face.indices.size() <= 32, "Invalid polygon dimensions");
            append_u32(bytes, static_cast<std::uint32_t>(face.indices.size()));
            bytes.insert(bytes.end(), face.attributes.begin(), face.attributes.end());
            for (const auto index : face.indices) {
                require(index < vertex_count, "Invalid polygon vertex");
                append_u32(bytes, index);
            }
        }
        require(bytes.size() <= max_package_bytes, "Mesh package exceeds 128 MiB");
    }
    return bytes;
}

std::vector<Mesh> decode_meshes(Bytes bytes) {
    require(bytes.size() <= max_package_bytes, "Mesh package exceeds 128 MiB");
    Reader reader{bytes};
    const auto header = reader.take(magic.size());
    require(std::equal(header.begin(), header.end(), magic.begin()), "Unsupported mesh package version");
    const auto model_count = reader.u32();
    require(model_count > 0 && model_count <= 4096, "Invalid package model count");
    std::vector<Mesh> meshes;
    for (std::uint32_t i = 0; i < model_count; ++i) {
        Mesh mesh;
        mesh.rom_offset = reader.u32();
        const auto frames = reader.u32(), vertices = reader.u32(), polygons = reader.u32();
        require(mesh.rom_offset < rom_size && frames >= 1 && frames <= 64 && vertices >= 3 && vertices <= 256 &&
                polygons >= 1 && polygons <= 256, "Invalid model dimensions");
        const std::size_t minimum_size = std::size_t(frames) * vertices * 12 + std::size_t(polygons) * 22;
        require(minimum_size <= bytes.size() - reader.p, "Truncated model payload");
        for (unsigned frame = 0; frame < frames; ++frame) {
            std::vector<Vertex> positions;
            for (unsigned vertex = 0; vertex < vertices; ++vertex) {
                Vertex v{std::bit_cast<std::int32_t>(reader.u32()), std::bit_cast<std::int32_t>(reader.u32()),
                         std::bit_cast<std::int32_t>(reader.u32())};
                for (const auto coordinate : {v.x, v.y, v.z})
                    require(coordinate >= -32768 && coordinate <= 32768, "Invalid ROM vertex coordinate");
                positions.push_back(v);
            }
            mesh.frames.push_back(std::move(positions));
        }
        for (unsigned face_index = 0; face_index < polygons; ++face_index) {
            const auto count = reader.u32();
            require(count >= 3 && count <= 32, "Invalid polygon dimensions");
            Polygon face;
            const auto attrs = reader.take(6);
            std::copy(attrs.begin(), attrs.end(), face.attributes.begin());
            for (unsigned index = 0; index < count; ++index) {
                const auto value = reader.u32();
                require(value < vertices, "Polygon references missing vertex");
                face.indices.push_back(value);
            }
            mesh.polygons.push_back(std::move(face));
        }
        meshes.push_back(std::move(mesh));
    }
    require(reader.p == bytes.size(), "Unexpected trailing mesh package data");
    return meshes;
}

void generate_assets(const Rom& rom, const std::filesystem::path& output_directory, CourseId course) {
    if (std::filesystem::exists(output_directory))
        require(std::filesystem::is_directory(output_directory) && std::filesystem::is_empty(output_directory),
                "Asset output directory must be empty; existing builds are never overwritten");
    else std::filesystem::create_directories(output_directory);
    const auto meshes = extract_meshes(rom);
    const auto scene = extract_course_scene(rom,course);
    const auto& definition = course_definition(course);
    const auto scene_report = scene_json(scene,definition.name);
    const auto scene_digest = sha256(Bytes(reinterpret_cast<const std::uint8_t*>(scene_report.data()), scene_report.size()));
    const auto bytes = encode_meshes(meshes);
    const auto digest = sha256(bytes);
    const auto decoded = decode_meshes(bytes);
    require(decoded.size() == meshes.size(), "Generated mesh package validation failed");
    std::ostringstream manifest;
    manifest << "{\n  \"schema_version\": 1,\n  \"rom_sha256\": \"" << supported_sha256
             << "\",\n  \"mesh_sha256\": \"" << digest << "\",\n  \"model_count\": " << meshes.size()
             << ",\n  \"scene_sha256\": \"" << scene_digest << "\",\n  \"scene_objects\": " << scene.objects.size()
             << ",\n  \"scene_coordinate_records\": " << scene.points.size()
             << ",\n  \"coordinate_system\": \"raw_rom_unscaled\",\n"
             << "  \"content_status\": \"structurally_validated_meshes\",\n"
             << "  \"material_mapping\": \"unknown\",\n  \"native_race\": false,\n  \"first_course_recovered\": false\n}\n";
    std::ostringstream header;
    header << "#pragma once\n#include <cstddef>\nnamespace srf::generated {\n"
           << "inline constexpr char rom_sha256[] = \"" << supported_sha256 << "\";\n"
           << "inline constexpr char mesh_sha256[] = \"" << digest << "\";\n"
           << "inline constexpr char scene_sha256[] = \"" << scene_digest << "\";\n"
           << "inline constexpr std::size_t model_count = " << meshes.size() << ";\n}\n";
    write_new(output_directory / "models.srfmesh", bytes);
    write_new(output_directory / (std::string(definition.slug)+"-scene.json"), scene_report);
    write_new(output_directory / "generated_scene.hpp", scene_header(scene));
    write_new(output_directory / "manifest.json", manifest.str());
    write_new(output_directory / "generated_manifest.hpp", header.str());
}
}
