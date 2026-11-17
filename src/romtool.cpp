#include "srf/rom.hpp"
#include "srf/random.hpp"
#include "srf/mesh.hpp"
#include "srf/world.hpp"
#include "srf/audio.hpp"
#include "srf/scene.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 4 || std::wstring(argv[2]) != L"--rom") {
            std::cerr << "Usage: srf_romtool verify --rom <file>\n"
                      << "       srf_romtool inspect --rom <file> --output <new-manifest.json>\n"
                      << "       srf_romtool generate --rom <file> --output <empty-directory>\n";
            return 2;
        }
        const std::wstring command = argv[1];
        if ((command != L"verify" || argc != 4) &&
            ((command != L"inspect" && command != L"generate") || argc != 6 || std::wstring(argv[4]) != L"--output"))
            throw std::runtime_error("Invalid command or arguments");
        const auto rom = srf::Rom::load(argv[3]);
        if (command == L"generate") {
            const std::filesystem::path root=argv[5];
            std::ostringstream header;
            header<<"#pragma once\n#include <array>\n#include <cstddef>\nnamespace srf::generated {\n"
                  <<"struct CoursePackage { const char *scene_sha256, *world_sha256, *sprite_sha256, *dusk_sprite_sha256; "
                  <<"std::size_t objects, points, explicit_meshes, models, instances; "
                  <<"std::array<unsigned short,256> day_palette,dusk_palette; };\n"
                  <<"inline constexpr std::array<CoursePackage,"<<srf::courses.size()<<"> course_packages{{\n";
            const auto meshes=srf::extract_meshes(rom);
            for(const auto& course:srf::courses) {
                const auto output=course.id==srf::CourseId::easy_ride?root:root/std::string(course.slug);
                srf::generate_assets(rom,output,course.id);
                srf::generate_world_assets(rom,output,course.id);
                srf::generate_audio_assets(rom,output,course.id);
                const auto scene=srf::extract_course_scene(rom,course.id);
                const auto world=srf::extract_world(rom,scene);
                const auto report=srf::scene_json(scene,course.name);
                const auto resolved=std::count_if(scene.objects.begin(),scene.objects.end(),[&](const auto& object) {
                    return std::any_of(meshes.begin(),meshes.end(),[&](const auto& mesh){return mesh.rom_offset==object.mesh_rom_offset;});
                });
                header<<"{\""<<srf::sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(report.data()),report.size()))
                      <<"\",\""<<srf::sha256(srf::encode_world(world))<<"\",\""
                      <<srf::sha256(srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course.id)))<<"\",\"";
                if(course.id==srf::CourseId::sunset_valley || course.id==srf::CourseId::night_owl)
                    header<<srf::sha256(srf::encode_world_sprites(srf::extract_vehicle_sprites(rom,course.id,true)));
                header<<"\","<<scene.objects.size()<<','<<scene.points.size()<<','<<resolved<<','<<world.models.size()<<','<<world.instances.size();
                for (bool dusk : {false, true}) {
                    header<<",{{";
                    for (auto color : srf::extract_world_palette(rom,course.id,dusk)) header<<color<<',';
                    header<<"}}";
                }
                header<<"},\n";
            }
            header<<"}};\n}\n";
            std::ofstream file(root/"generated_courses.hpp",std::ios::binary);
            if(!file || !(file<<header.str()))throw std::runtime_error("Cannot write course manifest");
            std::cout << "{\"generated\":true,\"native_race_ready\":true,\"courses\":"<<srf::courses.size()<<"}\n";
            return 0;
        }
        const auto manifest = rom.manifest();
        if (command == L"inspect") {
            const std::filesystem::path output = argv[5];
            if (std::filesystem::exists(output))
                throw std::runtime_error("Output exists; select a new manifest path");
            if (!output.parent_path().empty())
                std::filesystem::create_directories(output.parent_path());
            std::ofstream file(output, std::ios::binary);
            if (!file || !(file << manifest)) throw std::runtime_error("Cannot write manifest");
        }
        std::cout << manifest;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"error\":" << srf::json_string(error.what()) << "}\n";
        return 1;
    }
}
