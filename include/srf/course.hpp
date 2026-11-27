#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <stdexcept>

namespace srf {
enum class CourseId : std::uint8_t { easy_ride, aqua_tunnel, sunset_valley, night_owl, kings_forest, sea_breeze, white_land, night_cruise, lake_side, big_ravine, sky_ramp, harbor_city, ember_mesa, kasumi_loop };
struct CourseDefinition {
    CourseId id;
    std::string_view slug, name;
    std::uint32_t scene_offset, coordinate_offset;
    std::uint16_t route_points;
};
// Original scene pointers CPU03:F4A5, confirmed against SRAM1092/1094.
inline constexpr std::array<CourseDefinition,12> courses{{
    {CourseId::easy_ride,"easy-ride","Easy Ride",0x28074,0x288b7,64},
    {CourseId::aqua_tunnel,"aqua-tunnel","Aqua Tunnel",0x28ab9,0x29679,82},
    {CourseId::sunset_valley,"sunset-valley","Sunset Valley",0x2990b,0x2a27a,80},
    {CourseId::night_owl,"night-owl","Night Owl",0x2a4fc,0x2afe3,84},
    {CourseId::kings_forest,"kings-forest","King's Forest",0x2b285,0x2bd18,79},
    {CourseId::sea_breeze,"sea-breeze","Sea Breeze",0x2bf92,0x2c8c8,66},
    {CourseId::white_land,"white-land","White Land",0x2cada,0x2d562,82},
    {CourseId::night_cruise,"night-cruise","Night Cruise",0x2d7f4,0x2e1c7,104},
    {CourseId::lake_side,"lake-side","Lake Side",0x68000,0x68a8f,77},
    {CourseId::big_ravine,"big-ravine","Big Ravine",0x68cf9,0x6985c,96},
    {CourseId::sky_ramp,"sky-ramp","Sky Ramp",0x69b5e,0x6a95b,121},
    {CourseId::harbor_city,"harbor-city","Harbor City",0x6ad25,0x6b9c7,117}
}};
inline constexpr CourseDefinition ember_mesa{CourseId::ember_mesa,"ember-mesa","Ember Mesa",0,0,64};
inline constexpr CourseDefinition kasumi_loop{CourseId::kasumi_loop,"kasumi-loop","Kasumi Inner Loop",0,0,112};
constexpr bool custom_course(CourseId course) {return course==ember_mesa.id || course==kasumi_loop.id;}
constexpr CourseId asset_course(CourseId course) {
    return custom_course(course) ? CourseId::easy_ride : course;
}
inline const CourseDefinition& course_definition(CourseId id) {
    if(id==kasumi_loop.id)return kasumi_loop;
    if(id==ember_mesa.id)return ember_mesa;
    const auto index=static_cast<unsigned>(id);
    if(index>=courses.size())throw std::invalid_argument("Unsupported original course");
    return courses[index];
}
inline CourseId parse_course(std::string_view slug) {
    if(slug==kasumi_loop.slug)return kasumi_loop.id;
    if(slug==ember_mesa.slug)return ember_mesa.id;
    for(const auto& course:courses)if(course.slug==slug)return course.id;
    throw std::invalid_argument("Supported courses: easy-ride, aqua-tunnel, sunset-valley, night-owl, kings-forest, sea-breeze, white-land, night-cruise, lake-side, big-ravine, sky-ramp, harbor-city");
}
}
